/*
Copyright (C) 2026 Xonotic Touch contributors

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.
*/

/*
The decisions behind the touch fall rescue (touch_hook.c): where a falling
player will come down, which point a grappling hook can catch to pull them
back over ground, and when to fire, hold and let go.

Where the player comes down: the arc from the position and velocity under
gravity, traced in segments with the player's box, then straight down from
where the arc ends or meets a wall. It ends on ground (a floor flat enough
to stand on, or water), in a death zone (sky, a nodrop or lava/slime volume,
a trigger_hurt box), or in the void (nothing above the world's lowest point).
Air control is not modelled: a player steering hard toward a ledge is judged
by where they are heading now.

The world is reached through callbacks (tr_world_t), so this file is plain C
with no engine types: scripts/test-touch-hook.sh runs it against a world of
boxes. Only touch_hook.c includes it in the engine; the includer supplies
qbool, true and false.
*/

#ifndef TOUCH_RESCUE_H
#define TOUCH_RESCUE_H

#include <math.h>
#include <string.h>

// What a trace stopped at.
#define TR_HIT_NONE  0
#define TR_HIT_SOLID 1 // world or a brush entity; the normal says whether it is a floor
#define TR_HIT_WATER 2 // a liquid that does not kill: something to land in
#define TR_HIT_DEATH 3 // sky, nodrop, lava or slime

typedef struct tr_trace_s
{
	float fraction;   // 0..1 of the way; 1 = nothing in the way
	float endpos[3];
	float normal[3];
	int hit;          // TR_HIT_*
	int startsolid;   // began inside something: no answer from this point
} tr_trace_t;

typedef struct tr_world_s
{
	// A trace from start to end: with the player's box (hull 1) or a point (0).
	void (*trace)(void *ctx, const float start[3], const float end[3], int hull, tr_trace_t *out);
	// Where the player's box moving from start to end first enters a death
	// volume (a trigger_hurt), as a fraction of the way, or > 1 for never.
	// May be NULL.
	float (*death)(void *ctx, const float start[3], const float end[3]);
	void *ctx;
	float floor_z;   // the world's lowest point: below it there is nothing
	float gravity;   // units/s^2, positive
	int traces;      // traces made, for the frame cost
} tr_world_t;

// Where a fall ends.
#define TR_LAND  0
#define TR_VOID  1
#define TR_DEATH 2

static const char *const tr_outcome_name[] = { "ground", "void", "death zone" };

typedef struct tr_fall_s
{
	int outcome;     // TR_LAND, TR_VOID, TR_DEATH
	float point[3];  // where it ends (the landing point, or the last point traced)
	float time;      // seconds along the arc to it (0 for a straight drop)
} tr_fall_t;

// A floor is a surface whose normal points at least this much up (the
// engine's own walkable slope).
#define TR_FLOOR_NZ   0.7f
// The arc is followed this long, in segments of about TR_SEG_LEN units (at
// least TR_SEG_MIN_T seconds), at most TR_MAX_SEGS of them; then straight down.
#define TR_HORIZON    2.5f
#define TR_SEG_LEN    384.0f
#define TR_SEG_MIN_T  0.08f
#define TR_MAX_SEGS   8
// A wall slide or a ceiling bump is followed by at most this many straight drops.
#define TR_MAX_SLIDES 2
#define TR_DEG2RAD    0.017453293f

static void tr_ma(const float a[3], float s, const float b[3], float out[3])
{
	out[0] = a[0] + s * b[0];
	out[1] = a[1] + s * b[1];
	out[2] = a[2] + s * b[2];
}

static float tr_dot(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float tr_dist(const float a[3], const float b[3])
{
	float d[3];
	d[0] = a[0] - b[0];
	d[1] = a[1] - b[1];
	d[2] = a[2] - b[2];
	return sqrtf(tr_dot(d, d));
}

static void tr_trace(tr_world_t *w, const float start[3], const float end[3], int hull, tr_trace_t *out)
{
	w->traces++;
	w->trace(w->ctx, start, end, hull, out);
}

// Fraction where start->end enters a death volume, > 1 for none.
static float tr_death(tr_world_t *w, const float start[3], const float end[3])
{
	return w->death ? w->death(w->ctx, start, end) : 2.0f;
}

// A trace from a to b judged: TR_LAND / TR_DEATH when it ends the fall
// (*done set), else not done (a wall, or nothing within reach), with the
// stopping point in `at`.
static int tr_judge(tr_world_t *w, const float a[3], const float b[3], int *done, float at[3], float normal[3])
{
	tr_trace_t tr;
	float df;
	tr_trace(w, a, b, 1, &tr);
	df = tr_death(w, a, b);
	*done = 0;
	memcpy(at, tr.endpos, sizeof(float[3]));
	memcpy(normal, tr.normal, sizeof(float[3]));
	if (tr.startsolid)
	{
		memcpy(at, a, sizeof(float[3]));
		return TR_VOID; // stuck: not done, and no progress
	}
	if (df <= tr.fraction)
	{
		// into a trigger_hurt before reaching whatever is beyond it
		at[0] = a[0] + (b[0] - a[0]) * df;
		at[1] = a[1] + (b[1] - a[1]) * df;
		at[2] = a[2] + (b[2] - a[2]) * df;
		*done = 1;
		return TR_DEATH;
	}
	if (tr.fraction >= 1 || tr.hit == TR_HIT_NONE)
		return TR_VOID;
	if (tr.hit == TR_HIT_DEATH)
	{
		*done = 1;
		return TR_DEATH;
	}
	if (tr.hit == TR_HIT_WATER || tr.normal[2] >= TR_FLOOR_NZ)
	{
		*done = 1;
		return TR_LAND;
	}
	return TR_VOID; // a wall or a ceiling: not done
}

// The origin is estimated from the view, a unit or two off: on the frame a
// player lands it can be in the floor, and against a wall in it. Step out of
// whatever the box starts in, up first, then sideways; 0 when it cannot.
static int tr_unstick(tr_world_t *w, float p[3])
{
	static const float off[][3] = { {0, 0, 4}, {4, 0, 0}, {-4, 0, 0}, {0, 4, 0}, {0, -4, 0}, {0, 0, 10}, {10, 0, 0}, {-10, 0, 0}, {0, 10, 0}, {0, -10, 0} };
	tr_trace_t tr;
	float q[3];
	int i;
	tr_trace(w, p, p, 1, &tr);
	if (!tr.startsolid)
		return 1;
	for (i = 0; i < (int)(sizeof(off) / sizeof(off[0])); i++)
	{
		q[0] = p[0] + off[i][0];
		q[1] = p[1] + off[i][1];
		q[2] = p[2] + off[i][2];
		tr_trace(w, q, q, 1, &tr);
		if (!tr.startsolid)
		{
			memcpy(p, q, sizeof(q));
			return 1;
		}
	}
	return 0;
}

// Straight down from `from`, at most `depth` units (never past the world's
// lowest point): ground, a death zone, or the void. A steep surface on the way
// is slid down. With depth <= 0 the drop goes to the bottom of the world.
static int TouchRescue_Below(tr_world_t *w, const float from[3], float depth, tr_fall_t *out)
{
	float a[3], b[3], at[3], normal[3];
	int slides, done, r;
	memcpy(a, from, sizeof(a));
	out->time = 0;
	if (!tr_unstick(w, a))
	{
		memcpy(out->point, from, sizeof(out->point));
		return out->outcome = TR_VOID; // buried: no answer from here
	}
	for (slides = 0; slides <= TR_MAX_SLIDES; slides++)
	{
		float bottom = w->floor_z - 64;
		if (depth > 0 && from[2] - depth > bottom)
			bottom = from[2] - depth;
		if (a[2] <= bottom)
			break;
		b[0] = a[0];
		b[1] = a[1];
		b[2] = bottom;
		r = tr_judge(w, a, b, &done, at, normal);
		memcpy(out->point, at, sizeof(at));
		if (done)
			return out->outcome = r;
		if (at[2] <= bottom + 0.5f || at[2] >= a[2] - 0.01f)
			break; // nothing all the way down, or stuck where it began
		// a slope too steep to stand on: off it, a little lower, and on down
		a[0] = at[0] + normal[0] * 4;
		a[1] = at[1] + normal[1] * 4;
		a[2] = at[2] - 1;
	}
	memcpy(out->point, a, sizeof(a));
	out->point[2] = a[2] < w->floor_z ? a[2] : w->floor_z;
	return out->outcome = TR_VOID;
}

// Where the arc from org at vel comes down. A start inside something is
// stepped out of (tr_unstick); one still inside is ground: whatever that is,
// it is not a fall into the void.
static int TouchRescue_Predict(tr_world_t *w, const float org[3], const float vel[3], tr_fall_t *out)
{
	float pos[3], v[3], next[3], at[3], normal[3];
	float t = 0, g = w->gravity > 0 ? w->gravity : 800;
	int seg, done, r;
	memcpy(pos, org, sizeof(pos));
	memcpy(v, vel, sizeof(v));
	if (!tr_unstick(w, pos))
	{
		memcpy(out->point, org, sizeof(out->point));
		out->time = 0;
		return out->outcome = TR_LAND;
	}
	for (seg = 0; seg < TR_MAX_SEGS && t < TR_HORIZON; seg++)
	{
		float speed = sqrtf(tr_dot(v, v)), dt;
		dt = speed > 1 ? TR_SEG_LEN / speed : 0.25f;
		if (dt < TR_SEG_MIN_T)
			dt = TR_SEG_MIN_T;
		if (t + dt > TR_HORIZON)
			dt = TR_HORIZON - t;
		next[0] = pos[0] + v[0] * dt;
		next[1] = pos[1] + v[1] * dt;
		next[2] = pos[2] + v[2] * dt - 0.5f * g * dt * dt;
		r = tr_judge(w, pos, next, &done, at, normal);
		if (done)
		{
			float frac = tr_dist(pos, next) > 0.001f ? tr_dist(pos, at) / tr_dist(pos, next) : 0;
			memcpy(out->point, at, sizeof(at));
			out->time = t + dt * frac;
			return out->outcome = r;
		}
		if (tr_dist(at, next) > 0.5f)
		{
			// a wall or a ceiling: the player slides or drops from there
			float from[3];
			int o;
			from[0] = at[0] + normal[0] * 4;
			from[1] = at[1] + normal[1] * 4;
			from[2] = at[2] + (normal[2] < -0.3f ? -2 : 0);
			o = TouchRescue_Below(w, from, 0, out);
			out->time = t + dt;
			return o;
		}
		memcpy(pos, next, sizeof(pos));
		v[2] -= g * dt;
		t += dt;
		if (pos[2] < w->floor_z)
		{
			memcpy(out->point, pos, sizeof(pos));
			out->time = t;
			return out->outcome = TR_VOID;
		}
	}
	r = TouchRescue_Below(w, pos, 0, out);
	out->time = t;
	return r;
}

// A landing the player stays on: ground also TR_LIP_MARGIN further along the
// way they are moving, so they do not come down on the very lip and slide or
// carry on over it.
#define TR_LIP_MARGIN 32.0f

static int TouchRescue_LandsClear(tr_world_t *w, const tr_fall_t *fall, const float vel[3])
{
	float h = sqrtf(vel[0] * vel[0] + vel[1] * vel[1]), p[3];
	tr_fall_t f;
	if (fall->outcome != TR_LAND)
		return 0;
	if (h < 1)
		return 1;
	p[0] = fall->point[0] + vel[0] / h * TR_LIP_MARGIN;
	p[1] = fall->point[1] + vel[1] / h * TR_LIP_MARGIN;
	p[2] = fall->point[2] + 8;
	return TouchRescue_Below(w, p, 48, &f) == TR_LAND;
}

// ---------------------------------------------------------------------------
// The anchor: a point the hook can catch whose pull brings the player back
// over ground. Candidates are the first surface along rays fanned upward round
// the way back to where the player last stood (`home`); each must be solid
// (not sky), above the eye, and in reach. A candidate is good when the pull
// toward it carries the player's box, from where it will be when the hook
// catches, clear to a point with ground under it: a wall or pillar above a
// ledge, not the underside of the platform the player fell from nor a point
// whose line runs into the lip. One with no ground on the way still serves if
// it is well above the ledge the player left (home_z) and the box rises clear
// above it: the swing on the rope can carry them over. Among the good ones
// the nearest to the way home wins, then the shortest.
// ---------------------------------------------------------------------------
typedef struct tr_anchor_s
{
	float point[3];
	float normal[3];
	float dist;
	float score;
	int over_ground;  // ground is under the end of the pull
	int late_ok;      // the pull clears the lip even if the hook catches late
} tr_anchor_t;

// The search stops after this many traces (a frame's worth on a phone): the
// most promising rays come first.
#define TR_ANCHOR_BUDGET 320
// Anchors nearer than this are no use; one must be above the eye.
#define TR_ANCHOR_MIN_DIST   64.0f
#define TR_ANCHOR_ABOVE_EYE  16.0f
// Without ground under the pull, an anchor must be this far above home.
#define TR_ANCHOR_ABOVE_HOME 64.0f

static const float tr_fan_yaw[] = { 0, -20, 20, -45, 45, -80, 80, 135, -135, 180 };
static const float tr_fan_pitch[] = { 10, 22, 34, 46, 58, 70, 84 };

// Where the player will be when a hook sent `dist` away catches: the aim takes
// about TR_AIM_LEAD seconds, the hook flies at TR_HOOK_SPEED, and the round
// trip to the server can add TR_CATCH_SLACK. A pull that clears the lip from
// higher up can wedge from lower down, so a pull that clears it either way is
// preferred; one that only clears it from the early point is still tried
// (the rope swings rather than pulls straight, and a try costs less than the
// fall).
#define TR_AIM_LEAD    0.35f
#define TR_HOOK_SPEED  1800.0f
#define TR_CATCH_SLACK 0.2f
// The pull is followed in this many steps along the line to the anchor.
#define TR_PULL_STEPS  6

typedef struct tr_from_s
{
	float org[3];   // the player's origin now
	float vel[3];
	float eye_h;    // eye above origin
	float home_z;   // the height the player last stood at
} tr_from_t;

// Whether the pull toward anchor `a` can bring the player's box back over
// ground: from where the player will be when the hook catches, straight
// toward the anchor (the rope pulls the eye to it), the first point with
// ground within TR_RELEASE_DROP under it must be reachable with the box
// clear all the way. 2 when it is (ground), 1 when there is no ground on the
// way but the box rises clear above home (the swing may carry the player
// over), 0 when the box would wedge under a lip or hang below the ledge.
// A drop this long lands at under 900 u/s: no fall damage on the stock
// balance (g_balance_falldamage_minspeed).
#define TR_RELEASE_DROP    400.0f
static int tr_pull_reaches(tr_world_t *w, const tr_from_t *from, const float a[3], float dist, float slack)
{
	float p[3], target[3], d[3], q[3];
	float t = TR_AIM_LEAD + dist / TR_HOOK_SPEED + slack;
	tr_trace_t tr;
	tr_fall_t f;
	int i, risen = 0;
	p[0] = from->org[0] + from->vel[0] * t;
	p[1] = from->org[1] + from->vel[1] * t;
	p[2] = from->org[2] + from->vel[2] * t - 0.5f * w->gravity * t * t;
	// the origin ends below the anchor by the eye height
	target[0] = a[0];
	target[1] = a[1];
	target[2] = a[2] - from->eye_h;
	d[0] = target[0] - p[0];
	d[1] = target[1] - p[1];
	d[2] = target[2] - p[2];
	for (i = 1; i <= TR_PULL_STEPS; i++)
	{
		tr_ma(p, (float)i / TR_PULL_STEPS, d, q);
		if (TouchRescue_Below(w, q, TR_RELEASE_DROP, &f) == TR_LAND)
		{
			tr_trace(w, p, q, 1, &tr);
			return !tr.startsolid && tr.fraction >= 0.98f ? 2 : 0;
		}
		if (!risen && q[2] > from->home_z + 32)
		{
			tr_trace(w, p, q, 1, &tr);
			risen = !tr.startsolid && tr.fraction >= 0.98f;
			if (!risen)
				return 0;
		}
	}
	return risen;
}

// One candidate ray: the first surface along it, scored. Returns 1 when it
// beat *best.
static int tr_try_anchor(tr_world_t *w, const tr_from_t *from, const float eye[3], const float dir[3], float range, float bonus, tr_anchor_t *best)
{
	float end[3], dist, score;
	tr_trace_t tr;
	int reach, late_ok;
	tr_ma(eye, range, dir, end);
	tr_trace(w, eye, end, 0, &tr);
	if (tr.startsolid || tr.fraction >= 1 || tr.hit != TR_HIT_SOLID)
		return 0; // nothing in reach, or sky that drops the hook
	dist = tr_dist(eye, tr.endpos);
	if (dist < TR_ANCHOR_MIN_DIST || tr.endpos[2] < eye[2] + TR_ANCHOR_ABOVE_EYE)
		return 0;
	if (tr.endpos[2] < from->home_z + TR_ANCHOR_ABOVE_HOME && best->over_ground)
		return 0; // cannot beat what we have
	reach = tr_pull_reaches(w, from, tr.endpos, dist, 0);
	if (!reach || (reach == 1 && tr.endpos[2] < from->home_z + TR_ANCHOR_ABOVE_HOME))
		return 0; // it would wedge the player under the lip or leave them hanging
	// ground first, then a pull that clears the lip even if the hook is late,
	// then the way home, then near
	score = (reach == 2 ? 1000.0f : 0) + bonus - dist * 0.1f;
	if (score + 500.0f <= best->score)
		return 0;
	late_ok = tr_pull_reaches(w, from, tr.endpos, dist, TR_CATCH_SLACK) == reach;
	if (late_ok)
		score += 500.0f;
	if (score <= best->score)
		return 0;
	memcpy(best->point, tr.endpos, sizeof(best->point));
	memcpy(best->normal, tr.normal, sizeof(best->normal));
	best->dist = dist;
	best->score = score;
	best->over_ground = reach == 2;
	best->late_ok = late_ok;
	return 1;
}

// Heights above home that the column of rays aims at.
static const float tr_column_h[] = { 48, 128, 256, 400, 600, 900, 1300 };

// Rays from the eye at points above home (where the player last stood: a
// wall, pillar or ceiling over the ledge is the anchor a player would pick),
// then a fan upward round the way home (base_yaw, degrees) for anything
// else. home_z is the height the player last stood at; range how far a hook
// is worth sending. Returns 1 with the best anchor, 0 when nothing catchable
// pulls the player back up.
static int TouchRescue_FindAnchor(tr_world_t *w, const tr_from_t *from, const float eye[3], const float home[3], float base_yaw, float range, tr_anchor_t *best)
{
	int i, j, found = 0;
	best->score = -1e9f;
	best->over_ground = 0;
	for (i = 0; i < (int)(sizeof(tr_column_h) / sizeof(tr_column_h[0])); i++)
	{
		float dir[3], len;
		dir[0] = home[0] - eye[0];
		dir[1] = home[1] - eye[1];
		dir[2] = home[2] + tr_column_h[i] - eye[2];
		len = sqrtf(tr_dot(dir, dir));
		if (len < 1)
			continue;
		dir[0] /= len;
		dir[1] /= len;
		dir[2] /= len;
		found |= tr_try_anchor(w, from, eye, dir, range, 0, best);
		if (w->traces >= TR_ANCHOR_BUDGET)
			return found;
	}
	for (i = 0; i < (int)(sizeof(tr_fan_yaw) / sizeof(tr_fan_yaw[0])); i++)
	{
		for (j = 0; j < (int)(sizeof(tr_fan_pitch) / sizeof(tr_fan_pitch[0])); j++)
		{
			float yaw = (base_yaw + tr_fan_yaw[i]) * TR_DEG2RAD;
			float pitch = tr_fan_pitch[j] * TR_DEG2RAD;
			float dir[3];
			dir[0] = cosf(pitch) * cosf(yaw);
			dir[1] = cosf(pitch) * sinf(yaw);
			dir[2] = sinf(pitch);
			found |= tr_try_anchor(w, from, eye, dir, range, -fabsf(tr_fan_yaw[i]) * 2.0f - 20.0f, best);
			if (w->traces >= TR_ANCHOR_BUDGET)
				return found;
		}
	}
	return found;
}

// ---------------------------------------------------------------------------
// The rescue, one step per client frame.
//
// IDLE     falling (moving down) toward the void or a death zone for
//          TR_CONFIRM seconds, with a hook to hand and not hooking by hand:
//          look for an anchor (out.find_anchor) and go to AIM.
// AIM      turn the view to the anchor (out.aim); fire once it is on it.
// HOLD     hold the hook (out.hold); until it has left (a switch to the Hook
//          weapon first) keep the view on the anchor. Then let go once there is
//          ground under the player within TR_RELEASE_DROP and the fall would
//          end on ground, or on landing; give up when the pull has not
//          started within the attach time, after TR_MAX_HOLD, or when the
//          hook is gone.
// RESTORE  the view goes back to where the player was facing (out.restore),
//          unless they turned it themselves meanwhile.
// COOLDOWN nothing for the cooldown, so a rescue cannot flap; it ends early
//          once the player has stood on ground for TR_GROUND_RESET, so a
//          slide off the ledge they were put on can be caught again.
//
// The player always wins: their own hook press takes over at once (the hold
// passes to their finger); a hard turn of the view while the rescue is aiming
// cancels it; any turn later means the view is theirs and stays where it is.
// ---------------------------------------------------------------------------
#define TR_IDLE     0
#define TR_AIM      1
#define TR_HOLD     2
#define TR_RESTORE  3
#define TR_COOLDOWN 4

static const char *const tr_phase_name[] = { "idle", "aim", "hold", "restore", "cooldown" };

// Falling at least this fast (u/s) before the fall counts.
#define TR_MIN_FALL_SPEED  80.0f
// The bad prediction must hold this long (seconds): one stray trace is not a fall.
#define TR_CONFIRM         0.06f
// Aim: fire once the view is this close to the anchor (degrees); give up after
// TR_AIM_TIMEOUT. A player turn faster than TR_TAKEOVER_RATE (deg/s) cancels.
#define TR_FIRE_TOL        2.0f
#define TR_AIM_TIMEOUT     0.7f
#define TR_TAKEOVER_RATE   120.0f
// A player turn faster than this (deg/s) after the aim keeps the view theirs.
#define TR_TURN_NOTICE     20.0f
// The hook has caught once the player has gained this much speed toward the
// anchor beyond free fall (u/s).
#define TR_ATTACH_SPEED    200.0f
// Let go when ground is TR_RELEASE_DROP or closer under the player and the
// fall ends on it.
#define TR_MAX_HOLD        4.0f
// Caught but stuck (moving slower than TR_STUCK_SPEED, not over ground) this
// long: wedged under a lip; let go rather than hang. The speed is the
// position's, not the velocity's, which the rope keeps up while wedged.
#define TR_STUCK_SPEED     80.0f
#define TR_STUCK_TIME      1.0f
// The hook must leave within this long of the decision (a weapon switch).
#define TR_SWITCH_TIMEOUT  1.2f
// The view goes back within this long, or is left where it is.
#define TR_RESTORE_TIME    0.6f
#define TR_GROUND_RESET    0.2f
#define TR_RESTORE_DONE    0.5f

typedef struct tr_frame_s
{
	double now;
	int allowed;        // touch mode, alive, playing, the rescue switched on
	int available;      // a hook can be fired now
	int manual;         // the player is hooking by hand
	int hook_out;       // HOLD: the hook has been sent (the weapon is in hand)
	int onground;
	float vz;           // vertical velocity
	int fall;           // where the fall ends (TR_LAND / TR_VOID / TR_DEATH), -1 not known
	int anchor;         // AIM: an anchor was found
	float aim_err;      // AIM: degrees from the view to the anchor; RESTORE: to the saved view
	float turn_rate;    // the player's own view turn this frame, deg/s
	float pull_speed;   // HOLD: speed toward the anchor gained beyond free fall since the shot, u/s
	float speed;        // HOLD: how fast the player's position changes, u/s
	float attach_time;  // HOLD: seconds the hook may take to catch
	float ground_below; // HOLD: distance down to ground, < 0 none within TR_RELEASE_DROP
	float cooldown;     // seconds
} tr_frame_t;

typedef struct tr_state_s
{
	int phase;
	double since;       // the phase began
	double void_since;  // the fall was first judged bad (0 = it is not)
	double fired;       // the hook was asked for
	double out;         // the hook left (0 = not yet)
	double attached;    // the pull was first seen (0 = not yet)
	double ground_since; // COOLDOWN: standing since (0 = in the air)
	double stuck_since; // HOLD: caught but not moving since (0 = moving)
	int player_turned;  // the player turned the view after the aim
	const char *why;    // the reason for the last change, for the debug line
} tr_state_t;

typedef struct tr_out_s
{
	int hold;           // hold the hook this frame
	int prepare;        // AIM: get the hook ready (switch to the Hook weapon)
	int aim;            // turn the view toward the anchor
	int restore;        // turn the view back
	int find_anchor;    // look for an anchor now (entering AIM)
	int changed;        // the phase changed this step (see state.why)
} tr_out_t;

static void tr_go(tr_state_t *s, tr_out_t *o, int phase, double now, const char *why)
{
	s->phase = phase;
	s->since = now;
	s->why = why;
	o->changed = 1;
}

static void TouchRescue_Reset(tr_state_t *s)
{
	memset(s, 0, sizeof(*s));
	s->phase = TR_IDLE;
	s->why = "";
}

static void TouchRescue_Step(tr_state_t *s, const tr_frame_t *f, tr_out_t *o)
{
	memset(o, 0, sizeof(*o));
	switch (s->phase)
	{
	case TR_IDLE:
		if (!f->allowed || !f->available || f->manual || f->onground || f->vz > -TR_MIN_FALL_SPEED
			|| (f->fall != TR_VOID && f->fall != TR_DEATH))
		{
			s->void_since = 0;
			break;
		}
		if (!s->void_since)
			s->void_since = f->now;
		if (f->now - s->void_since < TR_CONFIRM)
			break;
		s->player_turned = 0;
		s->attached = 0;
		s->stuck_since = 0;
		s->out = 0;
		o->find_anchor = 1;
		o->prepare = 1;
		tr_go(s, o, TR_AIM, f->now, f->fall == TR_DEATH ? "falling into a death zone" : "falling into the void");
		break;
	case TR_AIM:
		if (!f->allowed)
			tr_go(s, o, TR_COOLDOWN, f->now, "no longer allowed (dead, menu or keyboard)");
		else if (f->manual)
			tr_go(s, o, TR_COOLDOWN, f->now, "the player is hooking by hand");
		else if (!f->available)
			tr_go(s, o, TR_COOLDOWN, f->now, "no hook to fire");
		else if (f->onground)
			tr_go(s, o, TR_COOLDOWN, f->now, "landed");
		else if (f->fall == TR_LAND)
			tr_go(s, o, TR_COOLDOWN, f->now, "the fall ends on ground after all");
		else if (!f->anchor)
			tr_go(s, o, TR_COOLDOWN, f->now, "nothing the hook can catch");
		else if (f->turn_rate > TR_TAKEOVER_RATE)
			tr_go(s, o, TR_COOLDOWN, f->now, "the player turned the view");
		else if (f->now - s->since > TR_AIM_TIMEOUT)
			tr_go(s, o, TR_RESTORE, f->now, "could not aim in time");
		else if (f->aim_err <= TR_FIRE_TOL)
		{
			s->fired = f->now;
			o->hold = 1;
			tr_go(s, o, TR_HOLD, f->now, "on the anchor: hook fired");
		}
		else
		{
			o->aim = 1;
			o->prepare = 1;
		}
		break;
	case TR_HOLD:
		if (!s->out && f->hook_out)
			s->out = f->now;
		if (f->turn_rate > TR_TURN_NOTICE)
			s->player_turned = 1;
		if (!s->attached && f->pull_speed > TR_ATTACH_SPEED)
			s->attached = f->now;
		if (!s->attached || f->speed >= TR_STUCK_SPEED || f->ground_below >= 0)
			s->stuck_since = 0;
		else if (!s->stuck_since)
			s->stuck_since = f->now;
		if (!f->allowed)
			tr_go(s, o, TR_COOLDOWN, f->now, "no longer allowed (dead, menu or keyboard)");
		else if (f->manual)
			tr_go(s, o, TR_COOLDOWN, f->now, "the player is hooking by hand");
		else if (f->onground)
			tr_go(s, o, TR_RESTORE, f->now, "landed");
		else if (!f->available)
			tr_go(s, o, TR_RESTORE, f->now, "the hook is gone (no fuel)");
		else if (!s->out && f->now - s->fired > TR_SWITCH_TIMEOUT)
			tr_go(s, o, TR_RESTORE, f->now, "the hook was never fired");
		else if (s->out && !s->attached && f->now - s->out > f->attach_time)
			tr_go(s, o, TR_RESTORE, f->now, "the hook did not catch");
		else if (s->attached && f->ground_below >= 0 && f->ground_below <= TR_RELEASE_DROP && f->fall == TR_LAND)
			tr_go(s, o, TR_RESTORE, f->now, "back over ground");
		else if (s->stuck_since && f->now - s->stuck_since > TR_STUCK_TIME)
			tr_go(s, o, TR_RESTORE, f->now, "stuck under a ledge");
		else if (f->now - s->fired > TR_MAX_HOLD)
			tr_go(s, o, TR_RESTORE, f->now, "held too long");
		else
		{
			o->hold = 1;
			o->aim = !s->out; // the anchor moves in the view as the player falls
		}
		break;
	case TR_RESTORE:
		if (f->turn_rate > TR_TURN_NOTICE)
			s->player_turned = 1;
		if (!f->allowed || s->player_turned || f->aim_err <= TR_RESTORE_DONE || f->now - s->since > TR_RESTORE_TIME)
			tr_go(s, o, TR_COOLDOWN, f->now, s->player_turned ? "view left where the player put it" : "view handed back");
		else
			o->restore = 1;
		break;
	default:
		if (!f->onground)
			s->ground_since = 0;
		else if (!s->ground_since)
			s->ground_since = f->now;
		if (f->now - s->since >= f->cooldown || (s->ground_since && f->now - s->ground_since >= TR_GROUND_RESET))
		{
			s->void_since = 0;
			s->ground_since = 0;
			tr_go(s, o, TR_IDLE, f->now, "ready");
		}
		break;
	}
}

#endif
