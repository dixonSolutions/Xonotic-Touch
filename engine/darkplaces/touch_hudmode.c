/*
Godnotic: the in-match HUD mode, worked out once per frame and published
(touch_hudmode.h has the rules; docs/CONTROLS.md "HUD state" the contract).

Both touch HUDs, the console button and the native front ends used to guess
"is this a spectator, an intermission, a vote?" each in their own way, and
disagreed (the console button floated over an empty top bar at intermission).
Here it is decided in the one place that runs under every CSQC: the engine,
from the client state and the loaded CSQC's own globals (the port's and a
public server's are both Xonotic and name them the same).

Published, read-only (set every frame they change):
  _touch_hud_mode      off | playing | dead | spectating | following | out |
                       out_free | intermission | mapvote
  _touch_hud_target    the followed player's name ("" when not following)
  _touch_hud_flags     TOUCHMODE_F_* bits: 1 warmup, 2 ready wanted, 4 teams,
                       8 round under way, 16 carrying an objective
  _touch_hud_join      none | open | pending | queued | locked
  _touch_hud_controls  TOUCHCTL_* bits: the controls this mode has
  _touch_hud_actions   the MENU sheet's actions: "resume join:disabled
                       spectate:confirm team settings leave" (hidden ones left out)
  _touch_act_<id>      the same per action: 0 hidden, 1 enabled, 2 disabled,
                       3 enabled with a confirm
Commands:
  touch_action <id> [confirm]   runs one of those actions (resume, join,
                       spectate, team, settings, leave). spectate:confirm
                       needs "confirm"; without it the command only prints.
  touch_join_dialog    opens the join dialog (the menu's Team Selection:
                       auto-select, the teams, spectate). It also opens by
                       itself for an observer when a match starts
                       (touch_join_dialog_auto), and a game hosted here with
                       no team to pick joins straight away (touch_local_autojoin).
  touch_join <auto|red|blue|yellow|pink|spectate>
                       what the dialog's rows run: the stock join commands,
                       with the answer watched (_touch_hud_join).
  touch_hud_state      prints what the mode was derived from.
The port's CSQC adds what only it can read (touch_mode.qc):
  _touch_hint_round, _touch_hint_carry, _touch_hint_queued.
*/

#include "quakedef.h"
#include "csprogs.h"
#include "touch_ui.h"
#include "touch_hud.h"
#include "touch_hudmode.h"

extern qbool cl_touch_csqc_active;

static cvar_t pub_mode = {CF_CLIENT | CF_READONLY, "_touch_hud_mode", "off", "the in-match HUD mode (touch_hudmode.c); read-only"};
static cvar_t pub_target = {CF_CLIENT | CF_READONLY, "_touch_hud_target", "", "the followed player's name while following; read-only"};
static cvar_t pub_flags = {CF_CLIENT | CF_READONLY, "_touch_hud_flags", "0", "TOUCHMODE_F_* bits: 1 warmup, 2 ready wanted, 4 teams, 8 round live, 16 carrying; read-only"};
static cvar_t pub_join = {CF_CLIENT | CF_READONLY, "_touch_hud_join", "none", "what joining would do: none, open, pending, queued, locked; read-only"};
static cvar_t pub_controls = {CF_CLIENT | CF_READONLY, "_touch_hud_controls", "0", "TOUCHCTL_* bits: the controls the HUD mode has; read-only"};
static cvar_t pub_actions = {CF_CLIENT | CF_READONLY, "_touch_hud_actions", "", "the in-match menu's actions with their state; read-only (docs/CONTROLS.md)"};
// One per action as well, for menus that switch a button on a number:
// 0 hidden, 1 enabled, 2 shown disabled, 3 enabled asking for a confirm.
static cvar_t pub_act[TOUCHACT_COUNT] = {
	{CF_CLIENT | CF_READONLY, "_touch_act_resume", "0", "in-match menu action state: 0 hidden, 1 enabled, 2 disabled, 3 confirm; read-only"},
	{CF_CLIENT | CF_READONLY, "_touch_act_join", "0", "in-match menu action state: 0 hidden, 1 enabled, 2 disabled, 3 confirm; read-only"},
	{CF_CLIENT | CF_READONLY, "_touch_act_spectate", "0", "in-match menu action state: 0 hidden, 1 enabled, 2 disabled, 3 confirm; read-only"},
	{CF_CLIENT | CF_READONLY, "_touch_act_team", "0", "in-match menu action state: 0 hidden, 1 enabled, 2 disabled, 3 confirm; read-only"},
	{CF_CLIENT | CF_READONLY, "_touch_act_settings", "0", "in-match menu action state: 0 hidden, 1 enabled, 2 disabled, 3 confirm; read-only"},
	{CF_CLIENT | CF_READONLY, "_touch_act_leave", "0", "in-match menu action state: 0 hidden, 1 enabled, 2 disabled, 3 confirm; read-only"},
};
static cvar_t touch_join_dialog_auto = {CF_CLIENT | CF_ARCHIVE, "touch_join_dialog_auto", "1", "Godnotic: open Xonotic's join dialog (auto-select, the teams, spectate) by itself for an observer when a match starts: 0 never (MENU > Join game opens it), 1 on a device with a touchscreen, 2 always"};
static cvar_t touch_local_autojoin = {CF_CLIENT | CF_ARCHIVE, "touch_local_autojoin", "1", "Godnotic: in a game hosted on this device (Singleplayer, a LAN game you host) with no team to pick, join as soon as the match is up instead of starting as an observer. Once per match; Spectate still works"};
static cvar_t hint_round = {CF_CLIENT, "_touch_hint_round", "0", "set by the port's CSQC: a round-based game with a round under way"};
static cvar_t hint_carry = {CF_CLIENT, "_touch_hint_carry", "0", "set by the port's CSQC: carrying a flag, key or ball"};
static cvar_t hint_queued = {CF_CLIENT, "_touch_hint_queued", "0", "set by the port's CSQC: the server queued our join"};

typedef struct mglobal_s { int ofs; int type; } mglobal_t;
static void *bound_globals;
static mglobal_t g_intermission, g_mv_active, g_warmup, g_ready_me, g_teamplay;

static touchmode_t current;
static char current_target[64];
static touchspec_join_t join_req;

static mglobal_t mglobal(prvm_prog_t *prog, const char *name)
{
	mglobal_t g = {-1, 0};
	mdef_t *d = PRVM_ED_FindGlobal(prog, name);
	if (d)
	{
		g.ofs = d->ofs;
		g.type = d->type & ~DEF_SAVEGLOBAL;
	}
	return g;
}

static float gvalue(mglobal_t g)
{
	prvm_prog_t *prog = CLVM_prog;
	if (g.ofs < 0 || !prog->loaded || g.ofs >= prog->numglobals)
		return 0;
	if (g.type == ev_float)
		return PRVM_GLOBALFIELDFLOAT(g.ofs);
	return (float)PRVM_GLOBALFIELDVALUE(g.ofs)->_int;
}

static void bind_globals(void)
{
	prvm_prog_t *prog = CLVM_prog;
	bound_globals = prog->loaded ? (void *)prog->globals.fp : NULL;
	g_intermission.ofs = g_mv_active.ofs = g_warmup.ofs = g_ready_me.ofs = g_teamplay.ofs = -1;
	if (!prog->loaded)
		return;
	g_intermission = mglobal(prog, "intermission");
	g_mv_active = mglobal(prog, "mv_active");
	g_warmup = mglobal(prog, "warmup_stage");
	g_ready_me = mglobal(prog, "ready_waiting_for_me");
	g_teamplay = mglobal(prog, "teamplay");
}

static void set_string(cvar_t *var, const char *value)
{
	if (strcmp(var->string, value))
		Cvar_SetQuick(var, value);
}

static void set_number(cvar_t *var, int value)
{
	if (var->integer != value || var->string[0] == 0)
		Cvar_SetValueQuick(var, value);
}

static qbool in_match(void)
{
	return CLVM_prog->loaded && cls.state == ca_connected && cls.signon == SIGNONS && !cls.demoplayback;
}

static void read_inputs(touchmode_in_t *in)
{
	double now = host.realtime;
	memset(in, 0, sizeof(*in));
	in->in_match = in_match();
	if (!in->in_match)
		return;
	if (bound_globals != (void *)CLVM_prog->globals.fp)
		bind_globals();
	in->intermission = cl.intermission || gvalue(g_intermission) != 0;
	in->mapvote = gvalue(g_mv_active) != 0
		// a CSQC whose vote state cannot be read: the pointer at intermission
		|| (g_mv_active.ofs < 0 && in->intermission && cl.csqc_wantsmousemove);
	in->spec_state = TouchUI_SpectatorState();
	in->health = cl.stats[STAT_HEALTH];
	in->warmup = gvalue(g_warmup) != 0;
	in->ready_for_me = gvalue(g_ready_me) != 0;
	in->teamplay = gvalue(g_teamplay) != 0;
	if (cl_touch_csqc_active)
	{
		in->round_live = hint_round.integer != 0;
		in->carrying = hint_carry.integer != 0;
		in->join_queued = hint_queued.integer != 0;
	}
	in->join_pending = TouchSpec_JoinPending(&join_req);
	in->join_refused = TouchSpec_JoinLocked(&join_req, now);
}

static void publish(void)
{
	char actions[256];
	int act;
	actions[0] = 0;
	for (act = 0; act < TOUCHACT_COUNT; act++)
	{
		int st = TouchAct_State(&current, act);
		set_number(&pub_act[act], st);
		if (st == TOUCHACT_HIDDEN)
			continue;
		if (actions[0])
			dp_strlcat(actions, " ", sizeof(actions));
		dp_strlcat(actions, TouchAct_Id(act), sizeof(actions));
		if (st == TOUCHACT_DISABLED)
			dp_strlcat(actions, ":disabled", sizeof(actions));
		else if (st == TOUCHACT_CONFIRM)
			dp_strlcat(actions, ":confirm", sizeof(actions));
	}
	set_string(&pub_mode, TouchMode_Name(current.mode));
	set_string(&pub_target, current_target);
	set_number(&pub_flags, current.flags);
	set_string(&pub_join, TouchMode_JoinName(current.join));
	set_number(&pub_controls, TouchMode_Controls(current.mode, current.flags));
	set_string(&pub_actions, actions);
}

static touchjoindlg_t join_dlg = {1, TOUCHJOINDLG_TRIES, 0, 0};
static char join_dlg_map[64];
static qbool join_dlg_welcome;   // the game's own Welcome dialog (Join! / Spectate) came up for this connection

static qbool screen_free(void)
{
	return key_dest == key_game && !(key_consoleactive & KEY_CONSOLEACTIVE_USER) && !cl.csqc_wantsmousemove
		&& !Cvar_VariableValue(&cvars_all, "_touch_owns_screen", ~0);
}

static qbool join_dialog_here(void)
{
	if (touch_join_dialog_auto.integer >= 2)
		return true;
	return touch_join_dialog_auto.integer == 1
		&& (vid_touchscreen.integer || vid_touchscreen_detected.integer || VID_SDL_HasTouchDevices());
}

// A join the server did not take (locked, full, spectate-only): said once,
// in words, instead of leaving the player wondering.
static void join_refused_notice(void)
{
	SCR_CenterPrint("The server did not let you in (locked or full).\nMENU > Join game tries again.");
}

void TouchHudMode_Update(void)
{
	touchmode_in_t in;
	int last_mode = current.mode;
	read_inputs(&in);
	current = TouchMode_Derive(&in);
	current_target[0] = 0;
	if (current.mode == TOUCHMODE_FOLLOWING || current.mode == TOUCHMODE_OUT)
	{
		int v = cl.viewentity;
		if (cl.scores && v >= 1 && v <= cl.maxclients)
			dp_strlcpy(current_target, cl.scores[v - 1].name, sizeof(current_target));
	}
	{
		qbool was_locked = TouchSpec_JoinLocked(&join_req, host.realtime);
		TouchSpec_JoinFrame(&join_req, in.in_match && !TouchMode_IsMatchEnd(current.mode) ? in.spec_state : TOUCHSPEC_NONE, host.realtime);
		if (!was_locked && TouchSpec_JoinLocked(&join_req, host.realtime))
			join_refused_notice();
	}
	// The join dialog, owed once per match to a player who starts it as an
	// observer; a game hosted here with no team to pick goes straight in.
	// (Nothing calls this between maps, so a new map is seen by its name.)
	if (in.in_match && strcmp(join_dlg_map, cl.worldbasename))
	{
		dp_strlcpy(join_dlg_map, cl.worldbasename, sizeof(join_dlg_map));
		TouchJoinDlg_Frame(&join_dlg, TOUCHMODE_OFF, 0, 0, 0, 0, 0, host.realtime);
		if (join_dlg_welcome)
			TouchJoinDlg_Answered(&join_dlg);
		join_dlg_welcome = false;
	}
	else if (!in.in_match)
	{
		join_dlg_map[0] = 0;
		if (cls.state != ca_connected)
			join_dlg_welcome = false;
	}
	switch (TouchJoinDlg_Frame(&join_dlg, current.mode, current.flags,
		sv.active && touch_local_autojoin.integer, Cvar_VariableValue(&cvars_all, "g_campaign", ~0) != 0,
		screen_free(), join_dialog_here(), host.realtime))
	{
	case TOUCHJOINDLG_OPEN:
		Con_DPrintf("Touch HUD: observer at match start: the join dialog\n");
		Cbuf_AddText(cmd_local, "touch_join_dialog\n");
		break;
	case TOUCHJOINDLG_JOIN:
		Con_DPrintf("Touch HUD: a game hosted here: joining\n");
		Cbuf_AddText(cmd_local, "cmd join\n");
		break;
	default:
		break;
	}
	if (current.mode != last_mode)
		Con_DPrintf("Touch HUD mode: %s -> %s (join %s)\n", TouchMode_Name(last_mode), TouchMode_Name(current.mode), TouchMode_JoinName(current.join));
	publish();
}

const touchmode_t *TouchHudMode_Current(void)
{
	return &current;
}

const char *TouchHudMode_Target(void)
{
	return current_target;
}

// touch_join_dialog: Xonotic's join dialog. "directmenu TeamSelect" is what
// the stock team_selection_show alias runs; the menu shows it finger-sized
// on a touchscreen (dialog_touch_join.qc), or as the UI bridge's "join"
// popup when a bridge renderer draws the menu.
static void TouchJoinDialog_f(cmd_state_t *cmd)
{
	TouchHudMode_Update();
	if (current.mode == TOUCHMODE_OFF || TouchMode_IsMatchEnd(current.mode))
	{
		Con_Printf("touch_join_dialog: not in a match\n");
		return;
	}
	Cbuf_AddText(cmd_local, "menu_cmd directmenu TeamSelect\n");
}

// touch_join_answered: the menu showed the game's own Welcome dialog, which
// asks Join! or Spectate itself (newer servers' game code opens it on
// connect; 0.8.6 servers have none). The join dialog is then not owed.
static void TouchJoinAnswered_f(cmd_state_t *cmd)
{
	join_dlg_welcome = true;
	TouchJoinDlg_Answered(&join_dlg);
}

// touch_join <choice>: a row of the dialog.
static void TouchJoin_f(cmd_state_t *cmd)
{
	const char *choice = Cmd_Argv(cmd, 1);
	const char *text;
	TouchHudMode_Update();
	text = TouchJoinDlg_Command(choice, (current.flags & TOUCHMODE_F_TEAMS) != 0);
	if (!text[0] || current.mode == TOUCHMODE_OFF || TouchMode_IsMatchEnd(current.mode))
	{
		Con_Printf("touch_join <auto|red|blue|yellow|pink|spectate>, in a match; now: %s\n", pub_mode.string);
		return;
	}
	TouchJoinDlg_Answered(&join_dlg);
	if (!strcmp(choice, "spectate"))
	{
		// already watching: nothing to send, the dialog just closes
		if (TouchMode_IsSpectating(current.mode))
			return;
	}
	else if (TouchMode_IsSpectating(current.mode))
		TouchSpec_JoinSend(&join_req, host.realtime);
	Con_DPrintf("Touch HUD: join dialog: %s -> %s\n", choice, text);
	Cbuf_AddText(cmd_local, text);
	Cbuf_AddText(cmd_local, "\n");
}

void TouchHudMode_Reset(void)
{
	memset(&join_req, 0, sizeof(join_req));
}

static void TouchAction_f(cmd_state_t *cmd)
{
	const char *id = Cmd_Argv(cmd, 1);
	qbool confirmed = !strcmp(Cmd_Argv(cmd, 2), "confirm");
	int act;
	TouchHudMode_Update();
	for (act = 0; act < TOUCHACT_COUNT; act++)
		if (!strcmp(id, TouchAct_Id(act)))
			break;
	if (act == TOUCHACT_COUNT)
	{
		Con_Printf("touch_action <resume|join|spectate|team|settings|leave> [confirm]; now: %s\n", pub_actions.string);
		return;
	}
	switch (TouchAct_State(&current, act))
	{
	case TOUCHACT_HIDDEN:
	case TOUCHACT_DISABLED:
		Con_Printf("touch_action %s: not available now (%s)\n", id, pub_actions.string);
		return;
	case TOUCHACT_CONFIRM:
		if (!confirmed)
		{
			Con_Printf("touch_action %s: this would forfeit the round or the objective you carry; \"touch_action %s confirm\" to go ahead\n", id, id);
			return;
		}
		break;
	default:
		break;
	}
	if (act == TOUCHACT_SPECTATE)
	{
		TouchJoinDlg_Answered(&join_dlg);
		Cbuf_AddText(cmd_local, "togglemenu 0\n");
	}
	Cbuf_AddText(cmd_local, TouchAct_Command(&current, act));
	Cbuf_AddText(cmd_local, "\n");
}

// touch_hud_state: what the mode was derived from (to tell a wrong mode on
// a server apart from a wrong signal).
static void TouchHudState_f(cmd_state_t *cmd)
{
	int me = cl.realplayerentity, i;
	TouchHudMode_Update();
	Con_Printf("TOUCHHUD mode=%s join=%s flags=%d spec=%d me=%d playerentity=%d viewentity=%d maxclients=%d frags=%d colors=%d health=%d csqc=%d ours=%d\n",
		pub_mode.string, pub_join.string, current.flags, TouchUI_SpectatorState(), me, cl.playerentity, cl.viewentity, cl.maxclients,
		cl.scores && me >= 1 && me <= cl.maxclients ? cl.scores[me - 1].frags : 12345,
		cl.scores && me >= 1 && me <= cl.maxclients ? cl.scores[me - 1].colors : -1,
		cl.stats[STAT_HEALTH], CLVM_prog->loaded, cl_touch_csqc_active);
	// whether a menu (the join dialog, the game menu) has the screen
	Con_Printf("TOUCHMENU up=%d\n", key_dest == key_menu || key_dest == key_menu_grabbed);
	// what the controls are doing to the player, for the input tests
	// (scripts/test-touch-controls.sh): view angles, buttons, the frame's move
	{
		extern kbutton_t in_attack, in_jump;
		Con_Printf("TOUCHFEEL yaw=%.3f pitch=%.3f attack=%d jump=%d fwd=%.0f side=%.0f vz=%.0f onground=%d\n",
			cl.viewangles[YAW], cl.viewangles[PITCH], in_attack.state & 1, in_jump.state & 1,
			cl.cmd.forwardmove, cl.cmd.sidemove, cl.velocity[2], cl.onground);
	}
	if (Cmd_Argc(cmd) > 1 && !strcmp(Cmd_Argv(cmd, 1), "rects"))
	{
		TouchHUD_PrintRects();
		return;
	}
	if (Cmd_Argc(cmd) > 1 && cl.scores)
		for (i = 0; i < cl.maxclients; i++)
			if (cl.scores[i].name[0])
				Con_Printf("TOUCHHUD   slot %d frags=%d colors=%d name=%s\n", i + 1, cl.scores[i].frags, cl.scores[i].colors, cl.scores[i].name);
}

void TouchHudMode_Init(void)
{
	int act;
	Cvar_RegisterVariable(&pub_mode);
	Cvar_RegisterVariable(&pub_target);
	Cvar_RegisterVariable(&pub_flags);
	Cvar_RegisterVariable(&pub_join);
	Cvar_RegisterVariable(&pub_controls);
	Cvar_RegisterVariable(&pub_actions);
	for (act = 0; act < TOUCHACT_COUNT; act++)
		Cvar_RegisterVariable(&pub_act[act]);
	Cvar_RegisterVariable(&touch_join_dialog_auto);
	Cvar_RegisterVariable(&touch_local_autojoin);
	Cvar_RegisterVariable(&hint_round);
	Cvar_RegisterVariable(&hint_carry);
	Cvar_RegisterVariable(&hint_queued);
	Cmd_AddCommand(CF_CLIENT, "touch_hud_state", TouchHudState_f, "print what the touch HUD mode is derived from and what the controls are doing (any argument: every scoreboard slot too; \"rects\": every control on screen as a rect)");
	Cmd_AddCommand(CF_CLIENT, "touch_join_dialog", TouchJoinDialog_f, "open Xonotic's join dialog: auto-select, the teams the server offers, spectate");
	Cmd_AddCommand(CF_CLIENT, "touch_join_answered", TouchJoinAnswered_f, "the game's own Welcome dialog (Join! / Spectate) was shown: the join dialog is not owed for this match");
	Cmd_AddCommand(CF_CLIENT, "touch_join", TouchJoin_f, "what the join dialog's rows run: touch_join <auto|red|blue|yellow|pink|spectate>");
	Cmd_AddCommand(CF_CLIENT, "touch_action", TouchAction_f, "run an in-match menu action: resume, join, spectate, team, settings, leave [confirm] (_touch_hud_actions lists them)");
}
