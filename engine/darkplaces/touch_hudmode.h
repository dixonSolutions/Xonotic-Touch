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
  - the join swipe (TouchSwipe_*): the only way to join from spectating, a
    mostly vertical swipe up that starts on the handle at the bottom middle.

No engine headers: scripts/test-touch-hudmode.sh compiles this alone. The
QuakeC side mirrors the numbers in qcsrc/client/touch_mode.qh.
*/
#ifndef TOUCH_HUDMODE_H
#define TOUCH_HUDMODE_H

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
#define TOUCHCTL_JOIN     (1 << 19)   // the join swipe handle
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
		c = TOUCHCTL_CHROME | TOUCHCTL_MOVE | TOUCHCTL_LOOK | TOUCHCTL_SPECBAR | TOUCHCTL_FLY | TOUCHCTL_JOIN;
		break;
	case TOUCHMODE_OUT_FREE:
		c = TOUCHCTL_CHROME | TOUCHCTL_MOVE | TOUCHCTL_LOOK | TOUCHCTL_SPECBAR | TOUCHCTL_FLY;
		break;
	case TOUCHMODE_FOLLOWING:
		c = TOUCHCTL_CHROME | TOUCHCTL_SPECBAR | TOUCHCTL_VITALS | TOUCHCTL_JOIN;
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
	static const char *labels[TOUCHACT_COUNT] = { "Resume", "Join", "Move to spectators", "Change team", "Settings", "Leave" };
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

// The console command an action runs. JOIN in a team game opens the stock
// team selection (it has Auto as well as the teams); otherwise "join".
static inline const char *TouchAct_Command(const touchmode_t *m, int act)
{
	switch (act)
	{
	case TOUCHACT_RESUME: return "togglemenu 0";
	case TOUCHACT_JOIN: return (m->flags & TOUCHMODE_F_TEAMS) ? "menu_showteamselect" : "join";
	case TOUCHACT_SPECTATE: return "spec";
	case TOUCHACT_TEAM: return "menu_showteamselect";
	case TOUCHACT_SETTINGS: return "menu_cmd settings";
	case TOUCHACT_LEAVE: return "disconnect; g_campaign 0; menu_cmd sync; menu_cmd directmenu Welcome RESET";
	default: return "";
	}
}

// ---------------------------------------------------------------------------
// The join swipe. Positions in pixels of a w x h screen, times in seconds.
// It starts only on the handle's zone at the bottom middle, has to go up a
// fair way, mostly straight, at a swipe's speed, and is decided on release:
// let go early, too slowly or too slanted and it springs back. A finger that
// sets off sideways or down was looking around: it is handed to the look.
// ---------------------------------------------------------------------------
#define TOUCHSWIPE_ZONE_HALF_W  0.11f   // of the width, either side of the middle
#define TOUCHSWIPE_ZONE_H       0.16f   // of the height, from the bottom edge
#define TOUCHSWIPE_DIST         0.14f   // of the height, upwards
#define TOUCHSWIPE_SLANT        0.60f   // |dx| / dy at most (about 31 degrees)
#define TOUCHSWIPE_SLOP         0.025f  // of the height: below this nothing is decided
#define TOUCHSWIPE_MIN_SPEED    0.25f   // heights per second, on average
#define TOUCHSWIPE_MAX_TIME     1.20f   // seconds from down to release

#define TOUCHSWIPE_TRACKING  0
#define TOUCHSWIPE_ACCEPT    1
#define TOUCHSWIPE_LOOK      2   // handed to the look: sideways or down
#define TOUCHSWIPE_SHORT     3   // released before the threshold
#define TOUCHSWIPE_SLOW      4
#define TOUCHSWIPE_SLANTED   5

typedef struct touchswipe_s
{
	float x0, y0, x, y;
	double t0, t;
	int state;
} touchswipe_t;

static inline int TouchSwipe_InZone(float x, float y, float w, float h)
{
	float dx = x - w * 0.5f;
	return (dx < 0 ? -dx : dx) <= w * TOUCHSWIPE_ZONE_HALF_W && y >= h * (1.0f - TOUCHSWIPE_ZONE_H) && y <= h;
}

static inline void TouchSwipe_Begin(touchswipe_t *s, float x, float y, double t)
{
	s->x0 = s->x = x;
	s->y0 = s->y = y;
	s->t0 = s->t = t;
	s->state = TOUCHSWIPE_TRACKING;
}

static inline int TouchSwipe_Move(touchswipe_t *s, float x, float y, double t, float h)
{
	float dx, up, adx;
	if (s->state != TOUCHSWIPE_TRACKING)
		return s->state;
	s->x = x;
	s->y = y;
	s->t = t;
	dx = x - s->x0;
	up = s->y0 - y;
	adx = dx < 0 ? -dx : dx;
	// Off sideways or downwards before it went up: a look, not a join.
	if (up < h * TOUCHSWIPE_SLOP && (adx > h * TOUCHSWIPE_SLOP || -up > h * TOUCHSWIPE_SLOP))
		s->state = TOUCHSWIPE_LOOK;
	else if (adx > up && adx > h * TOUCHSWIPE_SLOP)
		s->state = TOUCHSWIPE_LOOK;
	return s->state;
}

static inline int TouchSwipe_End(touchswipe_t *s, float x, float y, double t, float h)
{
	float dx, up, adx, dt;
	if (TouchSwipe_Move(s, x, y, t, h) != TOUCHSWIPE_TRACKING)
		return s->state;
	dx = x - s->x0;
	up = s->y0 - y;
	adx = dx < 0 ? -dx : dx;
	dt = (float)(t - s->t0);
	if (up < h * TOUCHSWIPE_DIST)
		s->state = TOUCHSWIPE_SHORT;
	else if (adx > up * TOUCHSWIPE_SLANT)
		s->state = TOUCHSWIPE_SLANTED;
	else if (dt > TOUCHSWIPE_MAX_TIME || up / (dt > 0.001f ? dt : 0.001f) < h * TOUCHSWIPE_MIN_SPEED)
		s->state = TOUCHSWIPE_SLOW;
	else
		s->state = TOUCHSWIPE_ACCEPT;
	return s->state;
}

// How far up, 0 at the start, 1 at the threshold (more past it), for the
// handle following the finger.
static inline float TouchSwipe_Progress(const touchswipe_t *s, float h)
{
	float p = (s->y0 - s->y) / (h * TOUCHSWIPE_DIST);
	return p < 0 ? 0 : p;
}

static inline const char *TouchSwipe_Name(int state)
{
	switch (state)
	{
	case TOUCHSWIPE_TRACKING: return "tracking";
	case TOUCHSWIPE_ACCEPT: return "accept";
	case TOUCHSWIPE_LOOK: return "look";
	case TOUCHSWIPE_SHORT: return "short";
	case TOUCHSWIPE_SLOW: return "slow";
	case TOUCHSWIPE_SLANTED: return "slanted";
	default: return "?";
	}
}

// ---------------------------------------------------------------------------
// Sizes in density-independent pixels: primary actions 56-64 dp, secondary
// 44-48 dp, never a touch target under 44 dp.
// ---------------------------------------------------------------------------
#define TOUCHDP_PRIMARY     60.0f
#define TOUCHDP_SECONDARY   46.0f
#define TOUCHDP_MIN_TARGET  44.0f
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
