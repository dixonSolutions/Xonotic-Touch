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
The decisions behind the touch ledge guard (touch_ledge.c): a thumb on a
virtual stick overshoots, and on a floating map one step too far is a death.
While the player stands on the ground, the move their stick asks for is flown
ahead (touch_dodge.h's ground physics) and the ground under each point of it
looked for with the player's own box (touch_rescue.h's drop): the player only
falls once the whole box is off the floor, so a walk along a narrow bridge is
left alone. When the path runs off into the void or a death zone (or, in the
stricter mode, any drop deeper than a limit), the move is changed, in this
order of preference:

  1. turned up to 45 degrees, the side that keeps the most of the run: a
     player angled at the edge slides along it;
  2. the same direction at less speed: they creep up to the edge;
  3. no input: friction stops them;
  4. against their sliding: a brake, the best that is left.

A jump is never held back: the player means to clear the gap (the includer
skips the guard while jump is held, and in the air). Nor is a walk onto a
jump pad or into a teleporter (tl_opts_t.launch), however much void lies
under it.

Plain C with no engine types, like touch_rescue.h: scripts/test-touch-ledge.sh
runs it against a world of boxes. The includer supplies qbool, true and false.
*/

#ifndef TOUCH_LEDGE_H
#define TOUCH_LEDGE_H

#include "touch_dodge.h"

#define TL_MODE_OFF    0
#define TL_MODE_DEADLY 1 // the void and death zones only
#define TL_MODE_DROPS  2 // also any drop deeper than tl_opts_t.drop

#define TL_CLEAR 0 // the player's own move is safe
#define TL_STEER 1 // turned along the edge
#define TL_SLOW  2 // the same way, slower
#define TL_STOP  3 // no input
#define TL_BRAKE 4 // input against the slide

static const char *const tl_verdict_name[] = { "clear", "steer", "slow", "stop", "brake" };

// What lies past the edge.
#define TL_EDGE_VOID  TR_VOID
#define TL_EDGE_DEATH TR_DEATH
#define TL_EDGE_DROP  3
static const char *const tl_edge_name[] = { "ground", "void", "death zone", "long drop" };

typedef struct tl_opts_s
{
	int mode;         // TL_MODE_*
	float drop;       // TL_MODE_DROPS: deepest drop let through, units
	float lookahead;  // seconds of the move flown ahead (latency + stopping)
	// Whether the player's box standing at p is in a jump pad or teleporter:
	// the map takes over from there, so the path counts as safe. May be NULL.
	int (*launch)(void *ctx, const float p[3]);
	void *launch_ctx;
} tl_opts_t;

typedef struct tl_result_s
{
	int verdict;      // TL_*
	float dir[2];     // the move to use: a unit horizontal direction...
	float mag;        // ...and its share of full speed (0..1)
	int edge;         // TL_EDGE_* found on the player's own path (TL_CLEAR: 0)
	float edge_at[3]; // where on it
	int tried;        // candidate moves flown
} tl_result_t;

// Points sampled along a flown path: judged from step height, so a stair up
// is not a wall and a stair down is not a drop.
#define TL_STEP_UP   20.0f
#define TL_SLOW_N    3
static const float tl_slow[TL_SLOW_N] = { 0.6f, 0.35f, 0.15f };
#define TL_TURN_N    2
#define TL_TURN_DEG  22.5f

// What is under a box standing at q (raised by TL_STEP_UP): 0 ground,
// else TL_EDGE_*.
static int tl_under(tr_world_t *w, const float q[3], float stand_z, const tl_opts_t *o, float at[3])
{
	tr_fall_t fall;
	int r = TouchRescue_Below(w, q, 0, &fall);
	memcpy(at, fall.point, sizeof(fall.point));
	if (r != TR_LAND)
		return r;
	if (o->mode >= TL_MODE_DROPS && stand_z - fall.point[2] > o->drop)
		return TL_EDGE_DROP;
	return 0;
}

// Whether the path the player takes with this plan stays over ground: 0 safe,
// else TL_EDGE_* with the point in `at`. The path stops at the first wall
// (the player stops there too).
static int tl_path_edge(tr_world_t *w, const td_player_t *p, const td_move_t *m, const td_plan_t *plan, int steps, const tl_opts_t *o, float at[3])
{
	float path[TD_MAX_STEPS][3], prev[3], q[3], under[3];
	int i, e;
	td_player_path(p, m, plan, steps, path);
	memcpy(prev, p->org, sizeof(prev));
	prev[2] += TL_STEP_UP;
	for (i = 1; i < steps; i += 2)
	{
		tr_trace_t tr;
		int last = i + 2 >= steps;
		if (last)
			i = steps - 1;
		q[0] = path[i][0];
		q[1] = path[i][1];
		q[2] = p->org[2] + TL_STEP_UP;
		if (o->launch && o->launch(o->launch_ctx, q))
			return 0; // onto a jump pad or into a teleporter: the map carries them on
		tr_trace(w, prev, q, 1, &tr);
		if (tr.startsolid)
			return 0; // no answer from inside something: leave the player be
		if (tr.fraction < 1)
		{
			// a wall: the player stops against it; judge the spot they reach
			memcpy(q, tr.endpos, sizeof(q));
			e = tl_under(w, q, p->org[2], o, under);
			if (e)
				memcpy(at, q, sizeof(q));
			return e;
		}
		e = tl_under(w, q, p->org[2], o, under);
		if (e)
		{
			memcpy(at, q, sizeof(q));
			return e;
		}
		memcpy(prev, q, sizeof(prev));
	}
	return 0;
}

static int tl_try(tr_world_t *w, const td_player_t *p, const td_move_t *m, const float dir[2], float mag, int steps, const tl_opts_t *o, tl_result_t *r)
{
	td_move_t mm = *m;
	td_plan_t plan;
	float at[3];
	memset(&plan, 0, sizeof(plan));
	plan.dir[0] = mag > 0 ? dir[0] : 0;
	plan.dir[1] = mag > 0 ? dir[1] : 0;
	plan.name = "ledge guard";
	mm.maxspeed = m->maxspeed * (mag > 0 ? mag : 1);
	r->tried++;
	return tl_path_edge(w, p, &mm, &plan, steps, o, at) == 0;
}

// The move to make instead of the player's own (own_dir unit, own_mag 0..1).
static void TouchLedge_Guard(tr_world_t *w, const td_player_t *p, const td_move_t *m, const float own_dir[2], float own_mag, const tl_opts_t *o, tl_result_t *r)
{
	td_plan_t plan;
	td_move_t mm = *m;
	int steps, k, s;
	float vh[2], speed;
	memset(r, 0, sizeof(*r));
	r->verdict = TL_CLEAR;
	r->dir[0] = own_dir[0];
	r->dir[1] = own_dir[1];
	r->mag = own_mag;
	if (o->mode <= TL_MODE_OFF || !p->onground)
		return;
	steps = (int)(o->lookahead / TD_STEP + 0.5f);
	if (steps < 4)
		steps = 4;
	if (steps > TD_MAX_STEPS)
		steps = TD_MAX_STEPS;
	if (own_mag < 0.05f)
		own_mag = 0;
	// the player's own move
	memset(&plan, 0, sizeof(plan));
	plan.dir[0] = own_mag > 0 ? own_dir[0] : 0;
	plan.dir[1] = own_mag > 0 ? own_dir[1] : 0;
	mm.maxspeed = m->maxspeed * (own_mag > 0 ? own_mag : 1);
	r->tried++;
	r->edge = tl_path_edge(w, p, &mm, &plan, steps, o, r->edge_at);
	if (!r->edge)
		return;
	vh[0] = p->vel[0];
	vh[1] = p->vel[1];
	speed = sqrtf(vh[0] * vh[0] + vh[1] * vh[1]);
	if (own_mag > 0)
	{
		// 1. turned along the edge, the side nearer the way they slide first
		for (k = 1; k <= TL_TURN_N; k++)
		{
			float a = k * TL_TURN_DEG * TR_DEG2RAD, c = cosf(a), sn = sinf(a);
			float d[2][2];
			int first;
			d[0][0] = own_dir[0] * c - own_dir[1] * sn;
			d[0][1] = own_dir[0] * sn + own_dir[1] * c;
			d[1][0] = own_dir[0] * c + own_dir[1] * sn;
			d[1][1] = -own_dir[0] * sn + own_dir[1] * c;
			first = d[1][0] * vh[0] + d[1][1] * vh[1] > d[0][0] * vh[0] + d[0][1] * vh[1] ? 1 : 0;
			for (s = 0; s < 2; s++)
			{
				const float *dd = d[s ? !first : first];
				if (tl_try(w, p, m, dd, own_mag, steps, o, r))
				{
					r->verdict = TL_STEER;
					r->dir[0] = dd[0];
					r->dir[1] = dd[1];
					return;
				}
			}
		}
		// 2. the same way, slower
		for (k = 0; k < TL_SLOW_N; k++)
		{
			float mag = own_mag * tl_slow[k];
			if (tl_try(w, p, m, own_dir, mag, steps, o, r))
			{
				r->verdict = TL_SLOW;
				r->mag = mag;
				return;
			}
		}
	}
	// 3. no input
	if (own_mag > 0 && tl_try(w, p, m, own_dir, 0, steps, o, r))
	{
		r->verdict = TL_STOP;
		r->mag = 0;
		return;
	}
	// 4. a brake against the slide (or nothing, standing still)
	r->verdict = speed > 1 ? TL_BRAKE : TL_STOP;
	r->mag = speed > 1 ? 1 : 0;
	if (speed > 1)
	{
		r->dir[0] = -vh[0] / speed;
		r->dir[1] = -vh[1] / speed;
	}
}

// ---------------------------------------------------------------------------
// In the air
// ---------------------------------------------------------------------------
// A thumb that keeps pushing after a jump pad, a step off a ramp or a jump
// can steer the player past the platform they would have landed on. In the
// air the input is flown for TL_AIR_CONTROL seconds and the arc from there
// followed to where it comes down (touch_rescue.h). Only when the player's
// own input comes down in the void or a death zone and another move does not
// is anything changed: a jump that lands is left alone, and so is one that
// nothing saves (the fall rescue's turn).
#define TL_AIR_CONTROL 0.4f
// The arc after it is traced in pieces of at most this length: the fall
// rescue's longer chords cut under the lip of a platform the arc clears.
#define TL_ARC_SEG     128.0f
#define TL_ARC_MAX     28

// Where the arc from org at vel comes down: TR_LAND, TR_VOID or TR_DEATH.
static int tl_arc(tr_world_t *w, const float org[3], const float vel[3], float at[3])
{
	float pos[3], v[3], next[3], normal[3], t = 0, g = w->gravity > 0 ? w->gravity : 800;
	int seg, done, r;
	tr_fall_t fall;
	memcpy(pos, org, sizeof(pos));
	memcpy(v, vel, sizeof(v));
	if (!tr_unstick(w, pos))
	{
		memcpy(at, org, sizeof(pos));
		return TR_LAND; // inside something: not a fall into the void
	}
	for (seg = 0; seg < TL_ARC_MAX && t < TR_HORIZON; seg++)
	{
		float speed = sqrtf(tr_dot(v, v)), dt = speed > 1 ? TL_ARC_SEG / speed : 0.1f;
		if (dt < 0.03f)
			dt = 0.03f;
		if (dt > 0.2f)
			dt = 0.2f;
		next[0] = pos[0] + v[0] * dt;
		next[1] = pos[1] + v[1] * dt;
		next[2] = pos[2] + v[2] * dt - 0.5f * g * dt * dt;
		r = tr_judge(w, pos, next, &done, at, normal);
		if (done)
			return r;
		if (tr_dist(at, next) > 0.5f)
		{
			// a wall or a ceiling: slide or drop from there
			float from[3];
			from[0] = at[0] + normal[0] * 4;
			from[1] = at[1] + normal[1] * 4;
			from[2] = at[2] + (normal[2] < -0.3f ? -2 : 0);
			r = TouchRescue_Below(w, from, 0, &fall);
			memcpy(at, fall.point, sizeof(fall.point));
			return r;
		}
		memcpy(pos, next, sizeof(pos));
		v[2] -= g * dt;
		t += dt;
		if (pos[2] < w->floor_z)
		{
			memcpy(at, pos, sizeof(pos));
			return TR_VOID;
		}
	}
	r = TouchRescue_Below(w, pos, 0, &fall);
	memcpy(at, fall.point, sizeof(fall.point));
	return r;
}

// Where the player comes down with this plan held for `steps`: TR_LAND,
// TR_VOID or TR_DEATH.
static int tl_air_outcome(tr_world_t *w, const td_player_t *p, const td_move_t *m, const td_plan_t *plan, int steps, const tl_opts_t *o, float at[3])
{
	float path[TD_MAX_STEPS][3], prev[3], vel[3];
	tr_trace_t tr;
	int i, r;
	td_player_path(p, m, plan, steps, path);
	memcpy(prev, p->org, sizeof(prev));
	for (i = 1; i < steps; i += 2)
	{
		int last = i + 2 >= steps;
		if (last)
			i = steps - 1;
		if (o->launch && o->launch(o->launch_ctx, path[i]))
			return TR_LAND;
		tr_trace(w, prev, path[i], 1, &tr);
		if (!tr.startsolid && tr_death(w, prev, path[i]) <= tr.fraction)
		{
			memcpy(at, path[i], sizeof(path[i]));
			return TR_DEATH; // into a hurt zone while steering
		}
		if (!tr.startsolid && tr.fraction < 1)
		{
			float d[3];
			d[0] = path[i][0] - prev[0];
			d[1] = path[i][1] - prev[1];
			d[2] = path[i][2] - prev[2];
			if (tr_dot(d, tr.normal) >= 0)
			{
				// a surface the box only touches and moves away from (the
				// floor a jump leaves)
				memcpy(prev, path[i], sizeof(prev));
				continue;
			}
		}
		if (!tr.startsolid && tr.fraction < 1)
		{
			// met something while steering: a floor is a landing, else the
			// arc carries on from there
			int k;
			if (tr.hit == TR_HIT_DEATH)
			{
				memcpy(at, tr.endpos, sizeof(tr.endpos));
				return TR_DEATH;
			}
			if (tr.hit == TR_HIT_WATER || tr.normal[2] >= TR_FLOOR_NZ)
				return TR_LAND;
			for (k = 0; k < 3; k++)
				vel[k] = (path[i][k] - path[i - 1][k]) / TD_STEP;
			// off the wall, keeping only the part of the motion along it
			{
				float into = tr_dot(vel, tr.normal), from[3];
				if (into < 0)
					for (k = 0; k < 3; k++)
						vel[k] -= into * tr.normal[k];
				tr_ma(tr.endpos, 2, tr.normal, from);
				return tl_arc(w, from, vel, at);
			}
		}
		memcpy(prev, path[i], sizeof(prev));
	}
	for (i = 0; i < 3; i++)
		vel[i] = (path[steps - 1][i] - path[steps - 2][i]) / TD_STEP;
	vel[2] -= 0.5f * m->gravity * TD_STEP; // the difference is half a step behind
	r = tl_arc(w, path[steps - 1], vel, at);
	if (r != TR_LAND && o->launch && o->launch(o->launch_ctx, at))
		return TR_LAND;
	return r;
}

static int tl_air_try(tr_world_t *w, const td_player_t *p, const td_move_t *m, const float dir[2], float mag, int steps, const tl_opts_t *o, tl_result_t *r)
{
	td_move_t mm = *m;
	td_plan_t plan;
	float at[3];
	memset(&plan, 0, sizeof(plan));
	plan.dir[0] = mag > 0 ? dir[0] : 0;
	plan.dir[1] = mag > 0 ? dir[1] : 0;
	plan.name = "ledge guard";
	mm.maxspeed = m->maxspeed * (mag > 0 ? mag : 1);
	if (mm.maxairspeed > 0 && mag > 0)
		mm.maxairspeed *= mag;
	r->tried++;
	return tl_air_outcome(w, p, &mm, &plan, steps, o, at) == TR_LAND;
}

// The move to make in the air instead of the player's own.
static void TouchLedge_GuardAir(tr_world_t *w, const td_player_t *p, const td_move_t *m, const float own_dir[2], float own_mag, const tl_opts_t *o, tl_result_t *r)
{
	int steps = (int)(TL_AIR_CONTROL / TD_STEP + 0.5f), k, s, e;
	float vh[2], speed;
	td_player_t pa = *p;
	memset(r, 0, sizeof(*r));
	r->verdict = TL_CLEAR;
	r->dir[0] = own_dir[0];
	r->dir[1] = own_dir[1];
	r->mag = own_mag;
	if (o->mode <= TL_MODE_OFF || p->onground)
		return;
	pa.onground = 0;
	if (own_mag < 0.05f)
		own_mag = 0;
	{
		td_move_t mm = *m;
		td_plan_t plan;
		memset(&plan, 0, sizeof(plan));
		plan.dir[0] = own_mag > 0 ? own_dir[0] : 0;
		plan.dir[1] = own_mag > 0 ? own_dir[1] : 0;
		mm.maxspeed = m->maxspeed * (own_mag > 0 ? own_mag : 1);
		if (mm.maxairspeed > 0 && own_mag > 0)
			mm.maxairspeed *= own_mag;
		r->tried++;
		e = tl_air_outcome(w, &pa, &mm, &plan, steps, o, r->edge_at);
	}
	if (e == TR_LAND)
		return;
	vh[0] = p->vel[0];
	vh[1] = p->vel[1];
	speed = sqrtf(vh[0] * vh[0] + vh[1] * vh[1]);
	if (own_mag > 0)
	{
		for (k = 1; k <= TL_TURN_N; k++)
		{
			float a = k * TL_TURN_DEG * TR_DEG2RAD, c = cosf(a), sn = sinf(a);
			float d[2][2];
			d[0][0] = own_dir[0] * c - own_dir[1] * sn;
			d[0][1] = own_dir[0] * sn + own_dir[1] * c;
			d[1][0] = own_dir[0] * c + own_dir[1] * sn;
			d[1][1] = -own_dir[0] * sn + own_dir[1] * c;
			for (s = 0; s < 2; s++)
				if (tl_air_try(w, &pa, m, d[s], own_mag, steps, o, r))
				{
					r->verdict = TL_STEER;
					r->edge = e;
					r->dir[0] = d[s][0];
					r->dir[1] = d[s][1];
					return;
				}
		}
		if (tl_air_try(w, &pa, m, own_dir, 0, steps, o, r))
		{
			r->verdict = TL_STOP;
			r->edge = e;
			r->mag = 0;
			return;
		}
	}
	if (speed > 1)
	{
		float back[2];
		back[0] = -vh[0] / speed;
		back[1] = -vh[1] / speed;
		if (tl_air_try(w, &pa, m, back, 1, steps, o, r))
		{
			r->verdict = TL_BRAKE;
			r->edge = e;
			r->dir[0] = back[0];
			r->dir[1] = back[1];
			r->mag = 1;
			return;
		}
	}
	// nothing comes down on ground: the player's own input stands
}

#endif
