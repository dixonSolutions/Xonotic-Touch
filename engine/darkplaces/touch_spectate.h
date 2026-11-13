/*
Godnotic: what the touch controls do while spectating, as a pure table
both HUDs follow (the engine's touch_hud.c on servers, the port's CSQC in a
local match mirrors it in touch_spectate.qc), checked by
scripts/test-touch-spectate.sh.

The server's spectator code (ObserverOrSpectatorThink, server/client.qc in
0.8.6 and later) reads, for a client that is not playing:
  observing (free-fly)   +attack            start following a player
                         +jump              join, when joining is allowed
                         +moveup/+movedown  fly up / down (cl_upspeed)
  following a player     +attack, weapnext  next player
                         weapprev           previous player
                         +attack2           back to free-fly
                         +jump              join
  out of the round (Clan Arena and the like, frags -616, -669 since 0.8.6):
  following or free-flying as above, but the join waits for the next round.
Jump is the join request in every state, and joining is a swipe up from the
handle at the bottom middle (touch_hudmode.h), never a button: so no
spectator control sends +jump, and the flying controls use +moveup /
+movedown, never +jump / +crouch.

A spectator's controls are stripped to what a spectator uses: the move stick
and the fly rocker (UP / DOWN) while free-flying, and one compact bar,
previous | follow or free | next. No weapons, fire, jump, crouch, hook,
nade or turn.
*/
#ifndef TOUCH_SPECTATE_H
#define TOUCH_SPECTATE_H

// Spectator states (TouchUI_SpectatorState).
#define TOUCHSPEC_NONE      0
#define TOUCHSPEC_OBSERVING 1
#define TOUCHSPEC_FOLLOWING 2
#define TOUCHSPEC_OUT       3   // out of the round, following a player
#define TOUCHSPEC_OUT_FREE  4   // out of the round, free-flying

// The spectator bar's parts, left to right.
#define TOUCHSPEC_BAR_PREV  0
#define TOUCHSPEC_BAR_MAIN  1
#define TOUCHSPEC_BAR_NEXT  2
#define TOUCHSPEC_BAR_PARTS 3

// What a part does.
#define TOUCHSPEC_ACT_NONE    0
#define TOUCHSPEC_ACT_FOLLOW  1   // +attack while held: follow a player
#define TOUCHSPEC_ACT_NEXT    2   // +attack while held: next player
#define TOUCHSPEC_ACT_PREV    3   // weapprev on the press: previous player
#define TOUCHSPEC_ACT_FREE    4   // +attack2 while held: back to free-fly
#define TOUCHSPEC_ACT_UP      5   // +moveup while held (fly rocker)
#define TOUCHSPEC_ACT_DOWN    6   // +movedown while held (fly rocker)

static inline int TouchSpec_Flying(int state)
{
	return state == TOUCHSPEC_OBSERVING || state == TOUCHSPEC_OUT_FREE;
}

static inline int TouchSpec_BarAction(int state, int part)
{
	if (TouchSpec_Flying(state))
		// Free-flying, either arrow or the middle starts following; next /
		// previous weapon would only change the flying speed.
		return part >= 0 && part < TOUCHSPEC_BAR_PARTS ? TOUCHSPEC_ACT_FOLLOW : TOUCHSPEC_ACT_NONE;
	switch (state)
	{
	case TOUCHSPEC_FOLLOWING:
		return part == TOUCHSPEC_BAR_PREV ? TOUCHSPEC_ACT_PREV
			: part == TOUCHSPEC_BAR_MAIN ? TOUCHSPEC_ACT_FREE
			: part == TOUCHSPEC_BAR_NEXT ? TOUCHSPEC_ACT_NEXT
			: TOUCHSPEC_ACT_NONE;
	case TOUCHSPEC_OUT:
		// Out of the round the server keeps the view on a player.
		return part == TOUCHSPEC_BAR_PREV ? TOUCHSPEC_ACT_PREV
			: part == TOUCHSPEC_BAR_NEXT ? TOUCHSPEC_ACT_NEXT
			: TOUCHSPEC_ACT_NONE;
	default:
		return TOUCHSPEC_ACT_NONE;
	}
}

static inline const char *TouchSpec_ActionLabel(int action)
{
	switch (action)
	{
	case TOUCHSPEC_ACT_FOLLOW: return "FOLLOW";
	case TOUCHSPEC_ACT_NEXT: return "NEXT";
	case TOUCHSPEC_ACT_PREV: return "PREV";
	case TOUCHSPEC_ACT_FREE: return "FREE";
	case TOUCHSPEC_ACT_UP: return "UP";
	case TOUCHSPEC_ACT_DOWN: return "DOWN";
	default: return "";
	}
}

// What the bar part shows: arrows for the sides, a word in the middle.
static inline const char *TouchSpec_BarLabel(int state, int part)
{
	int action = TouchSpec_BarAction(state, part);
	if (action == TOUCHSPEC_ACT_NONE)
		return part == TOUCHSPEC_BAR_MAIN && state != TOUCHSPEC_NONE ? "WATCHING" : "";
	if (part == TOUCHSPEC_BAR_PREV)
		return "<";
	if (part == TOUCHSPEC_BAR_NEXT)
		return ">";
	return TouchSpec_ActionLabel(action);
}

// The move stick, the look drag and the fly rocker work only while free-
// flying; following someone, the view is theirs.
static inline int TouchSpec_StickShown(int state)
{
	return TouchSpec_Flying(state);
}

// ---------------------------------------------------------------------------
// After the join swipe (or a click on its handle): +jump goes out for a
// moment. A join the server does not take (joining locked, teams full,
// spectate-only) leaves the player spectating; the handle then says so for
// a while instead of failing silently.
// ---------------------------------------------------------------------------
#define TOUCHSPEC_JOIN_PRESS  0.25  // how long +jump is held for the join
#define TOUCHSPEC_JOIN_WAIT   3.0   // a join not taken by then was refused
#define TOUCHSPEC_JOIN_LOCKED 15.0  // how long the handle then shows LOCKED

typedef struct touchspec_join_s
{
	double sent_time;     // the join was sent, 0 = not
	double locked_until;  // shown as locked until
} touchspec_join_t;

// Join is offered observing and following (the stock HUD's "press jump to
// join"); out of the round the player is already in the game.
static inline int TouchSpec_JoinShown(int state)
{
	return state == TOUCHSPEC_OBSERVING || state == TOUCHSPEC_FOLLOWING;
}

static inline void TouchSpec_JoinSend(touchspec_join_t *j, double now)
{
	j->sent_time = now;
	j->locked_until = 0;
}

static inline int TouchSpec_JoinPending(const touchspec_join_t *j)
{
	return j->sent_time != 0;
}

static inline int TouchSpec_JoinLocked(const touchspec_join_t *j, double now)
{
	return now < j->locked_until;
}

// One frame. Returns whether +jump is to be held this frame.
static inline int TouchSpec_JoinFrame(touchspec_join_t *j, int state, double now)
{
	if (state == TOUCHSPEC_NONE)
		j->locked_until = 0;
	if (!j->sent_time)
		return 0;
	if (state == TOUCHSPEC_NONE)
	{
		// Taken: playing. A lock from a slow answer goes too, or the next
		// time this player spectates the handle would say LOCKED.
		j->sent_time = 0;
		j->locked_until = 0;
		return 0;
	}
	if (now - j->sent_time >= TOUCHSPEC_JOIN_WAIT)
	{
		// Still a spectator well after the join went out: the server said no.
		j->sent_time = 0;
		j->locked_until = now + TOUCHSPEC_JOIN_LOCKED;
		return 0;
	}
	return now - j->sent_time < TOUCHSPEC_JOIN_PRESS;
}

#endif
