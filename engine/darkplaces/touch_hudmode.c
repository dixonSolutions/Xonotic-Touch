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
Command:
  touch_action <id> [confirm]   runs one of those actions (resume, join,
                       spectate, team, settings, leave). spectate:confirm
                       needs "confirm"; without it the command only prints.
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
static cvar_t hint_round = {CF_CLIENT, "_touch_hint_round", "0", "set by the port's CSQC: a round-based game with a round under way"};
static cvar_t hint_carry = {CF_CLIENT, "_touch_hint_carry", "0", "set by the port's CSQC: carrying a flag, key or ball"};
static cvar_t hint_queued = {CF_CLIENT, "_touch_hint_queued", "0", "set by the port's CSQC: the server queued our join"};

typedef struct mglobal_s { int ofs; int type; } mglobal_t;
static void *bound_globals;
static mglobal_t g_intermission, g_mv_active, g_warmup, g_ready_me, g_teamplay;

static touchmode_t current;
static char current_target[64];
static touchspec_join_t join_req;
static qbool join_key_down;

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

// The +jump the join swipe asks for, held for TOUCHSPEC_JOIN_PRESS under its
// own key number so a keyboard's jump is never let go by it.
static void join_key(qbool down)
{
	if (down == join_key_down)
		return;
	join_key_down = down;
	Cbuf_AddText(cmd_local, down ? "+jump 50002\n" : "-jump 50002\n");
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
	join_key(TouchSpec_JoinFrame(&join_req, in.in_match && !TouchMode_IsMatchEnd(current.mode) ? in.spec_state : TOUCHSPEC_NONE, host.realtime) != 0);
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

// The join swipe, or a click on its handle. In a team game the stock team
// selection comes up (Auto and the teams); otherwise +jump, as the stock HUD's
// "press jump to join", which every server reads.
void TouchHudMode_RequestJoin(void)
{
	if (!TouchMode_IsSpectating(current.mode))
		return;
	if (current.flags & TOUCHMODE_F_TEAMS)
	{
		Con_DPrintf("Touch HUD: join -> team selection\n");
		Cbuf_AddText(cmd_local, "menu_showteamselect\n");
		return;
	}
	Con_DPrintf("Touch HUD: join (+jump)\n");
	TouchSpec_JoinSend(&join_req, host.realtime);
}

void TouchHudMode_Reset(void)
{
	join_key(false);
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
	if (act == TOUCHACT_JOIN)
	{
		// The menu is closed first: the join (or the team selection) is in the match.
		Cbuf_AddText(cmd_local, "togglemenu 0\n");
		TouchHudMode_RequestJoin();
		return;
	}
	if (act == TOUCHACT_SPECTATE)
		Cbuf_AddText(cmd_local, "togglemenu 0\n");
	Cbuf_AddText(cmd_local, TouchAct_Command(&current, act));
	Cbuf_AddText(cmd_local, "\n");
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
	Cvar_RegisterVariable(&hint_round);
	Cvar_RegisterVariable(&hint_carry);
	Cvar_RegisterVariable(&hint_queued);
	Cmd_AddCommand(CF_CLIENT, "touch_action", TouchAction_f, "run an in-match menu action: resume, join, spectate, team, settings, leave [confirm] (_touch_hud_actions lists them)");
}
