/*
Godnotic: what the in-match touch HUD is showing, as pure functions both HUD
copies, the native front ends and the tests share.

  - the mode the HUD is in (playing, dead, free-fly, following, out of the
    round, intermission, the next-map vote), derived once per frame by the
    engine (touch_hudmode.c) from the client state and the loaded CSQC's own
    globals, and published as read-only cvars (_touch_hud_*). The port's CSQC
    reads those instead of guessing again; touch_shellbtn.c can too.
  - the controls each mode has (TouchMode_Controls): one layout model, a
    control set per mode.
  - the MENU sheet's action list (TouchMode_Actions): Resume, Join, Move to
    spectators, Change team, Settings, Leave, each available, disabled or
    asking for a confirm, for the in-match menu and the native pause menus.
  - joining (TouchJoinDlg_*): Xonotic's own join dialog (the menu's Team
    Selection: auto-select, the teams, spectate), which comes up by itself
    for an observer when a match starts and from the MENU sheet after that.
    There is no join gesture and no join button on the HUD.

No engine headers: scripts/test-touch-hudmode.sh compiles this alone. The
QuakeC side mirrors the numbers in qcsrc/client/touch_mode.qh.
*/
#ifndef TOUCH_HUDMODE_H
#define TOUCH_HUDMODE_H

#include <string.h>

#include "touch_spectate.h"

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------
#define TOUCHMODE_OFF          0   // not in a match (menu, loading, demo)
#define TOUCHMODE_PLAYING      1
#define TOUCHMODE_DEAD         2   // waiting to respawn
#define TOUCHMODE_OBSERVING    3   // spectator, free-fly
#define TOUCHMODE_FOLLOWING    4   // spectator, watching a player
#define TOUCHMODE_OUT          5   // in the game, out of the round, watching
#define TOUCHMODE_OUT_FREE     6   // in the game, out of the round, free-fly
#define TOUCHMODE_INTERMISSION 7   // end-of-match scoreboard
#define TOUCHMODE_MAPVOTE      8   // the next map / gametype vote
#define TOUCHMODE_COUNT        9

// Flags beside the mode
#define TOUCHMODE_F_WARMUP 1    // warmup stage
#define TOUCHMODE_F_READY  2    // warmup waits for this player's "ready"
#define TOUCHMODE_F_TEAMS  4    // a team game: joining picks a team
#define TOUCHMODE_F_ROUND  8    // a round-based game with a round under way
#define TOUCHMODE_F_CARRY  16   // carrying an objective (flag, key, ball)

// What joining would do now (spectators only)
#define TOUCHJOIN_NONE    0   // not offered
#define TOUCHJOIN_OPEN    1
#define TOUCHJOIN_PENDING 2   // sent, waiting for the server
#define TOUCHJOIN_QUEUED  3   // the server queued us for a slot
#define TOUCHJOIN_LOCKED  4   // the server did not take it (locked, full, spectate-only)

typedef struct touchmode_in_s
{
	int in_match;      // connected, world in, game code loaded, not a demo
	int intermission;  // end-of-match
	int mapvote;       // the next map / gametype vote is up
	int spec_state;    // TOUCHSPEC_* (touch_spectate.h)
	int health;        // STAT_HEALTH
	int warmup;
	int ready_for_me;  // the warmup waits for this player's ready
	int teamplay;
	int round_live;    // a round-based game, round under way
	int carrying;      // holding a flag, key or ball
	int join_pending;  // a join was sent and not answered yet
	int join_queued;   // the server queued the join
	int join_refused;  // a join was refused a short while ago
} touchmode_in_t;

typedef struct touchmode_s
{
	int mode;
	int flags;
	int join;
} touchmode_t;

static inline int TouchMode_IsSpectating(int mode)
{
	return mode == TOUCHMODE_OBSERVING || mode == TOUCHMODE_FOLLOWING;
}

static inline int TouchMode_IsOutOfRound(int mode)
{
	return mode == TOUCHMODE_OUT || mode == TOUCHMODE_OUT_FREE;
}

static inline int TouchMode_IsMatchEnd(int mode)
{
	return mode == TOUCHMODE_INTERMISSION || mode == TOUCHMODE_MAPVOTE;
}

static inline int TouchMode_FreeFly(int mode)
{
	return mode == TOUCHMODE_OBSERVING || mode == TOUCHMODE_OUT_FREE;
}

static inline touchmode_t TouchMode_Derive(const touchmode_in_t *in)
{
	touchmode_t m;
	m.mode = TOUCHMODE_OFF;
	m.flags = 0;
	m.join = TOUCHJOIN_NONE;
	if (!in->in_match)
		return m;
	if (in->mapvote)
		m.mode = TOUCHMODE_MAPVOTE;
	else if (in->intermission)
		m.mode = TOUCHMODE_INTERMISSION;
	else switch (in->spec_state)
	{
	case TOUCHSPEC_OBSERVING: m.mode = TOUCHMODE_OBSERVING; break;
	case TOUCHSPEC_FOLLOWING: m.mode = TOUCHMODE_FOLLOWING; break;
	case TOUCHSPEC_OUT: m.mode = TOUCHMODE_OUT; break;
	case TOUCHSPEC_OUT_FREE: m.mode = TOUCHMODE_OUT_FREE; break;
	default: m.mode = in->health > 0 ? TOUCHMODE_PLAYING : TOUCHMODE_DEAD; break;
	}
	if (in->warmup && !TouchMode_IsMatchEnd(m.mode))
		m.flags |= TOUCHMODE_F_WARMUP;
	if (in->ready_for_me && (m.mode == TOUCHMODE_PLAYING || m.mode == TOUCHMODE_DEAD))
		m.flags |= TOUCHMODE_F_READY;
	if (in->teamplay)
		m.flags |= TOUCHMODE_F_TEAMS;
	if (in->round_live && (m.mode == TOUCHMODE_PLAYING || m.mode == TOUCHMODE_DEAD))
		m.flags |= TOUCHMODE_F_ROUND;
	if (in->carrying && m.mode == TOUCHMODE_PLAYING)
		m.flags |= TOUCHMODE_F_CARRY;
	if (TouchMode_IsSpectating(m.mode))
		m.join = in->join_queued ? TOUCHJOIN_QUEUED
			: in->join_pending ? TOUCHJOIN_PENDING
			: in->join_refused ? TOUCHJOIN_LOCKED
			: TOUCHJOIN_OPEN;
	return m;
}

static inline const char *TouchMode_Name(int mode)
{
	static const char *names[TOUCHMODE_COUNT] = {
		"off", "playing", "dead", "spectating", "following", "out", "out_free", "intermission", "mapvote"
	};
	return mode >= 0 && mode < TOUCHMODE_COUNT ? names[mode] : "off";
}

static inline const char *TouchMode_JoinName(int join)
{
	switch (join)
	{
	case TOUCHJOIN_OPEN: return "open";
	case TOUCHJOIN_PENDING: return "pending";
	case TOUCHJOIN_QUEUED: return "queued";
	case TOUCHJOIN_LOCKED: return "locked";
	default: return "none";
	}
}

// ---------------------------------------------------------------------------
// Controls per mode. A control also needs its own layout switch
// (touch_<name>_visible) and, for some, a weapon that has it (ALT, ZOOM,
// HOOK): the mode only says whether it may be there at all.
// ---------------------------------------------------------------------------
#define TOUCHCTL_MENU     (1 << 0)
#define TOUCHCTL_CHAT     (1 << 1)
#define TOUCHCTL_SCORE    (1 << 2)
#define TOUCHCTL_MOVE     (1 << 3)
#define TOUCHCTL_LOOK     (1 << 4)
#define TOUCHCTL_FIRE     (1 << 5)
#define TOUCHCTL_ALT      (1 << 6)
#define TOUCHCTL_HOP      (1 << 7)
#define TOUCHCTL_DUCK     (1 << 8)
#define TOUCHCTL_WEAPONS  (1 << 9)    // the strip, its grip, WEP
#define TOUCHCTL_ZOOM     (1 << 10)
#define TOUCHCTL_HOOK     (1 << 11)
#define TOUCHCTL_NADE     (1 << 12)
#define TOUCHCTL_TURN     (1 << 13)
#define TOUCHCTL_RELOAD   (1 << 14)
#define TOUCHCTL_DODGE    (1 << 15)
#define TOUCHCTL_VITALS   (1 << 16)
#define TOUCHCTL_SPECBAR  (1 << 17)   // previous / follow-or-free / next
#define TOUCHCTL_FLY      (1 << 18)   // up / down while free-flying
#define TOUCHCTL_JOIN     (1 << 19)   // unused: joining is the join dialog (the bit stays reserved)
#define TOUCHCTL_TOSPEC   (1 << 20)   // "move to spectators", when placed
#define TOUCHCTL_READY    (1 << 21)
#define TOUCHCTL_VOTECHIP (1 << 22)   // a vote called in the match
#define TOUCHCTL_RESPAWN  (1 << 23)   // HOP's place, as RESPAWN

#define TOUCHCTL_CHROME (TOUCHCTL_MENU | TOUCHCTL_CHAT | TOUCHCTL_SCORE | TOUCHCTL_VOTECHIP)
#define TOUCHCTL_PLAY (TOUCHCTL_MOVE | TOUCHCTL_LOOK | TOUCHCTL_FIRE | TOUCHCTL_ALT | TOUCHCTL_HOP | TOUCHCTL_DUCK \
	| TOUCHCTL_WEAPONS | TOUCHCTL_ZOOM | TOUCHCTL_HOOK | TOUCHCTL_NADE | TOUCHCTL_TURN | TOUCHCTL_RELOAD \
	| TOUCHCTL_DODGE | TOUCHCTL_VITALS | TOUCHCTL_TOSPEC)

static inline int TouchMode_Controls(int mode, int flags)
{
	int c;
	switch (mode)
	{
	case TOUCHMODE_PLAYING:
		c = TOUCHCTL_CHROME | TOUCHCTL_PLAY;
		break;
	case TOUCHMODE_DEAD:
		c = TOUCHCTL_CHROME | TOUCHCTL_LOOK | TOUCHCTL_RESPAWN | TOUCHCTL_TOSPEC;
		break;
	case TOUCHMODE_OBSERVING:
		c = TOUCHCTL_CHROME | TOUCHCTL_MOVE | TOUCHCTL_LOOK | TOUCHCTL_SPECBAR | TOUCHCTL_FLY;
		break;
	case TOUCHMODE_OUT_FREE:
		c = TOUCHCTL_CHROME | TOUCHCTL_MOVE | TOUCHCTL_LOOK | TOUCHCTL_SPECBAR | TOUCHCTL_FLY;
		break;
	case TOUCHMODE_FOLLOWING:
		c = TOUCHCTL_CHROME | TOUCHCTL_SPECBAR | TOUCHCTL_VITALS;
		break;
	case TOUCHMODE_OUT:
		c = TOUCHCTL_CHROME | TOUCHCTL_SPECBAR | TOUCHCTL_VITALS;
		break;
	case TOUCHMODE_INTERMISSION:
	case TOUCHMODE_MAPVOTE:
		// The scoreboard and the vote own the screen: MENU only (and the
		// console button, touch_shellbtn.c, which is not part of the HUD).
		return TOUCHCTL_MENU;
	default:
		return 0;
	}
	if (flags & TOUCHMODE_F_READY)
		c |= TOUCHCTL_READY;
	return c;
}

// ---------------------------------------------------------------------------
// The in-match MENU sheet's actions, for the touch menu and the native
// pause menus (docs/CONTROLS.md, "Menu actions"). Fixed order.
// ---------------------------------------------------------------------------
#define TOUCHACT_RESUME   0
#define TOUCHACT_JOIN     1
#define TOUCHACT_SPECTATE 2   // "Move to spectators"
#define TOUCHACT_TEAM     3   // "Change team"
#define TOUCHACT_SETTINGS 4
#define TOUCHACT_LEAVE    5
#define TOUCHACT_COUNT    6

#define TOUCHACT_HIDDEN   0
#define TOUCHACT_ENABLED  1
#define TOUCHACT_DISABLED 2   // shown, not now (a join being answered, in the queue)
#define TOUCHACT_CONFIRM  3   // enabled, asks first: it would forfeit something

static inline const char *TouchAct_Id(int act)
{
	static const char *ids[TOUCHACT_COUNT] = { "resume", "join", "spectate", "team", "settings", "leave" };
	return act >= 0 && act < TOUCHACT_COUNT ? ids[act] : "";
}

static inline const char *TouchAct_Label(int act)
{
	static const char *labels[TOUCHACT_COUNT] = { "Resume", "Join game", "Spectate", "Change team", "Settings", "Leave" };
	return act >= 0 && act < TOUCHACT_COUNT ? labels[act] : "";
}

// Moving to the spectators forfeits something mid-round in a round-based
// game (the life in this round) or while holding an objective.
static inline int TouchMode_SpectateForfeits(const touchmode_t *m)
{
	return (m->flags & (TOUCHMODE_F_ROUND | TOUCHMODE_F_CARRY)) != 0;
}

static inline int TouchAct_State(const touchmode_t *m, int act)
{
	if (m->mode == TOUCHMODE_OFF)
		return TOUCHACT_HIDDEN;
	switch (act)
	{
	case TOUCHACT_RESUME:
	case TOUCHACT_SETTINGS:
	case TOUCHACT_LEAVE:
		return TOUCHACT_ENABLED;
	case TOUCHACT_JOIN:
		if (!TouchMode_IsSpectating(m->mode))
			return TOUCHACT_HIDDEN;
		return m->join == TOUCHJOIN_OPEN || m->join == TOUCHJOIN_LOCKED ? TOUCHACT_ENABLED : TOUCHACT_DISABLED;
	case TOUCHACT_SPECTATE:
		if (m->mode == TOUCHMODE_PLAYING || m->mode == TOUCHMODE_DEAD)
			return TouchMode_SpectateForfeits(m) ? TOUCHACT_CONFIRM : TOUCHACT_ENABLED;
		if (TouchMode_IsOutOfRound(m->mode))
			return TOUCHACT_ENABLED;
		return TOUCHACT_HIDDEN;
	case TOUCHACT_TEAM:
		if (!(m->flags & TOUCHMODE_F_TEAMS) || TouchMode_IsMatchEnd(m->mode))
			return TOUCHACT_HIDDEN;
		return TOUCHACT_ENABLED;
	default:
		return TOUCHACT_HIDDEN;
	}
}

// The console command an action runs. JOIN and TEAM open the join dialog
// (touch_join_dialog: Xonotic's Team Selection, with auto-select, the teams
// the server offers and spectate).
static inline const char *TouchAct_Command(const touchmode_t *m, int act)
{
	(void)m;
	switch (act)
	{
	case TOUCHACT_RESUME: return "togglemenu 0";
	case TOUCHACT_JOIN: return "touch_join_dialog";
	case TOUCHACT_SPECTATE: return "spec";
	case TOUCHACT_TEAM: return "touch_join_dialog";
	case TOUCHACT_SETTINGS: return "menu_cmd settings";
	case TOUCHACT_LEAVE: return "disconnect; g_campaign 0; menu_cmd sync; menu_cmd directmenu Welcome RESET";
	default: return "";
	}
}

// ---------------------------------------------------------------------------
// Joining. The join dialog is Xonotic's own (the menu's Team Selection:
// "join 'best' team (auto-select)", the teams, "spectate"), finger-sized on a
// touchscreen. It is owed once per match to a player who starts it as an
// observer, and opens when that has lasted a moment and nothing else has the
// screen; after that it is the MENU sheet's "Join game" (observing) and
// "Change team". A game hosted on this device with no team to pick skips it:
// the player goes straight in ("cmd join", a few tries while the server is
// not ready for it). Picking Spectate, or being in the game once, settles
// it: spectating later in the same match is the player's own choice.
// ---------------------------------------------------------------------------
#define TOUCHJOINDLG_SETTLE  0.75f  // seconds as an observer before it opens
#define TOUCHJOINDLG_RETRY   1.5f   // a game hosted here: between "join" tries
#define TOUCHJOINDLG_TRIES   6

#define TOUCHJOINDLG_NOTHING 0
#define TOUCHJOINDLG_OPEN    1   // open the join dialog
#define TOUCHJOINDLG_JOIN    2   // hosted here, no team to pick: join now

typedef struct touchjoindlg_s
{
	int owed;       // this match still owes the observer the dialog
	int tries;      // straight-in joins left (a game hosted here)
	double since;   // an observer since, 0 = not
	double next;    // nothing before this time
} touchjoindlg_t;

// One frame. local_game: the server is this process. campaign: the campaign
// joins by itself. screen_free: the game has the screen (no menu, console,
// chat sheet or CSQC pointer UI). enabled: touch_join_dialog allows it here.
static inline int TouchJoinDlg_Frame(touchjoindlg_t *d, int mode, int flags, int local_game, int campaign, int screen_free, int enabled, double now)
{
	if (mode == TOUCHMODE_OFF || TouchMode_IsMatchEnd(mode))
	{
		d->owed = 1;
		d->tries = TOUCHJOINDLG_TRIES;
		d->since = 0;
		d->next = 0;
		return TOUCHJOINDLG_NOTHING;
	}
	if (mode == TOUCHMODE_PLAYING || TouchMode_IsOutOfRound(mode))
	{
		d->owed = 0;
		d->since = 0;
		return TOUCHJOINDLG_NOTHING;
	}
	if (!TouchMode_IsSpectating(mode))
	{
		// dead: also what a client reads before its first scoreboard arrives
		d->since = 0;
		return TOUCHJOINDLG_NOTHING;
	}
	if (!d->owed || campaign)
		return TOUCHJOINDLG_NOTHING;
	if (!d->since)
		d->since = now;
	if (now - d->since < TOUCHJOINDLG_SETTLE || now < d->next || !screen_free)
		return TOUCHJOINDLG_NOTHING;
	if (local_game && !(flags & TOUCHMODE_F_TEAMS) && d->tries > 0)
	{
		d->tries--;
		d->next = now + TOUCHJOINDLG_RETRY;
		return TOUCHJOINDLG_JOIN;
	}
	d->owed = 0;
	return enabled ? TOUCHJOINDLG_OPEN : TOUCHJOINDLG_NOTHING;
}

// The player answered the dialog (or the MENU sheet's Spectate): nothing is
// owed any more in this match.
static inline void TouchJoinDlg_Answered(touchjoindlg_t *d)
{
	d->owed = 0;
}

// What a choice in the dialog sends. Choices: "auto", "red", "blue",
// "yellow", "pink", "spectate". teams: a team game. Returns "" for a choice
// that is not one. The same commands the stock dialog's buttons run
// (team_auto, team_red, ...; "cmd spectate"), without the menu toggles.
static inline const char *TouchJoinDlg_Command(const char *choice, int teams)
{
	if (!strcmp(choice, "auto")) return teams ? "cmd selectteam auto; cmd join" : "cmd join";
	if (!strcmp(choice, "red")) return "cmd selectteam red; cmd join red";
	if (!strcmp(choice, "blue")) return "cmd selectteam blue; cmd join blue";
	if (!strcmp(choice, "yellow")) return "cmd selectteam yellow; cmd join yellow";
	if (!strcmp(choice, "pink")) return "cmd selectteam pink; cmd join pink";
	if (!strcmp(choice, "spectate")) return "cmd spectate";
	return "";
}

// ---------------------------------------------------------------------------
// Sizes in density-independent pixels: primary actions 56-64 dp, secondary
// 46-48 dp, never a touch target under 48 dp.
// ---------------------------------------------------------------------------
#define TOUCHDP_PRIMARY     60.0f
#define TOUCHDP_SECONDARY   46.0f
#define TOUCHDP_MIN_TARGET  48.0f
#define TOUCHDP_PILL_HEIGHT 36.0f   // drawn; its target is TOUCHDP_MIN_TARGET or more

// Physical pixels per dp from a reported dpi; without one, a guess from the
// shorter side (a 1080 px phone is about 2.6), never below 1.
static inline float TouchDp_Density(float dpi, float short_px)
{
	float d;
	if (dpi > 1 && dpi != 160)
		d = dpi / 160.0f;
	else
		d = short_px >= 900 ? short_px / 420.0f : 1.0f;
	return d < 1 ? 1 : d > 4 ? 4 : d;
}

#endif
