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

// The engine side's read-only query (touch_dodge.c), for other touch modules:
// include this header with TOUCH_DODGE_API_ONLY defined to get only this.
//
// TouchDodge_Unescapable: 1 while an enemy shot judged this frame would do at
// least the dodge's minimum damage and no strafe, jump or dash escape clears
// it (the best one still takes at least half of it); then *damage is what the
// best escape still takes and *seconds the time to impact, in server time.
// 0 otherwise, with both set to 0.
int TouchDodge_Unescapable(float *damage, float *seconds);
// TouchDodge_Lateral: 1 while a dodge is under way, with the horizontal
// velocity (world x y, u/s) it is steering the player toward this frame, so
// the aim can lead for it; 0 and zeros otherwise.
int TouchDodge_Lateral(float vel[2]);

#ifndef TOUCH_DODGE_API_ONLY

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
	float bounce;       // bounces off the world, keeping this share of its speed into the surface (0: no)
	float fuse;         // explodes this long after its first bounce (< 0: not from a bounce)
	float homing;       // steers toward the player: share of the way to the player's direction per step (0: no)
	float accel;        // speeds up by this, u/s^2, up to max_speed (a Devastator rocket, a seeker)
	float max_speed;
	float prox;         // lies where it lands and explodes when the player comes this close (0: no)
	float margin;       // extra uncertainty, units, on top of TD_SPLASH_MARGIN and the size (a late view of it)
	float life;         // blows this long from now wherever it is (< 0: not on a timer); a nade
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
#define TD_KIND_MINE             9
#define TD_KIND_SEEKER_MISSILE   12
// Offhand grenades (the nades mutator): the ones that hurt or hold an enemy.
// Translocate, spawn, veil, monster and ammo ones do neither and are left alone.
#define TD_KIND_NADE             71
#define TD_KIND_NADE_BURN        72
#define TD_KIND_NADE_NAPALM      73
#define TD_KIND_NADE_NAPALM_BURN 74
#define TD_KIND_NADE_ICE         76
#define TD_KIND_NADE_ICE_BURN    77
#define TD_KIND_NADE_HEAL        80
#define TD_KIND_NADE_HEAL_BURN   81
#define TD_KIND_NADE_ENTRAP      84
#define TD_KIND_NADE_ENTRAP_BURN 85
#define TD_KIND_NADE_DARK        90
#define TD_KIND_NADE_DARK_BURN   91
// A thrown nade has cooked at least a second of its 3.5 s fuse (stock), and
// mostly a little more: it is judged to blow this long after it is first
// seen (sooner is no harm: the player just steps out earlier).
#define TD_NADE_FUSE_SEEN        2.0f

typedef struct td_kind_s
{
	int kind; const char *name; float damage, edge, radius, size; int world_blast;
	float bounce, fuse, homing, accel, max_speed, prox;
	int nade;           // on a fuse from its throw (td_proj_t.life), and dangerous lying too
} td_kind_t;

// kind, name, damage, edge, radius, size, world_blast, bounce, fuse, homing, accel, max_speed, prox, nade
static const td_kind_t td_kinds[] = {
	// leaves at 1000 u/s, reaching 1300 in a quarter of a second
	{ TD_KIND_ROCKET,           "rocket",         80,  40,    110, 4,  1, 0,    -1,   0,     1300, 1300, 0, 0 },
	{ TD_KIND_GRENADE,          "mortar grenade", 55,  25,    120, 3,  1, 0,    -1,   0,     0,    0,    0, 0 },
	// bounces, blows 0.5 s after the first bounce or on a player
	{ TD_KIND_GRENADE_BOUNCING, "mortar grenade", 55,  30,    120, 3,  0, 0.5f, 0.5f, 0,     0,    0,    0, 0 },
	{ TD_KIND_ELECTRO_BEAM,     "electro bolt",   40,  20,    100, 3,  1, 0,    -1,   0,     0,    0,    0, 0 },
	// bounces; blows on a player
	{ TD_KIND_ELECTRO,          "electro orb",    30,  15,    150, 3,  0, 0.3f, -1,   0,     0,    0,    0, 0 },
	{ TD_KIND_HAGAR,            "hagar rocket",   25,  12.5f, 65,  2,  1, 0,    -1,   0,     0,    0,    0, 0 },
	{ TD_KIND_HAGAR_BOUNCING,   "hagar rocket",   35,  17.5f, 80,  2,  1, 0,    -1,   0,     0,    0,    0, 0 },
	{ TD_KIND_CRYLINK,          "crylink",        10,  5,     80,  2,  1, 0,    -1,   0,     0,    0,    0, 0 },
	{ TD_KIND_CRYLINK_BOUNCING, "crylink",        10,  5,     80,  2,  1, 0,    -1,   0,     0,    0,    0, 0 },
	{ TD_KIND_ARC_BOLT,         "arc bolt",       25,  12.5f, 65,  2,  1, 0,    -1,   0,     0,    0,    0, 0 },
	{ TD_KIND_FIREBALL,         "fireball",       200, 50,    200, 16, 1, 0,    -1,   0,     0,    0,    0, 0 },
	{ TD_KIND_RPC,              "rpc rocket",     150, 50,    300, 4,  1, 0,    -1,   0,     0,    0,    0, 0 },
	// homes: the stock turn rate, per server frame (taken as a TD_STEP)
	{ TD_KIND_SEEKER_MISSILE,   "seeker missile", 30,  10,    80,  4,  1, 0,    -1,   0.65f, 1400, 1300, 0, 0 },
	// lands, then blows 0.1 - 0.3 s after an enemy comes within 125
	{ TD_KIND_MINE,             "mine",           40,  20,    175, 4,  0, 0,    -1,   0,     0,    0,    125, 0 },
	// nades: bounce, and blow on their fuse; the damage of each kind at the
	// stock radius, or what holding still in it is worth (ice freezes, entrap
	// slows, darkness blinds, heal hurts a foe)
	{ TD_KIND_NADE,             "nade",           225, 90,    300, 8,  0, 0.5f, -1,   0,     0,    0,    0,   1 },
	{ TD_KIND_NADE_BURN,        "nade",           225, 90,    300, 8,  0, 0.5f, -1,   0,     0,    0,    0,   1 },
	{ TD_KIND_NADE_NAPALM,      "napalm nade",    60,  30,    300, 8,  0, 0.5f, -1,   0,     0,    0,    0,   1 },
	{ TD_KIND_NADE_NAPALM_BURN, "napalm nade",    60,  30,    300, 8,  0, 0.5f, -1,   0,     0,    0,    0,   1 },
	{ TD_KIND_NADE_ICE,         "ice nade",       100, 100,   300, 8,  0, 0.5f, -1,   0,     0,    0,    0,   1 },
	{ TD_KIND_NADE_ICE_BURN,    "ice nade",       100, 100,   300, 8,  0, 0.5f, -1,   0,     0,    0,    0,   1 },
	{ TD_KIND_NADE_HEAL,        "heal nade",      40,  40,    300, 8,  0, 0.5f, -1,   0,     0,    0,    0,   1 },
	{ TD_KIND_NADE_HEAL_BURN,   "heal nade",      40,  40,    300, 8,  0, 0.5f, -1,   0,     0,    0,    0,   1 },
	{ TD_KIND_NADE_ENTRAP,      "entrap nade",    40,  40,    300, 8,  0, 0.5f, -1,   0,     0,    0,    0,   1 },
	{ TD_KIND_NADE_ENTRAP_BURN, "entrap nade",    40,  40,    300, 8,  0, 0.5f, -1,   0,     0,    0,    0,   1 },
	{ TD_KIND_NADE_DARK,        "darkness nade",  30,  30,    300, 8,  0, 0.5f, -1,   0,     0,    0,    0,   1 },
	{ TD_KIND_NADE_DARK_BURN,   "darkness nade",  30,  30,    300, 8,  0, 0.5f, -1,   0,     0,    0,    0,   1 },
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
	// In the air (Xonotic's air physics): input only sideways to the view
	// accelerates at airstrafeaccel up to maxairstrafespeed, any other at
	// airaccel up to maxairspeed. 0: as on the ground's maxspeed / airaccel.
	float fwd[2];       // the view's horizontal forward
	float airstrafeaccel, maxairstrafespeed, maxairspeed;
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
	int i, ground = p->onground, lands = p->onground; // a jump from the ground comes back to it; a fall's floor is not known
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
			float wish = m->maxspeed, rate = ground ? m->accel : m->airaccel;
			if (!ground)
			{
				float along = plan->dir[0] * m->fwd[0] + plan->dir[1] * m->fwd[1];
				if (m->maxairspeed > 0)
					wish = m->maxairspeed;
				if (along * along < 0.04f && m->airstrafeaccel > 0 && m->maxairstrafespeed > 0)
				{
					// strafe keys alone
					wish = m->maxairstrafespeed;
					rate = m->airstrafeaccel;
				}
			}
			cur = v[0] * plan->dir[0] + v[1] * plan->dir[1];
			add = wish - cur;
			if (add > 0)
			{
				acc = rate * wish * dt;
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
			if (lands && o[2] <= p->org[2] && v[2] < 0)
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

// Splash at `dist` from the blast: Xonotic's linear fall-off from core to
// edge damage at the radius. Judged with a margin: where the blast lands and
// where the player is are both predictions, a few tens of units out, and a
// player just outside the radius by the prediction is often just inside it.
#define TD_SPLASH_MARGIN 32.0f
static float td_splash(const td_proj_t *pr, float dist)
{
	if (dist >= pr->radius + TD_SPLASH_MARGIN + pr->margin)
		return 0;
	if (dist >= pr->radius)
		return pr->edge;
	return pr->edge + (pr->damage - pr->edge) * (1 - dist / pr->radius);
}

// Where the projectile is every TD_STEP for `steps`, and the step at which it
// ends (`*impact_step`, -1 none) with the point: where it meets the world, or
// for a bouncing one with a fuse, where the fuse runs out; for a mine, where
// it lands.
typedef struct td_flight_s
{
	float pos[TD_MAX_STEPS][3];
	int steps;
	int impact_step;
	float impact[3];
	int impact_sky;     // it met sky: gone without a blast
	int blast;          // it ends in a blast at the impact point
	int landed;         // a mine lying at the impact point
} td_flight_t;

// One step of a homing projectile's flight toward `target` (Xonotic's seeker:
// the new direction is the old one plus `homing` of the way to the target,
// speed growing by accel up to max_speed).
static void td_home(const td_proj_t *pr, const float o[3], float v[3], const float target[3])
{
	float to[3], d, sp, nv[3], l;
	int k;
	to[0] = target[0] - o[0]; to[1] = target[1] - o[1]; to[2] = target[2] - o[2];
	d = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
	sp = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (d < 1 || sp < 1)
		return;
	for (k = 0; k < 3; k++)
		nv[k] = v[k] / sp + pr->homing * to[k] / d;
	l = sqrtf(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
	sp += pr->accel * TD_STEP;
	if (pr->max_speed > 0 && sp > pr->max_speed)
		sp = pr->max_speed;
	if (l > 0.001f)
		for (k = 0; k < 3; k++)
			v[k] = nv[k] / l * sp;
}

// The flight. A homing projectile follows `target` (the player's centre each
// step along a path, NULL: straight).
static void TouchDodge_FlyAt(tr_world_t *w, const td_proj_t *pr, float seconds, const float (*target)[3], td_flight_t *f)
{
	float o[3], v[3];
	int i, n = (int)(seconds / TD_STEP), bounced = -1;
	if (n > TD_MAX_STEPS)
		n = TD_MAX_STEPS;
	memcpy(o, pr->org, sizeof(o));
	memcpy(v, pr->vel, sizeof(v));
	f->impact_step = -1;
	f->impact_sky = 0;
	f->blast = 0;
	f->landed = 0;
	f->steps = n;
	for (i = 0; i < n; i++)
	{
		float next[3];
		tr_trace_t tr;
		if (pr->homing > 0 && target)
			td_home(pr, o, v, target[i]);
		else if (pr->accel > 0 && pr->max_speed > 0)
		{
			// speeding up along its way
			float sp = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
			if (sp > 1 && sp < pr->max_speed)
			{
				float k = (sp + pr->accel * TD_STEP < pr->max_speed ? sp + pr->accel * TD_STEP : pr->max_speed) / sp;
				v[0] *= k; v[1] *= k; v[2] *= k;
			}
		}
		next[0] = o[0] + v[0] * TD_STEP;
		next[1] = o[1] + v[1] * TD_STEP;
		next[2] = o[2] + v[2] * TD_STEP - 0.5f * pr->gravity * TD_STEP * TD_STEP;
		v[2] -= pr->gravity * TD_STEP;
		tr_trace(w, o, next, 0, &tr);
		if (!tr.startsolid && tr.fraction < 1)
		{
			int sky = tr.hit == TR_HIT_DEATH;
			if (pr->bounce > 0 && !sky)
			{
				// off the surface (Quake's ClipVelocity, overbounce 1 + bounce)
				float into = v[0] * tr.normal[0] + v[1] * tr.normal[1] + v[2] * tr.normal[2];
				int k;
				for (k = 0; k < 3; k++)
				{
					v[k] -= tr.normal[k] * into * (1 + pr->bounce);
					next[k] = tr.endpos[k] + tr.normal[k] * 0.5f;
				}
				if (bounced < 0)
					bounced = i;
				if (tr.normal[2] > 0.7f && v[2] < 60)
				{
					// at rest on the floor
					v[0] = v[1] = v[2] = 0;
				}
			}
			else
			{
				memcpy(f->impact, tr.endpos, sizeof(f->impact));
				memcpy(f->pos[i], tr.endpos, sizeof(f->pos[i]));
				f->impact_step = i;
				f->impact_sky = sky;
				f->blast = pr->world_blast && !sky;
				f->landed = pr->prox > 0 && !sky;
				f->steps = i + 1;
				if (f->landed)
				{
					// it lies there
					int j;
					for (j = i + 1; j < n; j++)
						memcpy(f->pos[j], tr.endpos, sizeof(f->pos[j]));
					f->steps = n;
				}
				return;
			}
		}
		memcpy(f->pos[i], next, sizeof(f->pos[i]));
		memcpy(o, next, sizeof(o));
		if (pr->life >= 0 && (i + 1) * TD_STEP >= pr->life)
		{
			// its fuse runs out here
			memcpy(f->impact, next, sizeof(f->impact));
			f->impact_step = i;
			f->blast = 1;
			f->steps = i + 1;
			return;
		}
		if (bounced >= 0 && pr->fuse >= 0 && (i - bounced + 1) * TD_STEP >= pr->fuse)
		{
			memcpy(f->impact, next, sizeof(f->impact));
			f->impact_step = i;
			f->blast = 1;
			f->steps = i + 1;
			return;
		}
	}
}

static void TouchDodge_Fly(tr_world_t *w, const td_proj_t *pr, float seconds, td_flight_t *f)
{
	TouchDodge_FlyAt(w, pr, seconds, NULL, f);
}

// Damage the projectile does to a player following `path`, and when.
static float TouchDodge_Damage(const td_proj_t *pr, const td_flight_t *f, const td_player_t *p, float path[][3], float *when)
{
	int i;
	*when = -1;
	// through the box (with the projectile's own size) on the way: a hit;
	// a nade bounces off a player and only blows on its fuse
	for (i = 0; i < f->steps && pr->life < 0; i++)
	{
		if (td_box_dist(f->pos[i], path[i], p->mins, p->maxs) <= pr->size + pr->margin * 0.4f)
		{
			*when = (i + 1) * TD_STEP;
			return pr->damage;
		}
	}
	if (f->landed)
	{
		// a mine: it blows once the player comes within reach, a moment later
		for (i = f->impact_step; i < f->steps; i++)
			if (td_box_dist(f->impact, path[i], p->mins, p->maxs) <= pr->prox)
			{
				int at = i + (int)(0.2f / TD_STEP);
				if (at >= f->steps)
					at = f->steps - 1;
				*when = (at + 1) * TD_STEP;
				return td_splash(pr, td_box_dist(f->impact, path[at], p->mins, p->maxs));
			}
		return 0;
	}
	if (f->impact_step >= 0 && f->blast)
	{
		*when = (f->impact_step + 1) * TD_STEP;
		return td_splash(pr, td_box_dist(f->impact, path[f->impact_step], p->mins, p->maxs));
	}
	return 0;
}

// Enemy mines lying about: an escape never takes the player within `radius`
// of one.
#define TD_MAX_HAZARDS 16
typedef struct td_hazards_s
{
	int n;
	float org[TD_MAX_HAZARDS][3];
	float radius;
} td_hazards_t;

// Whether an escape is safe to take: no wall in the way of its first part,
// ground under every point of it (never off a ledge into the void, a death
// zone or a long drop), and no lying mine within reach of it.
#define TD_GROUND_DEPTH 160.0f
static int TouchDodge_PathSafe(tr_world_t *w, const td_player_t *p, float path[][3], int steps, const td_hazards_t *hz)
{
	int i, check[3];
	tr_trace_t tr;
	tr_fall_t fall;
	if (hz)
	{
		// within reach of one, and closer than the player is now
		int j;
		for (j = 0; j < hz->n; j++)
		{
			float d0 = td_box_dist(hz->org[j], p->org, p->mins, p->maxs);
			for (i = 0; i < steps; i += 2)
			{
				float d = td_box_dist(hz->org[j], path[i], p->mins, p->maxs);
				if (d < hz->radius && d < d0 - 1)
					return 0;
			}
		}
	}
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
// back away from it and angled toward the shooter (out of a blast against a
// wall behind the player), straight back, and when the player is running along the
// shot's path, each side keeping that run; each plain, with a jump, and with
// a dodge (when the server has dodging); and carrying on as now (index 0).
#define TD_MAX_PLANS 32
static int td_make_plans(const float shot_dir[2], const td_move_t *m, const float own_dir[2], td_plan_t *plans)
{
	static const char *const names[] = { "left", "right", "left and back", "right and back", "back", "left and forward", "right and forward",
		"left, keeping the run", "right, keeping the run" };
	float sx = shot_dir[0], sy = shot_dir[1];
	float dirs[9][2], run = own_dir[0] * sx + own_dir[1] * sy;
	int n = 0, i, variant, ndirs = 7;
	dirs[0][0] = -sy; dirs[0][1] = sx;   // left of the shot's path
	dirs[1][0] = sy;  dirs[1][1] = -sx;  // right
	dirs[2][0] = (-sy + sx) * 0.7071f; dirs[2][1] = (sx + sy) * 0.7071f;
	dirs[3][0] = (sy + sx) * 0.7071f;  dirs[3][1] = (-sx + sy) * 0.7071f;
	dirs[4][0] = sx; dirs[4][1] = sy;   // straight on, away from the shooter
	dirs[5][0] = (-sy - sx) * 0.7071f; dirs[5][1] = (sx - sy) * 0.7071f;   // left, toward the shooter
	dirs[6][0] = (sy - sx) * 0.7071f;  dirs[6][1] = (-sx - sy) * 0.7071f;  // right, toward the shooter
	if (run * run > 0.25f)
	{
		// the player's run along the shot's path, with the side added
		float r = run > 0 ? 0.7071f : -0.7071f;
		dirs[7][0] = dirs[0][0] * 0.7071f + sx * r; dirs[7][1] = dirs[0][1] * 0.7071f + sy * r;
		dirs[8][0] = dirs[1][0] * 0.7071f + sx * r; dirs[8][1] = dirs[1][1] * 0.7071f + sy * r;
		ndirs = 9;
	}
	plans[n].dir[0] = plans[n].dir[1] = 0;
	plans[n].jump = plans[n].dodge = 0;
	plans[n].name = "carry on";
	n++;
	for (variant = 0; variant < 3; variant++)
	{
		if (variant == 2 && !m->dodging)
			break;
		for (i = 0; i < ndirs && n < TD_MAX_PLANS; i++)
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
// combined direction, never near a lying mine in `hz`. Of escapes within a
// point of the best, the one nearest the player's own direction. Returns 1
// when an escape cuts the total by at least min_gain and at least halves it.
#define TD_MAX_PROJ 8
static int TouchDodge_ChooseAllHz(tr_world_t *w, const td_proj_t *pr, int n, const td_player_t *p, const td_move_t *m, const float own_dir[2],
	float window, float min_gain, const td_hazards_t *hz, td_choice_t *out)
{
	static float path[TD_MAX_STEPS][3], centre[TD_MAX_STEPS][3];
	static td_flight_t f[TD_MAX_PROJ], hf;
	td_plan_t plans[TD_MAX_PLANS];
	float shot[2] = { 0, 0 }, len, best = 0, best_pref = -2, mid[3];
	int nplans, i, j, k, steps = 0, homing = 0;
	memset(out, 0, sizeof(*out));
	if (n > TD_MAX_PROJ)
		n = TD_MAX_PROJ;
	for (j = 0; j < n; j++)
	{
		float l = sqrtf(pr[j].vel[0] * pr[j].vel[0] + pr[j].vel[1] * pr[j].vel[1]);
		TouchDodge_Fly(w, &pr[j], window, &f[j]);
		if (pr[j].homing > 0)
		{
			homing = 1;
			f[j].steps = (int)(window / TD_STEP) > TD_MAX_STEPS ? TD_MAX_STEPS : (int)(window / TD_STEP);
		}
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
	nplans = td_make_plans(shot, m, own_dir, plans);
	plans[0].dir[0] = own_dir[0];
	plans[0].dir[1] = own_dir[1];
	for (k = 0; k < 3; k++)
		mid[k] = (p->mins[k] + p->maxs[k]) * 0.5f;
	out->when = -1;
	for (i = 0; i < nplans; i++)
	{
		float d = 0, when, pref;
		td_player_path(p, m, &plans[i], steps, path);
		for (j = 0; j < n; j++)
		{
			const td_flight_t *fl = &f[j];
			if (pr[j].homing > 0)
			{
				// it steers at the player along this plan: flown again
				int q;
				for (q = 0; q < steps; q++)
					for (k = 0; k < 3; k++)
						centre[q][k] = path[q][k] + mid[k];
				TouchDodge_FlyAt(w, &pr[j], steps * TD_STEP, (const float (*)[3])centre, &hf);
				fl = &hf;
			}
			d += TouchDodge_Damage(&pr[j], fl, p, path, &when);
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
		if (d > best + 1)
			continue;
		pref = own_dir[0] * plans[i].dir[0] + own_dir[1] * plans[i].dir[1];
		if (out->plan > 0 && d >= best - 1 && pref <= best_pref)
			continue; // no better, and no nearer the player's own way
		if (out->plan == 0 && d >= best - 0.5f)
			continue;
		if (!TouchDodge_PathSafe(w, p, path, steps, hz))
		{
			out->rejected++;
			continue;
		}
		best = d;
		best_pref = pref;
		out->plan = i;
		out->chosen = plans[i];
		out->damage = d;
	}
	(void)homing;
	return out->plan > 0 && out->base_damage - out->damage >= min_gain && out->damage <= out->base_damage * 0.5f;
}

static int TouchDodge_ChooseAll(tr_world_t *w, const td_proj_t *pr, int n, const td_player_t *p, const td_move_t *m, const float own_dir[2],
	float window, float min_gain, td_choice_t *out)
{
	return TouchDodge_ChooseAllHz(w, pr, n, p, m, own_dir, window, min_gain, NULL, out);
}

// One projectile against the player (TouchDodge_ChooseAll with one).
static int TouchDodge_Choose(tr_world_t *w, const td_proj_t *pr, const td_player_t *p, const td_move_t *m, const float own_dir[2],
	float window, float min_gain, td_choice_t *out)
{
	return TouchDodge_ChooseAll(w, pr, 1, p, m, own_dir, window, min_gain, out);
}

#endif // TOUCH_DODGE_API_ONLY

#endif
