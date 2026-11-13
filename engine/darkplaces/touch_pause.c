/*
Godnotic: who holds a local game's pause (see touch_pause.h).

A local game stops in three places, each with its own holder:
- host.paused: the engine's own, a one-player local game (maxclients 1) while
  key_dest is not the game or the console is down (cl_screen.c), or CSQC's
  setpause.
- sv.paused: the server's, set by the game code (QC setpause): with
  sv_autopause, a listen server pauses while every real client sends the chat
  button, which the engine sends while key_dest is not the game, the console
  is down or the window is not active (cl_input.c); or the `pause` command.
- cl.paused: the client's copy of sv.paused (svc_setpause).

The server reads a client's chat button only from a move it executes, and it
executes a move only if the move's time is later than the last one. While the
game is paused neither clock advances, so a move can be dropped and the server
keep the chat button it last read: the menu closes, the window regains focus,
and the game stays frozen under a live HUD. It is certain during the start
countdown, when the player is held still and the server applies buttons only
in physics, which a pause stops. TouchPause_Frame lifts such a pause
(lift_stale_pause).

The `pause` command (or a key bound to it) pauses a local game with nothing on
screen, and the game code never lifts it: SV_PausedTic only runs for a pause
the game code set. On a touchscreen that is a frozen game with no way out, so
TouchPause_Frame turns it into the game menu, whose Resume unpauses.

touch_pause_status prints each holder, why, and the last changes.
*/

#include "quakedef.h"
#include "touch_pause.h"

extern cvar_t vid_touchscreen;

#define TP_LOG 12

typedef struct tp_change_s
{
	double t;
	char what[96];
} tp_change_t;

static struct
{
	int last; // bits: 1 host, 2 server, 4 client, 8 chat button
	int last_kd;
	tp_change_t log[TP_LOG];
	int nlog;
	int frames, paused_frames;
	double lifted; // when a pause command's pause was last turned into the menu
	double chat_off_since; // paused, and the local chat button off since then
} tp;

static const char *keydest_name(int kd)
{
	switch (kd)
	{
	case key_game: return "game";
	case key_message: return "chat";
	case key_menu: return "menu";
	case key_menu_grabbed: return "menu (grabbed)";
	case key_console: return "console";
	case key_void: return "void";
	default: return "?";
	}
}

// The chat button bit cl_input.c sends.
static qbool chat_button(void)
{
	return key_dest != key_game || key_consoleactive || !vid_activewindow;
}

static qbool is_local(client_t *c)
{
	return c->active && c->netconnection && LHNETADDRESS_GetAddressType(&c->netconnection->peeraddress) == LHNETADDRESSTYPE_LOOP;
}

static int real_clients(int *chatting)
{
	prvm_prog_t *prog = SVVM_prog;
	int i, n = 0;
	*chatting = 0;
	if (!sv.active)
		return 0;
	for (i = 0; i < svs.maxclients; i++)
	{
		client_t *c = svs.clients + i;
		if (!c->active || !c->netconnection)
			continue; // bots have no connection
		n++;
		if (c->edict && PRVM_serveredictfloat(c->edict, buttonchat))
			(*chatting)++;
	}
	return n;
}

static void note(const char *fmt, ...)
{
	va_list args;
	tp_change_t *e = tp.log + (tp.nlog++ % TP_LOG);
	e->t = host.realtime;
	va_start(args, fmt);
	dpvsnprintf(e->what, sizeof(e->what), fmt, args);
	va_end(args);
}

// The game code pauses a listen server only while every real player sends
// the chat button (sv_autopause). If the local player has stopped sending it
// and the pause still holds after STALE_PAUSE seconds, lift it as setpause(0)
// does; should another player still be chatting, the game code pauses again
// on its next frame.
#define STALE_PAUSE 0.4
static qbool lift_stale_pause(qbool chat)
{
	int i;
	if (!sv.active || sv.paused != 1 || sv.pausedstart <= 0 || cls.state != ca_connected || cls.demoplayback || chat)
	{
		tp.chat_off_since = 0;
		return false;
	}
	for (i = 0; i < svs.maxclients; i++)
		if (is_local(svs.clients + i) && svs.clients[i].begun)
			break;
	if (i == svs.maxclients)
		return false;
	if (!tp.chat_off_since)
		tp.chat_off_since = host.realtime;
	if (host.realtime - tp.chat_off_since < STALE_PAUSE)
		return false;
	tp.chat_off_since = 0;
	sv.paused = 0;
	sv.pausedstart = 0;
	MSG_WriteByte(&sv.reliable_datagram, svc_setpause);
	MSG_WriteByte(&sv.reliable_datagram, sv.paused);
	return true;
}

// A pause from the `pause` command: the game code's own set sv.pausedstart.
static qbool manual_pause(void)
{
	return sv.active && sv.paused && sv.pausedstart <= 0;
}

void TouchPause_Frame(void)
{
	qbool chat = chat_button();
	int now;
	// Wait for the console to close if the command was typed there; once
	// only, until the queued commands have run.
	if (vid_touchscreen.integer && manual_pause() && cls.state == ca_connected && !cls.demoplayback
		&& key_dest == key_game && !key_consoleactive && host.realtime - tp.lifted > 1.0)
	{
		tp.lifted = host.realtime;
		note("the pause command: unpaused, game menu opened");
		Cbuf_AddText(cmd_local, "\npause\n");
		if (Cvar_VariableValue(&cvars_all, "menu_gamemenu", ~0) && Cvar_VariableValue(&cvars_all, "_menu_gamemenu_dialog_available", ~0))
			Cbuf_AddText(cmd_local, "menu_showgamemenudialog\n");
		else
			Cbuf_AddText(cmd_local, "togglemenu 1\n");
	}
	if (lift_stale_pause(chat))
		note("lifted a pause the server kept after the chat button went off");
	now = (host.paused ? 1 : 0) | (sv.active && sv.paused ? 2 : 0) | (cl.paused ? 4 : 0) | (chat ? 8 : 0);
	tp.frames++;
	if (now & 7)
		tp.paused_frames++;
	if (now != tp.last || (int)key_dest != tp.last_kd)
		note("host %d server %d client %d, chat button %d, keys to %s%s", now & 1, (now >> 1) & 1, (now >> 2) & 1, (now >> 3) & 1,
			keydest_name(key_dest), key_consoleactive ? ", console" : "");
	tp.last = now;
	tp.last_kd = key_dest;
}

// What this client's chat button stands for right now.
static const char *chat_holder(void)
{
	if (key_consoleactive)
		return "the console";
	if (key_dest == key_message)
		return "the chat line";
	if (key_dest != key_game)
		return "the menu";
	if (vid_hidden)
		return "the window is hidden";
	if (!vid_activewindow)
		return "the window has no focus";
	return NULL;
}

static void TouchPause_Status_f(cmd_state_t *cmd)
{
	const char *mine = chat_holder();
	int chatting = 0, n = real_clients(&chatting), i, first;
	Con_Printf("touch pause: host %d, server %d, client %d; local game %d (maxclients %d), server time %.2f, client time %.2f\n",
		host.paused, sv.active ? sv.paused : -1, cl.paused, cl.islocalgame, sv.active ? svs.maxclients : 0,
		sv.active ? sv.time : -1.0, cl.time);
	Con_Printf("touch pause: keys to %s, console %s%s, window %s, sending chat %d; server sees %d of %d real clients chatting (sv_autopause %s)\n",
		keydest_name(key_dest), (key_consoleactive & KEY_CONSOLEACTIVE_USER) ? "open" : "closed",
		(key_consoleactive & KEY_CONSOLEACTIVE_FORCED) ? " (forced)" : "",
		vid_activewindow ? "active" : (vid_hidden ? "hidden" : "unfocused"), chat_button(), chatting, n,
		Cvar_VariableString(&cvars_all, "sv_autopause", CF_SERVER));
	if (host.paused)
		Con_Printf("touch pause: held by the engine: %s\n", mine ? mine : "the game code's setpause");
	if (sv.active && sv.paused)
	{
		if (n && chatting == n)
			Con_Printf("touch pause: held by the server: every real player sends the chat button; this one because %s\n", mine ? mine : "of a move the server has not read yet");
		else
			Con_Printf("touch pause: held by the server: %s\n", manual_pause() ? "the pause command" : "the game code's setpause");
	}
	else if (!sv.active && cl.paused && cls.state == ca_connected)
		Con_Printf("touch pause: held by the remote server\n");
	if (!host.paused && !(sv.active && sv.paused) && !cl.paused)
		Con_Printf("touch pause: running\n");
	Con_Printf("touch pause: paused %d of the last %d frames\n", tp.paused_frames, tp.frames);
	first = tp.nlog > TP_LOG ? tp.nlog - TP_LOG : 0;
	for (i = first; i < tp.nlog; i++)
		Con_Printf("touch pause:   %.2f s ago: %s\n", host.realtime - tp.log[i % TP_LOG].t, tp.log[i % TP_LOG].what);
	tp.frames = tp.paused_frames = 0;
	tp.nlog = 0;
}

void TouchPause_Init(void)
{
	Cmd_AddCommand(CF_CLIENT, "touch_pause_status", TouchPause_Status_f, "print whether a local game is paused, what holds the pause (menu, console, chat, window, server) and the changes since the last call");
}
