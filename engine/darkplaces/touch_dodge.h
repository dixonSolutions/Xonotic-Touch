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
The decisions behind touch auto dodge (touch_dodge.c): how much damage an
incoming projectile will do if the player carries on, and which sidestep
does better.

A projectile is followed along its flight (straight, or under its gravity)
until it meets the world or the warning window runs out; the player is
followed along each candidate escape with a simple model of Xonotic's
movement (ground acceleration and friction, a jump, the dodging mutator's
burst). Damage is the projectile's full damage if it passes through the
player's box, else its splash where it lands against the box's nearest
point (Xonotic's linear fall-off from core to edge damage at the radius).
An escape must keep ground under the player all the way (touch_rescue.h's
judgement: never off a ledge into the void or lava) and not run into a wall.

Plain C against touch_rescue.h's world callbacks; scripts/test-touch-dodge.sh
runs it against a world of boxes. Only touch_dodge.c includes it in the
engine; the includer supplies qbool, true and false.
*/

#ifndef TOUCH_DODGE_H
#define TOUCH_DODGE_H

#include "touch_rescue.h"

// A projectile as the client sees it.
typedef struct td_proj_s
{
	float org[3];
	float vel[3];
	float gravity;      // u/s^2 pulling it down (0: flies straight)
	float damage;       // on a direct hit
	float edge;         // at the edge of the blast
	float radius;       // blast radius
	float size;         // half the projectile's box (a near miss still touches)
	int world_blast;    // explodes where it meets the world (else bounces on: only a hit counts)
} td_proj_t;

// What a stock weapon's projectile does (the 0.8.6 / current stock balance;
// see RESEARCH-weapons.md). kind: csqcprojectile_type. Returns 0 for one not
// worth dodging (too fast to see coming, no damage, or homing).
#define TD_KIND_ELECTRO          1
#define TD_KIND_ROCKET           2
#define TD_KIND_CRYLINK          5
#define TD_KIND_ELECTRO_BEAM     6
#define TD_KIND_GRENADE          7
#define TD_KIND_GRENADE_BOUNCING 8
#define TD_KIND_HAGAR            17
#define TD_KIND_HAGAR_BOUNCING   18
#define TD_KIND_CRYLINK_BOUNCING 20
#define TD_KIND_FIREBALL         21
#define TD_KIND_ARC_BOLT         35
#define TD_KIND_RPC              60

typedef struct td_kind_s { int kind; const char *name; float damage, edge, radius, size; int world_blast; } td_kind_t;

static const td_kind_t td_kinds[] = {
	{ TD_KIND_ROCKET,           "rocket",         80,  40,  110, 4, 1 },
	{ TD_KIND_GRENADE,          "mortar grenade", 55,  25,  120, 3, 1 },
	{ TD_KIND_GRENADE_BOUNCING, "mortar grenade", 55,  30,  120, 3, 0 }, // bounces; blows on a player
	{ TD_KIND_ELECTRO_BEAM,     "electro bolt",   40,  20,  100, 3, 1 },
	{ TD_KIND_ELECTRO,          "electro orb",    30,  15,  150, 3, 0 }, // bounces; blows on a player
	{ TD_KIND_HAGAR,            "hagar rocket",   25,  12.5f, 65, 2, 1 },
	{ TD_KIND_HAGAR_BOUNCING,   "hagar rocket",   35,  17.5f, 80, 2, 1 },
	{ TD_KIND_CRYLINK,          "crylink",        10,  5,   80,  2, 1 },
	{ TD_KIND_CRYLINK_BOUNCING, "crylink",        10,  5,   80,  2, 1 },
	{ TD_KIND_ARC_BOLT,         "arc bolt",       25,  12.5f, 65, 2, 1 },
	{ TD_KIND_FIREBALL,         "fireball",       200, 50,  200, 16, 1 },
	{ TD_KIND_RPC,              "rpc rocket",     150, 50,  300, 4, 1 },
};

static const td_kind_t *TouchDodge_Kind(int kind)
{
	int i;
	for (i = 0; i < (int)(sizeof(td_kinds) / sizeof(td_kinds[0])); i++)
		if (td_kinds[i].kind == kind)
			return &td_kinds[i];
	return NULL;
}

// The player, and how they move.
typedef struct td_player_s
{
	float org[3];
	float vel[3];
	float mins[3], maxs[3];
	int onground;
} td_player_t;

typedef struct td_move_s
{
	float maxspeed;     // ground speed the input asks for
	float accel;        // ground acceleration factor (Quake: accel * maxspeed per second)
	float airaccel;
	float friction;
	float stopspeed;
	float jumpvel;
	float gravity;
	int dodging;        // the server runs the dodging mutator
	float dodge_speed;  // its horizontal burst (sv_dodging_horiz_force 400)
	float dodge_up;     // and its lift (sv_dodging_up_speed 200)
} td_move_t;

// An escape: a horizontal direction held (zero: no input), with a jump or a
// dodge at the start.
typedef struct td_plan_s
{
	float dir[2];
	int jump;
	int dodge;
	const char *name;
} td_plan_t;

#define TD_STEP      0.025f
#define TD_MAX_STEPS 64           // 1.6 s
#define TD_DODGE_RAMP 0.1f

// The player's origin along a plan, every TD_STEP, from now.
static int td_player_path(const td_player_t *p, const td_move_t *m, const td_plan_t *plan, int steps, float path[][3])
{
	float o[3], v[3], ramp_left = 0;
	int i, ground = p->onground;
	memcpy(o, p->org, sizeof(o));
	memcpy(v, p->vel, sizeof(v));
	if (plan->jump && ground)
	{
		v[2] = m->jumpvel;
		ground = 0;
	}
	if (plan->dodge && m->dodging && ground)
	{
		v[2] = m->dodge_up;
		ground = 0;
		ramp_left = TD_DODGE_RAMP;
	}
	for (i = 0; i < steps; i++)
	{
		float dt = TD_STEP, speed, cur, add, acc;
		if (ground)
		{
			// Quake friction, then acceleration toward the wished direction
			speed = sqrtf(v[0] * v[0] + v[1] * v[1]);
			if (speed > 0.01f)
			{
				float drop = (speed > m->stopspeed ? speed : m->stopspeed) * m->friction * dt;
				float k = speed - drop > 0 ? (speed - drop) / speed : 0;
				v[0] *= k;
				v[1] *= k;
			}
		}
		if (plan->dir[0] != 0 || plan->dir[1] != 0)
		{
			cur = v[0] * plan->dir[0] + v[1] * plan->dir[1];
			add = m->maxspeed - cur;
			if (add > 0)
			{
				acc = (ground ? m->accel : m->airaccel) * m->maxspeed * dt;
				if (acc > add)
					acc = add;
				v[0] += plan->dir[0] * acc;
				v[1] += plan->dir[1] * acc;
			}
		}
		if (ramp_left > 0)
		{
			float share = (dt < ramp_left ? dt : ramp_left) / TD_DODGE_RAMP;
			v[0] += plan->dir[0] * m->dodge_speed * share;
			v[1] += plan->dir[1] * m->dodge_speed * share;
			ramp_left -= dt;
		}
		o[0] += v[0] * dt;
		o[1] += v[1] * dt;
		if (!ground)
		{
			o[2] += v[2] * dt - 0.5f * m->gravity * dt * dt;
			v[2] -= m->gravity * dt;
			if (o[2] <= p->org[2] && v[2] < 0)
			{
				// back on (level) ground
				o[2] = p->org[2];
				v[2] = 0;
				ground = 1;
			}
		}
		memcpy(path[i], o, sizeof(o));
	}
	return steps;
}

// Distance from point q to the box at origin o.
static float td_box_dist(const float q[3], const float o[3], const float mins[3], const float maxs[3])
{
	float d2 = 0;
	int k;
	for (k = 0; k < 3; k++)
	{
		float lo = o[k] + mins[k], hi = o[k] + maxs[k], d = 0;
		if (q[k] < lo)
			d = lo - q[k];
		else if (q[k] > hi)
			d = q[k] - hi;
		d2 += d * d;
	}
	return sqrtf(d2);
}

static float td_splash(const td_proj_t *pr, float dist)
{
	if (dist >= pr->radius)
		return 0;
	return pr->edge + (pr->damage - pr->edge) * (1 - dist / pr->radius);
}

// Where the projectile is every TD_STEP for `steps`, and the step at which it
// meets the world (`*impact_step`, -1 none) with the point.
typedef struct td_flight_s
{
	float pos[TD_MAX_STEPS][3];
	int steps;
	int impact_step;
	float impact[3];
	int impact_sky;     // it met sky: gone without a blast
} td_flight_t;

static void TouchDodge_Fly(tr_world_t *w, const td_proj_t *pr, float seconds, td_flight_t *f)
{
	float o[3], v[3];
	int i, n = (int)(seconds / TD_STEP);
	if (n > TD_MAX_STEPS)
		n = TD_MAX_STEPS;
	memcpy(o, pr->org, sizeof(o));
	memcpy(v, pr->vel, sizeof(v));
	f->impact_step = -1;
	f->impact_sky = 0;
	f->steps = n;
	for (i = 0; i < n; i++)
	{
		float next[3];
		tr_trace_t tr;
		next[0] = o[0] + v[0] * TD_STEP;
		next[1] = o[1] + v[1] * TD_STEP;
		next[2] = o[2] + v[2] * TD_STEP - 0.5f * pr->gravity * TD_STEP * TD_STEP;
		v[2] -= pr->gravity * TD_STEP;
		tr_trace(w, o, next, 0, &tr);
		if (!tr.startsolid && tr.fraction < 1)
		{
			memcpy(f->impact, tr.endpos, sizeof(f->impact));
			memcpy(f->pos[i], tr.endpos, sizeof(f->pos[i]));
			f->impact_step = i;
			f->impact_sky = tr.hit == TR_HIT_DEATH;
			f->steps = i + 1;
			return;
		}
		memcpy(f->pos[i], next, sizeof(f->pos[i]));
		memcpy(o, next, sizeof(o));
	}
}

// Damage the projectile does to a player following `path`, and when.
static float TouchDodge_Damage(const td_proj_t *pr, const td_flight_t *f, const td_player_t *p, float path[][3], float *when)
{
	int i;
	*when = -1;
	for (i = 0; i < f->steps; i++)
	{
		// through the box (with the projectile's own size) on the way: a hit
		if (td_box_dist(f->pos[i], path[i], p->mins, p->maxs) <= pr->size)
		{
			*when = (i + 1) * TD_STEP;
			return pr->damage;
		}
	}
	if (f->impact_step >= 0 && pr->world_blast && !f->impact_sky)
	{
		*when = (f->impact_step + 1) * TD_STEP;
		return td_splash(pr, td_box_dist(f->impact, path[f->impact_step], p->mins, p->maxs));
	}
	return 0;
}

// Whether an escape is safe to take: no wall in the way of its first part,
// and ground under every point of it (never off a ledge into the void, a
// death zone or a long drop).
#define TD_GROUND_DEPTH 160.0f
static int TouchDodge_PathSafe(tr_world_t *w, const td_player_t *p, float path[][3], int steps)
{
	int i, check[3];
	tr_trace_t tr;
	tr_fall_t fall;
	check[0] = steps / 3;
	check[1] = (2 * steps) / 3;
	check[2] = steps - 1;
	tr_trace(w, p->org, path[check[2]], 1, &tr);
	if (tr.fraction < 0.5f)
		return 0; // a wall right beside the player that way
	for (i = 0; i < 3; i++)
	{
		float q[3];
		q[0] = path[check[i]][0];
		q[1] = path[check[i]][1];
		q[2] = p->org[2] + 8; // judged from standing height: a jump is not a ledge
		if (TouchRescue_Below(w, q, TD_GROUND_DEPTH, &fall) != TR_LAND)
			return 0;
	}
	return 1;
}

// The candidate escapes for a shot coming along `shot_dir` (horizontal, from
// the projectile toward the player): both sides square to it, the same angled
// back away from it, straight back, each plain, with a jump, and with a dodge
// (when the server has dodging); and carrying on as now (index 0).
#define TD_MAX_PLANS 16
static int td_make_plans(const float shot_dir[2], const td_move_t *m, td_plan_t *plans)
{
	static const char *const names[] = { "left", "right", "left and back", "right and back", "back" };
	float sx = shot_dir[0], sy = shot_dir[1];
	float dirs[5][2];
	int n = 0, i, variant;
	dirs[0][0] = -sy; dirs[0][1] = sx;   // left of the shot's path
	dirs[1][0] = sy;  dirs[1][1] = -sx;  // right
	dirs[2][0] = (-sy + sx) * 0.7071f; dirs[2][1] = (sx + sy) * 0.7071f;
	dirs[3][0] = (sy + sx) * 0.7071f;  dirs[3][1] = (-sx + sy) * 0.7071f;
	dirs[4][0] = sx; dirs[4][1] = sy;   // straight on, away from the shooter
	plans[n].dir[0] = plans[n].dir[1] = 0;
	plans[n].jump = plans[n].dodge = 0;
	plans[n].name = "carry on";
	n++;
	for (variant = 0; variant < 3; variant++)
	{
		if (variant == 2 && !m->dodging)
			break;
		for (i = 0; i < 5 && n < TD_MAX_PLANS; i++)
		{
			plans[n].dir[0] = dirs[i][0];
			plans[n].dir[1] = dirs[i][1];
			plans[n].jump = variant == 1;
			plans[n].dodge = variant == 2;
			plans[n].name = names[i];
			n++;
		}
	}
	return n;
}

typedef struct td_choice_s
{
	int plan;           // index into plans, 0 = carry on
	td_plan_t chosen;
	float base_damage;  // carrying on
	float damage;       // with the chosen escape
	float when;         // seconds to the hit (carrying on)
	int rejected;       // escapes ruled out by walls or ledges
} td_choice_t;

// Judge the projectiles coming (a Crylink's pellets, a Hagar burst, two
// players' rockets) against the player together: the damage of all of them
// if the player carries on (keeping their own input `own_dir`, zero for
// none), and the escape that does best against all of them, across their
// combined direction. Returns 1 when an escape cuts the total by at least
// min_gain.
#define TD_MAX_PROJ 8
static int TouchDodge_ChooseAll(tr_world_t *w, const td_proj_t *pr, int n, const td_player_t *p, const td_move_t *m, const float own_dir[2],
	float window, float min_gain, td_choice_t *out)
{
	static float path[TD_MAX_STEPS][3];
	static td_flight_t f[TD_MAX_PROJ];
	td_plan_t plans[TD_MAX_PLANS];
	float shot[2] = { 0, 0 }, len, best;
	int nplans, i, j, steps = 0;
	memset(out, 0, sizeof(*out));
	if (n > TD_MAX_PROJ)
		n = TD_MAX_PROJ;
	for (j = 0; j < n; j++)
	{
		float l = sqrtf(pr[j].vel[0] * pr[j].vel[0] + pr[j].vel[1] * pr[j].vel[1]);
		TouchDodge_Fly(w, &pr[j], window, &f[j]);
		if (f[j].steps > steps)
			steps = f[j].steps;
		if (l > 1)
		{
			shot[0] += pr[j].vel[0] / l;
			shot[1] += pr[j].vel[1] / l;
		}
	}
	len = sqrtf(shot[0] * shot[0] + shot[1] * shot[1]);
	if (len < 0.01f)
	{
		// falling straight down onto the player: any side will do
		shot[0] = 1;
		shot[1] = 0;
	}
	else
	{
		shot[0] /= len;
		shot[1] /= len;
	}
	nplans = td_make_plans(shot, m, plans);
	plans[0].dir[0] = own_dir[0];
	plans[0].dir[1] = own_dir[1];
	out->when = -1;
	for (i = 0; i < nplans; i++)
	{
		float d = 0, when;
		td_player_path(p, m, &plans[i], steps, path);
		for (j = 0; j < n; j++)
		{
			d += TouchDodge_Damage(&pr[j], &f[j], p, path, &when);
			if (i == 0 && when >= 0 && (out->when < 0 || when < out->when))
				out->when = when;
		}
		if (i == 0)
		{
			out->base_damage = out->damage = best = d;
			out->chosen = plans[0];
			if (d <= 0)
				return 0;
			continue;
		}
		if (d >= best - 0.5f)
			continue;
		if (!TouchDodge_PathSafe(w, p, path, steps))
		{
			out->rejected++;
			continue;
		}
		best = d;
		out->plan = i;
		out->chosen = plans[i];
		out->damage = d;
	}
	return out->plan > 0 && out->base_damage - out->damage >= min_gain;
}

// One projectile against the player (TouchDodge_ChooseAll with one).
static int TouchDodge_Choose(tr_world_t *w, const td_proj_t *pr, const td_player_t *p, const td_move_t *m, const float own_dir[2],
	float window, float min_gain, td_choice_t *out)
{
	return TouchDodge_ChooseAll(w, pr, 1, p, m, own_dir, window, min_gain, out);
}

#endif
