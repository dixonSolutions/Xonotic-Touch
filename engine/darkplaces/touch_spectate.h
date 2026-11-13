/*
Godnotic: what the touch controls do while spectating, as a pure table
both HUDs follow (the engine's touch_hud.c on servers, the port's CSQC in a
local match mirrors it), checked by scripts/test-touch-spectate.sh.

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
Jump is the join request in every state, so nothing but JOIN may send it:
the flying controls use +moveup / +movedown, never +jump / +crouch.
*/
#ifndef TOUCH_SPECTATE_H
#define TOUCH_SPECTATE_H

// Spectator states (TouchUI_SpectatorState).
#define TOUCHSPEC_NONE      0
#define TOUCHSPEC_OBSERVING 1
#define TOUCHSPEC_FOLLOWING 2
#define TOUCHSPEC_OUT       3   // out of the round, following a player
#define TOUCHSPEC_OUT_FREE  4   // out of the round, free-flying

// The play controls' places, reused with spectator meanings.
#define TOUCHSPEC_SLOT_FIRE 0
#define TOUCHSPEC_SLOT_ALT  1
#define TOUCHSPEC_SLOT_HOP  2
#define TOUCHSPEC_SLOT_DUCK 3
#define TOUCHSPEC_SLOTS     4

// What a slot does.
#define TOUCHSPEC_ACT_NONE    0
#define TOUCHSPEC_ACT_FOLLOW  1   // +attack while held: follow a player
#define TOUCHSPEC_ACT_NEXT    2   // +attack while held: next player
#define TOUCHSPEC_ACT_PREV    3   // weapprev on the press: previous player
#define TOUCHSPEC_ACT_FREE    4   // +attack2 while held: back to free-fly
#define TOUCHSPEC_ACT_UP      5   // +moveup while held
#define TOUCHSPEC_ACT_DOWN    6   // +movedown while held

static int TouchSpec_SlotAction(int state, int slot)
{
	switch (state)
	{
	case TOUCHSPEC_OBSERVING:
	case TOUCHSPEC_OUT_FREE:
		return slot == TOUCHSPEC_SLOT_FIRE ? TOUCHSPEC_ACT_FOLLOW
			: slot == TOUCHSPEC_SLOT_HOP ? TOUCHSPEC_ACT_UP
			: slot == TOUCHSPEC_SLOT_DUCK ? TOUCHSPEC_ACT_DOWN
			: TOUCHSPEC_ACT_NONE;
	case TOUCHSPEC_FOLLOWING:
		return slot == TOUCHSPEC_SLOT_FIRE ? TOUCHSPEC_ACT_NEXT
			: slot == TOUCHSPEC_SLOT_ALT ? TOUCHSPEC_ACT_PREV
			: slot == TOUCHSPEC_SLOT_HOP ? TOUCHSPEC_ACT_FREE
			: TOUCHSPEC_ACT_NONE;
	case TOUCHSPEC_OUT:
		return slot == TOUCHSPEC_SLOT_FIRE ? TOUCHSPEC_ACT_NEXT
			: slot == TOUCHSPEC_SLOT_ALT ? TOUCHSPEC_ACT_PREV
			: TOUCHSPEC_ACT_NONE;
	default:
		return TOUCHSPEC_ACT_NONE;
	}
}

static const char *TouchSpec_ActionLabel(int action)
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

// The move stick and look drag fly the camera only while observing; following
// someone, the view is theirs.
static int TouchSpec_StickShown(int state)
{
	return state == TOUCHSPEC_OBSERVING || state == TOUCHSPEC_OUT_FREE;
}

// ---------------------------------------------------------------------------
// JOIN: held, not tapped, so a thumb resting near it cannot join by accident.
// A join the server does not take (joining locked, teams full, spectate-only)
// leaves the player spectating; JOIN then shows as locked for a while.
// ---------------------------------------------------------------------------
#define TOUCHSPEC_JOIN_HOLD   0.45  // seconds held before it joins
#define TOUCHSPEC_JOIN_PRESS  0.25  // how long +jump is held for the join
#define TOUCHSPEC_JOIN_WAIT   3.0   // a join not taken by then was refused
#define TOUCHSPEC_JOIN_LOCKED 15.0  // how long JOIN then shows as locked

typedef struct touchspec_join_s
{
	double hold_start;    // finger on JOIN since, 0 = none
	double sent_time;     // the join was sent, 0 = not
	double locked_until;  // shown as locked until
	int state_at_send;    // the spectator state the join was sent from
} touchspec_join_t;

// JOIN is offered observing and following (the stock HUD's "press jump to
// join"); out of the round the player is already in the game.
static int TouchSpec_JoinShown(int state)
{
	return state == TOUCHSPEC_OBSERVING || state == TOUCHSPEC_FOLLOWING;
}

static int TouchSpec_JoinLocked(const touchspec_join_t *j, double now)
{
	return now < j->locked_until;
}

// 0..1 while held, for the fill.
static float TouchSpec_JoinProgress(const touchspec_join_t *j, double now)
{
	double p;
	if (!j->hold_start)
		return 0;
	p = (now - j->hold_start) / TOUCHSPEC_JOIN_HOLD;
	return p < 0 ? 0 : p > 1 ? 1 : (float)p;
}

// One frame. held: a finger is on JOIN. Returns whether +jump is to be held
// this frame.
static int TouchSpec_JoinFrame(touchspec_join_t *j, int state, int held, double now)
{
	if (j->sent_time)
	{
		// Still a spectator well after the join went out: the server said no.
		if (state == TOUCHSPEC_NONE)
			j->sent_time = 0;
		else if (now - j->sent_time >= TOUCHSPEC_JOIN_WAIT)
		{
			j->sent_time = 0;
			j->locked_until = now + TOUCHSPEC_JOIN_LOCKED;
		}
		else
			return now - j->sent_time < TOUCHSPEC_JOIN_PRESS;
	}
	if (!held || !TouchSpec_JoinShown(state))
	{
		j->hold_start = 0;
		return 0;
	}
	if (!j->hold_start)
		j->hold_start = now;
	if (now - j->hold_start >= TOUCHSPEC_JOIN_HOLD)
	{
		j->hold_start = 0;
		j->sent_time = now;
		j->state_at_send = state;
		j->locked_until = 0;
		return 1;
	}
	return 0;
}

#endif
