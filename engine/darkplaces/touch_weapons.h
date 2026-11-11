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
What touch auto-shoot knows about each weapon, and the decisions it makes
from that: where to aim (lead for projectiles, a lofted pitch for grenades),
primary or secondary fire, which weapon suits the distance, and when to
detonate a Devastator rocket.

The numbers are the stock Xonotic balance (bal-wep-xonotic.cfg), looked up
by the weapon's netname, which every CSQC reports the same way, so this works
on public servers too. A server running another balance can disagree; then a
lead or a range here is off by that difference, never worse than no lead.

Only touch_aim.c includes this in the engine. It is plain C (the includer
supplies qbool, true and false) so that scripts/test-touch-aim.sh can test it
without the rest of the engine.
*/

#ifndef TOUCH_WEAPONS_H
#define TOUCH_WEAPONS_H

#include <math.h>
#include <string.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// What a fire mode does.
#define TW_KIND_NONE      0
#define TW_KIND_HITSCAN   1 // instant hit: no lead (the server's antilag wants what is on screen)
#define TW_KIND_BEAM      2 // continuous instant beam (Arc)
#define TW_KIND_PROJ      3 // straight projectile, constant speed (or accelerating: accel > 0)
#define TW_KIND_BALLISTIC 4 // lobbed under gravity, launched at speed plus speed_up along the view's up
#define TW_KIND_MELEE     5 // a swing that reaches `range`

// What the secondary button does.
#define TW_SEC_NONE     0
#define TW_SEC_SHOT     1 // another shot, described by .sec
#define TW_SEC_MELEE    2
#define TW_SEC_DETONATE 3 // sets off what is already out (Devastator rockets, mines)
#define TW_SEC_ZOOM     4
#define TW_SEC_SWITCH   5 // back to the previous weapon (Blaster)
#define TW_SEC_LOAD     6 // hold to load, release to fire (Hagar)

// Weapon flags.
#define TW_GUIDED        1  // primary is steered along the view while attack stays held
#define TW_ONE_PER_PRESS 2  // holding attack fires once; the next shot needs a release
#define TW_NO_AUTOFIRE   4  // never fired automatically (porto, hook, tuba)
#define TW_NO_SWITCH_TO  8  // never auto-switched to
#define TW_WEAK_SPLASH   16 // own splash is harmless enough to fire point-blank (Blaster)
#define TW_MINES         32 // primary places mines that the secondary detonates
#define TW_FALLBACK      64 // a last resort: auto-switch picks it only when nothing else owned can shoot
#define TW_PIERCE       128 // the primary goes on through players it hits (Vortex, Vaporizer)

// How a target moves.
#define TW_CLASS_STAND   0
#define TW_CLASS_STEADY  1
#define TW_CLASS_ERRATIC 2
#define TW_CLASS_AIR     3

typedef struct touch_wep_fire_s
{
	int kind;
	float speed;     // units/s (start speed for an accelerating rocket)
	float speed_up;  // extra launch speed along the view's up vector (ballistic)
	float accel;     // units/s^2 up to speed_max (Devastator)
	float speed_max;
	float splash;    // splash radius, 0 = none
	float spread;    // stock spread factor (about radians), 0 = exact
	float range;     // useful range in units
	float damage;    // full damage of one shot (all pellets), for ranking only
} touch_wep_fire_t;

typedef struct touch_wep_s
{
	const char *netname;
	int flags;
	touch_wep_fire_t pri;
	int sec_what;
	touch_wep_fire_t sec;
	int cost;        // ammo one primary shot takes
} touch_wep_t;

#define TW_FAR 30000.0f
#define TW_DEG 57.29577951f // degrees per radian
// clang-format off
static const touch_wep_t touch_weapons[] =
{
	// netname        flags                              primary: kind              speed  up    accel max   splash spread range   damage   secondary                                   cost
	{ "blaster",      TW_WEAK_SPLASH | TW_FALLBACK,      { TW_KIND_PROJ,      6000,    0,    0,    0,   60,  0,     TW_FAR,  20 }, TW_SEC_SWITCH,   { 0 }, 0 },
	{ "shotgun",      0,                                 { TW_KIND_HITSCAN,      0,    0,    0,    0,    0,  0.12f, 1200,    48 }, TW_SEC_MELEE,    { TW_KIND_MELEE, 0, 0, 0, 0, 0, 0, 120, 70 }, 1 },
	{ "machinegun",   0,                                 { TW_KIND_HITSCAN,      0,    0,    0,    0,    0,  0.03f, TW_FAR,   6 }, TW_SEC_SHOT,     { TW_KIND_HITSCAN, 0, 0, 0, 0, 0, 0, TW_FAR, 30 }, 1 },
	{ "arc",          0,                                 { TW_KIND_BEAM,         0,    0,    0,    0,    0,  0,     1500,    25 }, TW_SEC_SHOT,     { TW_KIND_PROJ, 2300, 0, 0, 0, 65, 0, TW_FAR, 25 }, 1 },
	{ "mortar",       0,                                 { TW_KIND_BALLISTIC, 1900,  225,    0,    0,  120,  0,     2500,    55 }, TW_SEC_SHOT,     { TW_KIND_BALLISTIC, 1400, 150, 0, 0, 120, 0, 1500, 55 }, 2 },
	{ "minelayer",    TW_MINES | TW_NO_SWITCH_TO,        { TW_KIND_BALLISTIC,  800,    0,    0,    0,  175,  0,     1000,    40 }, TW_SEC_DETONATE, { 0 }, 4 },
	{ "electro",      0,                                 { TW_KIND_PROJ,      2500,    0,    0,    0,  100,  0,     TW_FAR,  40 }, TW_SEC_SHOT,     { TW_KIND_BALLISTIC, 1000, 200, 0, 0, 150, 0, 1200, 30 }, 4 },
	{ "crylink",      0,                                 { TW_KIND_PROJ,      2000,    0,    0,    0,   80,  0.08f, 2000,    60 }, TW_SEC_SHOT,     { TW_KIND_PROJ, 3000, 0, 0, 0, 100, 0.01f, 2500, 40 }, 3 },
	{ "hlac",         0,                                 { TW_KIND_PROJ,      6000,    0,    0,    0,   60,  0.03f, TW_FAR,  10 }, TW_SEC_SHOT,     { TW_KIND_PROJ, 6000, 0, 0, 0, 60, 0.15f, 1200, 90 }, 1 },
	{ "vortex",       TW_PIERCE,                         { TW_KIND_HITSCAN,      0,    0,    0,    0,    0,  0,     TW_FAR,  80 }, TW_SEC_ZOOM,     { 0 }, 6 },
	{ "rifle",        0,                                 { TW_KIND_HITSCAN,      0,    0,    0,    0,    0,  0,     TW_FAR,  80 }, TW_SEC_SHOT,     { TW_KIND_HITSCAN, 0, 0, 0, 0, 0, 0.04f, 2000, 80 }, 10 },
	{ "vaporizer",    TW_PIERCE,                         { TW_KIND_HITSCAN,      0,    0,    0,    0,    0,  0,     TW_FAR, 150 }, TW_SEC_SHOT,     { TW_KIND_PROJ, 6000, 0, 0, 0, 60, 0, TW_FAR, 20 }, 10 },
	{ "hagar",        0,                                 { TW_KIND_PROJ,      2200,    0,    0,    0,   65,  0,     TW_FAR,  25 }, TW_SEC_LOAD,     { 0 }, 1 },
	{ "seeker",       TW_NO_SWITCH_TO,                   { TW_KIND_PROJ,      5000,    0,    0,    0,    0,  0,     TW_FAR,   0 }, TW_SEC_SHOT,     { TW_KIND_BALLISTIC, 3000, 1000, 0, 0, 100, 0.4f, 300, 15 }, 1 },
	{ "devastator",   TW_GUIDED | TW_ONE_PER_PRESS,      { TW_KIND_PROJ,      1000,    0, 1300, 1300,  110,  0,     TW_FAR,  80 }, TW_SEC_DETONATE, { 0 }, 4 },
	{ "fireball",     TW_NO_SWITCH_TO,                   { TW_KIND_PROJ,      1200,    0,    0,    0,  200,  0,     TW_FAR, 200 }, TW_SEC_SHOT,     { TW_KIND_BALLISTIC, 900, 100, 0, 0, 0, 0, 600, 40 }, 0 },
	{ "tuba",         TW_NO_AUTOFIRE | TW_NO_SWITCH_TO,  { TW_KIND_NONE,         0,    0,    0,    0,    0,  0,     0,        0 }, TW_SEC_NONE,     { 0 }, 0 },
	{ "hook",         TW_NO_AUTOFIRE | TW_NO_SWITCH_TO,  { TW_KIND_NONE,         0,    0,    0,    0,    0,  0,     0,        0 }, TW_SEC_NONE,     { 0 }, 0 },
	{ "porto",        TW_NO_AUTOFIRE | TW_NO_SWITCH_TO,  { TW_KIND_NONE,         0,    0,    0,    0,    0,  0,     0,        0 }, TW_SEC_NONE,     { 0 }, 0 },
	// Overkill: every secondary is a blaster shot.
	{ "okhmg",        0,                                 { TW_KIND_HITSCAN,      0,    0,    0,    0,    0,  0.03f, TW_FAR,  30 }, TW_SEC_SHOT,     { TW_KIND_PROJ, 6000, 0, 0, 0, 60, 0, TW_FAR, 20 }, 1 },
	{ "okmachinegun", 0,                                 { TW_KIND_HITSCAN,      0,    0,    0,    0,    0,  0.03f, TW_FAR,  25 }, TW_SEC_SHOT,     { TW_KIND_PROJ, 6000, 0, 0, 0, 60, 0, TW_FAR, 20 }, 1 },
	{ "oknex",        TW_PIERCE,                         { TW_KIND_HITSCAN,      0,    0,    0,    0,    0,  0,     TW_FAR, 100 }, TW_SEC_SHOT,     { TW_KIND_PROJ, 6000, 0, 0, 0, 60, 0, TW_FAR, 20 }, 10 },
	{ "okrpc",        0,                                 { TW_KIND_PROJ,      2500,    0, 5000,10000,  300,  0,     TW_FAR, 150 }, TW_SEC_SHOT,     { TW_KIND_PROJ, 6000, 0, 0, 0, 60, 0, TW_FAR, 20 }, 10 },
	{ "okshotgun",    0,                                 { TW_KIND_HITSCAN,      0,    0,    0,    0,    0,  0.07f, 1200,   170 }, TW_SEC_SHOT,     { TW_KIND_PROJ, 6000, 0, 0, 0, 60, 0, TW_FAR, 20 }, 3 },
};
// clang-format on
#define TW_NUM_WEAPONS ((int)(sizeof(touch_weapons) / sizeof(touch_weapons[0])))

// The stock weapon with this netname, or NULL (unknown, or a mod's weapon).
static const touch_wep_t *TouchWep_Find(const char *netname)
{
	int i;
	if (!netname || !netname[0])
		return NULL;
	for (i = 0; i < TW_NUM_WEAPONS; i++)
		if (!strcmp(touch_weapons[i].netname, netname))
			return &touch_weapons[i];
	return NULL;
}

static float tw_dot(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// ---------------------------------------------------------------------------
// Lead
// ---------------------------------------------------------------------------

// Longest flight a lead is computed for: beyond it the target has every
// chance to change direction, and the aim would swing far off the body.
#define TW_LEAD_MAX_TIME 1.5f

// Distance a projectile has flown after t seconds: constant speed, or the
// Devastator's start speed accelerating up to its maximum.
static float TouchWep_FlightDist(const touch_wep_fire_t *f, float t)
{
	float ta;
	if (t <= 0)
		return 0;
	if (f->accel <= 0 || f->speed_max <= f->speed)
		return f->speed * t;
	ta = (f->speed_max - f->speed) / f->accel; // time until full speed
	if (t <= ta)
		return f->speed * t + 0.5f * f->accel * t * t;
	return f->speed * ta + 0.5f * f->accel * ta * ta + f->speed_max * (t - ta);
}

// Time for a constant-speed shot to meet a target at rel (from the shooter)
// moving at constant vel: the smallest t > 0 with |rel + vel t| = speed t.
// (vel.vel - s^2) t^2 + 2 (rel.vel) t + rel.rel = 0. False when the target
// outruns the shot or the meeting is later than maxt.
static qbool TouchWep_InterceptTime(const float rel[3], const float vel[3], float speed, float maxt, float *t_out)
{
	float a = tw_dot(vel, vel) - speed * speed;
	float b = 2 * tw_dot(rel, vel);
	float c = tw_dot(rel, rel);
	float t;
	if (speed <= 0)
		return false;
	if (fabs(a) < 1e-3f)
	{
		// target speed equals shot speed: linear
		if (b >= 0)
			return false;
		t = -c / b;
	}
	else
	{
		float disc = b * b - 4 * a * c, sq, t1, t2;
		if (disc < 0)
			return false;
		sq = sqrt(disc);
		t1 = (-b - sq) / (2 * a);
		t2 = (-b + sq) / (2 * a);
		if (t1 > t2)
		{
			float tmp = t1;
			t1 = t2;
			t2 = tmp;
		}
		t = t1 > 0 ? t1 : t2;
		if (t <= 0)
			return false;
	}
	if (t > maxt)
		return false;
	*t_out = t;
	return true;
}

// Same for an accelerating rocket: the flown distance is not linear in t, so
// solve FlightDist(t) = |rel + vel t| by bisection. Its left side minus the
// right is negative at t = 0, and only crosses once for a target slower than
// the rocket's start speed; a faster one is caught by scanning first.
static qbool TouchWep_InterceptTimeAccel(const touch_wep_fire_t *f, const float rel[3], const float vel[3], float maxt, float *t_out)
{
	float lo = 0, hi = -1, t;
	int i;
	for (i = 1; i <= 30; i++)
	{
		float p[3];
		t = maxt * i / 30.0f;
		p[0] = rel[0] + vel[0] * t;
		p[1] = rel[1] + vel[1] * t;
		p[2] = rel[2] + vel[2] * t;
		if (TouchWep_FlightDist(f, t) >= sqrt(tw_dot(p, p)))
		{
			hi = t;
			break;
		}
		lo = t;
	}
	if (hi < 0)
		return false;
	for (i = 0; i < 30; i++)
	{
		float p[3];
		t = 0.5f * (lo + hi);
		p[0] = rel[0] + vel[0] * t;
		p[1] = rel[1] + vel[1] * t;
		p[2] = rel[2] + vel[2] * t;
		if (TouchWep_FlightDist(f, t) >= sqrt(tw_dot(p, p)))
			hi = t;
		else
			lo = t;
	}
	*t_out = hi;
	return true;
}

// Launch elevation (radians, up positive) that lobs a shot of `speed` plus
// `up` along the view's up vector through a point dx away horizontally and dz
// above, under gravity g; and the flight time. The launch velocity is the view
// direction rotated up by atan2(up, speed) at sqrt(speed^2 + up^2), so this is
// the textbook low-arc solve shifted by that angle.
static qbool TouchWep_Ballistic(float dx, float dz, float speed, float up, float g, float *elev_out, float *t_out)
{
	float v2 = speed * speed + up * up, v = sqrt(v2), off = atan2(up, speed);
	float disc, phi, ct;
	if (dx < 1)
		return false;
	if (g <= 0)
	{
		phi = atan2(dz, dx);
	}
	else
	{
		disc = v2 * v2 - g * (g * dx * dx + 2 * dz * v2);
		if (disc < 0)
			return false; // out of reach
		phi = atan((v2 - sqrt(disc)) / (g * dx));
	}
	ct = cos(phi);
	if (ct < 1e-3f)
		return false;
	*elev_out = phi - off;
	*t_out = dx / (v * ct);
	return true;
}

// Where to put the crosshair to hit a target whose centre is rel from the
// eye, moving at vel: writes the aim direction's point (eye-relative) into
// aim and the flight time into tof. latency (seconds) is added to the flight:
// the server sees the shot that much after the client sees the target. g is
// the world gravity. Instant weapons aim at the target itself. False when no
// lead is possible (target outruns the shot, out of reach, beyond
// TW_LEAD_MAX_TIME): the caller then aims at the body as before.
static qbool TouchWep_AimPoint(const touch_wep_fire_t *f, const float rel[3], const float vel[3], float latency, float g, float aim[3], float *tof)
{
	float p[3], t = 0;
	int i;
	*tof = 0;
	aim[0] = rel[0];
	aim[1] = rel[1];
	aim[2] = rel[2];
	if (!f || f->kind == TW_KIND_HITSCAN || f->kind == TW_KIND_BEAM || f->kind == TW_KIND_MELEE || f->kind == TW_KIND_NONE)
		return true;
	// where the target is when the server fires
	for (i = 0; i < 3; i++)
		p[i] = rel[i] + vel[i] * latency;
	if (f->kind == TW_KIND_PROJ)
	{
		qbool ok = f->accel > 0 ? TouchWep_InterceptTimeAccel(f, p, vel, TW_LEAD_MAX_TIME, &t)
			: TouchWep_InterceptTime(p, vel, f->speed, TW_LEAD_MAX_TIME, &t);
		if (!ok)
			return false;
		for (i = 0; i < 3; i++)
			aim[i] = p[i] + vel[i] * t;
		*tof = t;
		return true;
	}
	if (f->kind == TW_KIND_BALLISTIC)
	{
		// a few fixed-point rounds: flight time -> where the target is then
		// -> the lob that reaches it -> its flight time
		float q[3], elev = 0, dx;
		for (i = 0; i < 6; i++)
		{
			int k;
			for (k = 0; k < 3; k++)
				q[k] = p[k] + vel[k] * t;
			dx = sqrt(q[0] * q[0] + q[1] * q[1]);
			if (!TouchWep_Ballistic(dx, q[2], f->speed, f->speed_up, g, &elev, &t))
				return false;
			if (t > TW_LEAD_MAX_TIME)
				return false;
		}
		for (i = 0; i < 3; i++)
			q[i] = p[i] + vel[i] * t;
		dx = sqrt(q[0] * q[0] + q[1] * q[1]);
		// a point along the launch direction at the target's horizontal distance
		aim[0] = q[0];
		aim[1] = q[1];
		aim[2] = dx * tan(elev);
		*tof = t;
		return true;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Airborne targets
// ---------------------------------------------------------------------------

// After landing, how much of its horizontal speed a target keeps: a player
// strafing or bunny-hopping carries on at full speed.
#define TW_LAND_SLOW 1.0f

// Where a target that was at rel with velocity vel is t seconds later. With
// t_land < 0 it runs on the ground in a straight line; otherwise it is in
// the air, falls under gravity g, lands t_land seconds from now and then
// carries on at TW_LAND_SLOW of its horizontal speed.
static void TouchWep_TargetAt(const float rel[3], const float vel[3], float g, float t_land, float t, float out[3])
{
	float ta;
	int i;
	if (t_land < 0)
	{
		for (i = 0; i < 3; i++)
			out[i] = rel[i] + vel[i] * t;
		return;
	}
	ta = t < t_land ? t : t_land;
	out[0] = rel[0] + vel[0] * ta;
	out[1] = rel[1] + vel[1] * ta;
	out[2] = rel[2] + vel[2] * ta - 0.5f * g * ta * ta;
	if (t > t_land)
	{
		out[0] += vel[0] * TW_LAND_SLOW * (t - t_land);
		out[1] += vel[1] * TW_LAND_SLOW * (t - t_land);
	}
}

// Seconds until a body falling from height h above its landing floor (h > 0)
// with vertical speed vz lands under gravity g: the positive root of
// h + vz t - g t^2 / 2 = 0. 0 when it is already at or below the floor.
static float TouchWep_LandTime(float h, float vz, float g)
{
	float disc;
	if (h <= 0 || g <= 0)
		return 0;
	disc = vz * vz + 2 * g * h;
	return (vz + sqrt(disc)) / g;
}

// TouchWep_AimPoint for a target in the air (t_land >= 0; < 0 is the
// ground case, unchanged): the shot meets it on its fall, or, if the fall
// ends first, where it lands. A straight shot solves flown distance =
// distance to the target at that time by scanning and bisection; a lob
// iterates as on the ground.
static qbool TouchWep_AimPointAir(const touch_wep_fire_t *f, const float rel[3], const float vel[3], float latency, float g, float t_land, float aim[3], float *tof)
{
	float q[3], t = 0;
	int i;
	if (t_land < 0)
		return TouchWep_AimPoint(f, rel, vel, latency, g, aim, tof);
	*tof = 0;
	aim[0] = rel[0];
	aim[1] = rel[1];
	aim[2] = rel[2];
	if (!f || f->kind == TW_KIND_HITSCAN || f->kind == TW_KIND_BEAM || f->kind == TW_KIND_MELEE || f->kind == TW_KIND_NONE)
		return true;
	if (f->kind == TW_KIND_PROJ)
	{
		float lo = 0, hi = -1;
		for (i = 1; i <= 45; i++)
		{
			t = TW_LEAD_MAX_TIME * i / 45.0f;
			TouchWep_TargetAt(rel, vel, g, t_land, t + latency, q);
			if (TouchWep_FlightDist(f, t) >= sqrt(tw_dot(q, q)))
			{
				hi = t;
				break;
			}
			lo = t;
		}
		if (hi < 0)
			return false;
		for (i = 0; i < 30; i++)
		{
			t = 0.5f * (lo + hi);
			TouchWep_TargetAt(rel, vel, g, t_land, t + latency, q);
			if (TouchWep_FlightDist(f, t) >= sqrt(tw_dot(q, q)))
				hi = t;
			else
				lo = t;
		}
		TouchWep_TargetAt(rel, vel, g, t_land, hi + latency, aim);
		*tof = hi;
		return true;
	}
	if (f->kind == TW_KIND_BALLISTIC)
	{
		float elev = 0, dx;
		for (i = 0; i < 8; i++)
		{
			TouchWep_TargetAt(rel, vel, g, t_land, t + latency, q);
			dx = sqrt(q[0] * q[0] + q[1] * q[1]);
			if (!TouchWep_Ballistic(dx, q[2], f->speed, f->speed_up, g, &elev, &t) || t > TW_LEAD_MAX_TIME)
				return false;
		}
		TouchWep_TargetAt(rel, vel, g, t_land, t + latency, q);
		dx = sqrt(q[0] * q[0] + q[1] * q[1]);
		aim[0] = q[0];
		aim[1] = q[1];
		aim[2] = dx * tan(elev);
		*tof = t;
		return true;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Erratic strafes
// ---------------------------------------------------------------------------

// How much of a target's sideways motion to lead (0..1). A target that
// reverses every t_rev seconds at a random moment is, t seconds ahead, on
// average v t (1 - t / t_rev) away. That holds for slow shots; measured, it
// costs hits for fast ones (HLAC, Crylink volley), whose short lead is
// mostly right whatever the dodger does next. So: full lead while the shot
// arrives within half a period, the average share beyond. t_rev <= 0: no
// reversals seen, full lead. t_since is kept for callers; unused.
static float TouchWep_LeadScale(float t, float t_rev, float t_since)
{
	float k;
	(void)t_since;
	if (t_rev <= 0 || t < 0.5f * t_rev)
		return 1;
	k = 1 - t / t_rev;
	return k < 0 ? 0 : k > 1 ? 1 : k;
}

// Share of a target's sideways motion to lead over a horizon of h seconds
// (flight plus latency), by motion class. Replaying 230,000 frames of skill
// 8-10 bot movement through the tracker (tests/touch-aim/eval_lead.c) at
// 50 / 250 / 350 ms of ping, the full lead hit most often for every class,
// dodgers included: a shrunk lead (TouchWep_LeadScale, tuned on scripted
// dodgers that reverse like clockwork) cut the dodger shots landing within
// 60 units at 500 units from 35 % to 15 % at 250 ms. So: the full lead.
static float TouchWep_LeadShare(int mclass, float h, float t_rev)
{
	(void)mclass;
	(void)h;
	(void)t_rev;
	return 1;
}

// How fast the part of the lead that comes from the target's motion may
// move: TW_LEAD_SLEW times the target's own speed, and at least
// TW_LEAD_SLEW_MIN units per second. A strafe that reverses flips that part
// from one side of the body to the other within a few frames; slewed, the
// aim point slides across the body instead of jumping, at an angular speed
// the aim pull can follow, while a steady strafe's lead is untouched.
#define TW_LEAD_SLEW      2.0f
#define TW_LEAD_SLEW_MIN  100.0f

// Move cur toward want by at most the slew allowed over dt seconds for a
// target moving at target_speed. The slewed lead is never longer than the
// wanted one: it may only lag in direction, so a lead that collapses (the
// target stops, or one bad velocity sample passes) shrinks at once.
static void TouchWep_LeadSlew(float cur[3], const float want[3], float target_speed, float dt)
{
	float d[3], len, step, curlen, wl;
	int i;
	for (i = 0; i < 3; i++)
		d[i] = want[i] - cur[i];
	len = sqrt(tw_dot(d, d));
	step = (TW_LEAD_SLEW * target_speed + TW_LEAD_SLEW_MIN) * (dt > 0 ? dt : 0);
	if (len <= step || len < 1e-4f)
	{
		for (i = 0; i < 3; i++)
			cur[i] = want[i];
		return;
	}
	for (i = 0; i < 3; i++)
		cur[i] += d[i] * (step / len);
	curlen = sqrt(tw_dot(cur, cur));
	wl = sqrt(tw_dot(want, want));
	if (curlen > wl && curlen > 1e-4f)
		for (i = 0; i < 3; i++)
			cur[i] *= wl / curlen;
}

// ---------------------------------------------------------------------------
// When to pull the trigger
// ---------------------------------------------------------------------------

// Degrees from the crosshair to the (lead-adjusted) body edge within which a
// shot is worth firing: the player's radius setting, narrowed to what the
// shot can actually cover at this distance -- half the spread cone for
// pellets and sprayed bullets, half the splash radius for explosives. An
// exact instant hit fires only on the body.
static float TouchWep_FireWindow(const touch_wep_fire_t *f, float dist, float radius_deg)
{
	float cover = 0;
	if (radius_deg <= 0 || !f)
		return 0;
	if (dist < 1)
		dist = 1;
	if (f->spread > 0)
		cover = (float)(atan(0.5f * f->spread) * TW_DEG);
	if (f->splash > 0 && f->kind != TW_KIND_HITSCAN)
	{
		float s = (float)(atan(0.5f * f->splash / dist) * TW_DEG);
		if (s > cover)
			cover = s;
	}
	return cover < radius_deg ? cover : radius_deg;
}

// ---------------------------------------------------------------------------
// Primary or secondary
// ---------------------------------------------------------------------------

#define TW_MODE_NONE      0
#define TW_MODE_PRIMARY   1
#define TW_MODE_SECONDARY 2

// Margin added to a weapon's splash radius before its own blast counts as safe.
#define TW_SPLASH_MARGIN    40.0f
// Shotgun melee reach used for the choice: a little inside the swing's 120.
#define TW_MELEE_DIST      110.0f
// The MachineGun's burst (no spread) is worth it only this far out.
#define TW_MG_BURST_DIST  1500.0f

typedef struct touch_wep_situation_s
{
	float dist;        // eye to the target's centre
	float impact_dist; // eye to where the shot meets it (the lead point)
	qbool pri_ready;   // primary has ammo for a shot (or needs none)
	qbool alt_ready;   // secondary can fire now (TOUCH_ALTFIRE_READY)
	qbool use_alt;     // the player allows secondary fire (cl_touch_autoshoot_alt)
} touch_wep_situation_t;

// Reasons, for cl_touch_aim_debug.
static const char *const touch_wep_why[] =
{
	"no target", "default primary", "not an auto-fire weapon", "melee in reach", "crylink spread",
	"burst at long range", "beam out of range: bolt", "out of ammo: secondary", "too close for own splash",
	"out of range", "secondary off: primary", "no ammo", "orbs for a combo",
};
#define TW_WHY_NONE         0
#define TW_WHY_DEFAULT      1
#define TW_WHY_NOAUTO       2
#define TW_WHY_MELEE        3
#define TW_WHY_CRYLINK      4
#define TW_WHY_BURST        5
#define TW_WHY_BOLT         6
#define TW_WHY_EMPTY_ALT    7
#define TW_WHY_OWN_SPLASH   8
#define TW_WHY_RANGE        9
#define TW_WHY_ALT_OFF     10
#define TW_WHY_NO_AMMO     11
#define TW_WHY_ORBS        12

static qbool tw_splash_unsafe(const touch_wep_t *w, const touch_wep_fire_t *f, float impact_dist)
{
	if (!f->splash || (w->flags & TW_WEAK_SPLASH) || f->kind == TW_KIND_HITSCAN)
		return false;
	return impact_dist < f->splash + TW_SPLASH_MARGIN;
}

// Which fire to use on a target. Rules follow Xonotic's own bots (each
// weapon's wr_aim) and keep the player out of their own blast.
static int TouchWep_FireMode(const touch_wep_t *w, const touch_wep_situation_t *s, int *why)
{
	qbool sec_shot, alt;
	int mode = TW_MODE_PRIMARY;
	*why = TW_WHY_DEFAULT;
	if (!w)
		return TW_MODE_PRIMARY; // unknown weapon: primary, as always
	if ((w->flags & TW_NO_AUTOFIRE) || w->pri.kind == TW_KIND_NONE)
	{
		*why = TW_WHY_NOAUTO;
		return TW_MODE_NONE;
	}
	alt = s->use_alt && s->alt_ready;
	sec_shot = alt && (w->sec_what == TW_SEC_SHOT || w->sec_what == TW_SEC_MELEE);

	if (!strcmp(w->netname, "shotgun") && alt && s->dist <= TW_MELEE_DIST)
	{
		*why = TW_WHY_MELEE;
		return TW_MODE_SECONDARY;
	}
	if (!strcmp(w->netname, "crylink") && sec_shot)
	{
		mode = TW_MODE_SECONDARY;
		*why = TW_WHY_CRYLINK;
	}
	else if (!strcmp(w->netname, "machinegun") && sec_shot && s->dist > TW_MG_BURST_DIST)
	{
		mode = TW_MODE_SECONDARY;
		*why = TW_WHY_BURST;
	}
	else if (!strcmp(w->netname, "arc") && s->dist > w->pri.range)
	{
		if (!sec_shot)
		{
			*why = s->use_alt ? TW_WHY_RANGE : TW_WHY_ALT_OFF;
			return TW_MODE_NONE;
		}
		mode = TW_MODE_SECONDARY;
		*why = TW_WHY_BOLT;
	}
	else if (!s->pri_ready)
	{
		// e.g. the Vaporizer's blaster shot, an Overkill weapon's
		if (!sec_shot || w->sec.kind == TW_KIND_MELEE)
		{
			*why = TW_WHY_NO_AMMO;
			return TW_MODE_NONE;
		}
		mode = TW_MODE_SECONDARY;
		*why = TW_WHY_EMPTY_ALT;
	}

	{
		const touch_wep_fire_t *f = mode == TW_MODE_SECONDARY ? &w->sec : &w->pri;
		if (s->dist > f->range)
		{
			*why = TW_WHY_RANGE;
			return TW_MODE_NONE;
		}
		if (tw_splash_unsafe(w, f, s->impact_dist) || tw_splash_unsafe(w, f, s->dist))
		{
			// the other fire, if it is a shot that is safe here
			const touch_wep_fire_t *o = mode == TW_MODE_SECONDARY ? &w->pri : &w->sec;
			qbool o_ok = mode == TW_MODE_SECONDARY ? s->pri_ready : sec_shot;
			*why = TW_WHY_OWN_SPLASH;
			if (o_ok && o->kind != TW_KIND_NONE && s->dist <= o->range
				&& !tw_splash_unsafe(w, o, s->impact_dist) && !tw_splash_unsafe(w, o, s->dist))
				return mode == TW_MODE_SECONDARY ? TW_MODE_PRIMARY : TW_MODE_SECONDARY;
			return TW_MODE_NONE;
		}
	}
	return mode;
}

// Electro combo (cl_touch_autoshoot_combo): three orbs (30 each on touch,
// 2 cells each) lobbed where the enemy is, then the primary bolt's blast
// sets off every orb within 300 units, 50 damage each over 150 units. Worth
// it against an enemy that stays within an orb's reach for the 1.2 s until
// the bolt can follow: one standing or coming at the player. Measured (8 s,
// local): standing 512 -> 622, running at the player and away 440 -> 592,
// steady strafe 347 -> 422; a dodger 100 -> 70 and a group of three (whom
// the bolt at the feet already catches) 1102 -> 1016 lost, so not those.
// Not too close (the combo would reach the player) nor beyond the lob.
#define TW_ORB_MIN      300.0f
#define TW_ORB_MAX      900.0f
#define TW_ORB_CELLS    10      // three orbs and a bolt
#define TW_ORB_CYCLE    4.0f    // orbs live 4 s: no new ones while these may still go off
static qbool TouchWep_ElectroOrbs(int mclass, float range_rate, float dist, int cells, int group)
{
	if (dist < TW_ORB_MIN || dist > TW_ORB_MAX || (cells >= 0 && cells < TW_ORB_CELLS) || group > 0)
		return false;
	return mclass == TW_CLASS_STAND || range_rate < -150;
}

// ---------------------------------------------------------------------------
// Last seen: what the player knows of an enemy that went out of sight
// ---------------------------------------------------------------------------

// Built only from frames in which the enemy was in sight; a frame without
// sight leaves it as it was. For TW_GHOST_PREDICT seconds the enemy is
// expected along its last seen velocity, then at that point until
// TW_GHOST_KEEP ("was there"), then it is forgotten.
#define TW_GHOST_PREDICT 1.0f
#define TW_GHOST_KEEP    3.0f
typedef struct touch_wep_ghost_s
{
	float org[3], vel[3];
	double seen;      // last time in sight, 0 never
} touch_wep_ghost_t;

static void TouchWep_GhostUpdate(touch_wep_ghost_t *g, qbool visible, const float org[3], const float vel[3], double now)
{
	int k;
	if (!visible)
		return;
	for (k = 0; k < 3; k++)
	{
		g->org[k] = org[k];
		g->vel[k] = vel[k];
	}
	g->seen = now;
}

// Where the enemy is expected now; false once it is forgotten. *conf: 1
// just after it went out of sight, falling to 0 at TW_GHOST_KEEP.
static qbool TouchWep_GhostPoint(const touch_wep_ghost_t *g, double now, float out[3], float *conf)
{
	float dt = (float)(now - g->seen), te;
	int k;
	if (g->seen <= 0 || dt < 0 || dt > TW_GHOST_KEEP)
		return false;
	te = dt < TW_GHOST_PREDICT ? dt : TW_GHOST_PREDICT;
	for (k = 0; k < 3; k++)
		out[k] = g->org[k] + g->vel[k] * te;
	out[2] = g->org[2]; // along the floor it was on
	*conf = 1 - dt / TW_GHOST_KEEP;
	return true;
}

// ---------------------------------------------------------------------------
// Which weapon
// ---------------------------------------------------------------------------

// Xonotic's bot preference lists (xonotic-server.cfg bot_ai_custom_weapon_
// priority_*) without what makes no sense for a thumb: tuba, mine layer,
// port-o-launch, hook, fireball, seeker. Best first.
static const char *const touch_wep_far[] =
{
	"vaporizer", "oknex", "vortex", "rifle", "electro", "devastator", "mortar", "hagar", "hlac", "crylink",
	"blaster", "okmachinegun", "machinegun", "okshotgun", "shotgun", NULL
};
static const char *const touch_wep_mid[] =
{
	"vaporizer", "devastator", "oknex", "vortex", "mortar", "electro", "okmachinegun", "machinegun", "arc",
	"crylink", "hlac", "hagar", "okshotgun", "shotgun", "blaster", "rifle", NULL
};
static const char *const touch_wep_close[] =
{
	"vaporizer", "oknex", "vortex", "okshotgun", "shotgun", "okmachinegun", "machinegun", "arc", "hlac",
	"hagar", "crylink", "mortar", "electro", "devastator", "blaster", "rifle", NULL
};
// The bots' band edges ("300 850") on clamp(distance - 200, 10, 10000).
#define TW_BAND_MID 300.0f
#define TW_BAND_FAR 850.0f
// A switch must win by at least this many places in the band's list.
#define TW_SWITCH_MARGIN 2

static int TouchWep_Band(float dist)
{
	float d = dist - 200;
	d = d < 10 ? 10 : d > 10000 ? 10000 : d;
	return d > TW_BAND_FAR ? 2 : d > TW_BAND_MID ? 1 : 0;
}

// Place of the weapon in the list for this distance (0 best), or -1 when it
// is not on it (never chosen).
static int TouchWep_Rank(const char *netname, float dist)
{
	const char *const *list;
	int band = TouchWep_Band(dist), i;
	list = band == 2 ? touch_wep_far : band == 1 ? touch_wep_mid : touch_wep_close;
	if (!netname)
		return -1;
	for (i = 0; list[i]; i++)
		if (!strcmp(list[i], netname))
			return i;
	return -1;
}

// Whether a weapon can do anything at this distance: in range, and not
// a splash weapon whose blast would reach the player (weak splash aside).
static qbool TouchWep_Usable(const touch_wep_t *w, float dist)
{
	if (!w || (w->flags & TW_NO_AUTOFIRE) || w->pri.kind == TW_KIND_NONE)
		return false;
	if (dist > w->pri.range)
		return w->sec_what == TW_SEC_SHOT && dist <= w->sec.range && !tw_splash_unsafe(w, &w->sec, dist);
	return !tw_splash_unsafe(w, &w->pri, dist);
}

// Best owned weapon for the distance among names[0..n-1], each with
// ready[i] (has ammo for a shot): its index, or -1. *rank_out gets its rank.
// fallback_last: a fallback (the Blaster) only when no real weapon can shoot here.
static int TouchWep_Choose(const char *const *names, const qbool *ready, int n, float dist, qbool fallback_last, int *rank_out)
{
	int i, best = -1, best_rank = 1 << 30, pass;
	for (pass = fallback_last ? 0 : 1; pass < 2 && best < 0; pass++)
		for (i = 0; i < n; i++)
		{
			const touch_wep_t *w = TouchWep_Find(names[i]);
			int r;
			if (!ready[i] || !w || (w->flags & TW_NO_SWITCH_TO) || !TouchWep_Usable(w, dist))
				continue;
			if (!pass && (w->flags & TW_FALLBACK))
				continue;
			r = TouchWep_Rank(names[i], dist);
			if (r < 0 || r >= best_rank)
				continue;
			best = i;
			best_rank = r;
		}
	*rank_out = best >= 0 ? best_rank : -1;
	return best;
}

// A last-resort weapon (TW_FALLBACK): picked by auto-switch only when no
// other owned weapon can shoot, and never in place of a held one that can.
static qbool TouchWep_IsFallback(const char *netname)
{
	const touch_wep_t *w = TouchWep_Find(netname);
	return w && (w->flags & TW_FALLBACK);
}

// Whether to leave the current weapon for a candidate of rank cand_rank:
// always when the current one cannot shoot here (no ammo, out of range,
// inside its own splash, not on the list), otherwise only for a clear win.
static qbool TouchWep_ShouldSwitch(int cur_rank, qbool cur_can_shoot, int cand_rank)
{
	if (cand_rank < 0)
		return false;
	if (!cur_can_shoot || cur_rank < 0)
		return true;
	return cand_rank + TW_SWITCH_MARGIN <= cur_rank;
}

// ---------------------------------------------------------------------------
// Teammates
// ---------------------------------------------------------------------------

#define TW_MATE_NONE  0
#define TW_MATE_LINE  1 // on the line of the shot, before the enemy (or anywhere on it for a piercing shot)
#define TW_MATE_BLAST 2 // inside the blast at the impact point
#define TW_MATE_MARGIN 16.0f

static float TouchWep_BoxGap(const float p[3], const float bmin[3], const float bmax[3]);

// Ray from o along unit d into an axis-aligned box: the entry distance.
static qbool TouchWep_RayBox(const float o[3], const float d[3], const float bmin[3], const float bmax[3], float *tnear)
{
	float t0 = 0, t1 = 1e9f;
	int k;
	for (k = 0; k < 3; k++)
	{
		if (fabs(d[k]) < 1e-6f)
		{
			if (o[k] < bmin[k] || o[k] > bmax[k])
				return false;
		}
		else
		{
			float inv = 1.0f / d[k], a = (bmin[k] - o[k]) * inv, b = (bmax[k] - o[k]) * inv;
			if (a > b)
			{
				float tmp = a;
				a = b;
				b = tmp;
			}
			t0 = a > t0 ? a : t0;
			t1 = b < t1 ? b : t1;
			if (t0 > t1)
				return false;
		}
	}
	*tnear = t0;
	return true;
}

// Whether a shot of fire f (pierce: it goes on through players) from eye
// along dir at an enemy target_dist away, meeting it at impact, would hit
// or catch a teammate whose box is bmin..bmax. The line is widened by the
// shot's spread at that distance; a lob is judged by its blast only.
static int TouchWep_MateBlocks(const touch_wep_fire_t *f, qbool pierce, const float eye[3], const float dir[3], float target_dist, const float impact[3],
	const float bmin[3], const float bmax[3])
{
	float t, pad, lo[3], hi[3], c[3];
	int k;
	if (!f || f->kind == TW_KIND_NONE)
		return TW_MATE_NONE;
	if (f->kind != TW_KIND_BALLISTIC)
	{
		for (k = 0; k < 3; k++)
		{
			lo[k] = bmin[k];
			hi[k] = bmax[k];
		}
		// widened by half the spread cone at the box's own distance
		for (k = 0; k < 3; k++)
			c[k] = 0.5f * (bmin[k] + bmax[k]) - eye[k];
		pad = 4 + 0.5f * f->spread * sqrt(tw_dot(c, c));
		for (k = 0; k < 3; k++)
		{
			lo[k] -= pad;
			hi[k] += pad;
		}
		if (TouchWep_RayBox(eye, dir, lo, hi, &t) && (pierce || t < target_dist))
			return TW_MATE_LINE;
	}
	if (f->splash > 0 && f->kind != TW_KIND_HITSCAN && f->kind != TW_KIND_BEAM && TouchWep_BoxGap(impact, bmin, bmax) < f->splash + TW_MATE_MARGIN)
		return TW_MATE_BLAST;
	return TW_MATE_NONE;
}

// ---------------------------------------------------------------------------
// Devastator detonation
// ---------------------------------------------------------------------------

// Fraction of the splash radius the enemy must be inside before the rocket
// is set off: well inside, so the blast does real damage.
#define TW_DETONATE_FRACTION 0.8f
// Seconds ahead the "about to pass" test looks (two frames at 60 fps, plus
// the trip of the attack2 press to the server).
#define TW_DETONATE_LOOKAHEAD 0.06f

// Whether to detonate a rocket at `rocket` flying at `rvel` relative to the
// enemy (rocket velocity minus enemy velocity): the enemy is
// inside TW_DETONATE_FRACTION of the radius and the rocket is no longer
// getting closer, or it is inside the radius and about to fly past. Never
// while the player (self) is inside the radius plus a margin.
static qbool TouchWep_Detonate(const float rocket[3], const float rvel[3], const float enemy[3], const float self[3], float radius)
{
	float rel[3], me[3], d, closing, ahead[3], d_ahead;
	int i;
	for (i = 0; i < 3; i++)
	{
		rel[i] = enemy[i] - rocket[i];
		me[i] = self[i] - rocket[i];
	}
	if (sqrt(tw_dot(me, me)) < radius + TW_SPLASH_MARGIN)
		return false;
	d = sqrt(tw_dot(rel, rel));
	if (d >= radius)
		return false;
	closing = d > 1e-3f ? tw_dot(rel, rvel) / d : 0; // > 0: getting closer
	if (d < TW_DETONATE_FRACTION * radius && closing <= 0)
		return true;
	for (i = 0; i < 3; i++)
		ahead[i] = rel[i] - rvel[i] * TW_DETONATE_LOOKAHEAD;
	d_ahead = sqrt(tw_dot(ahead, ahead));
	// about to pass: it will be farther in a moment than it is now
	return d_ahead > d;
}

// The server's rocket guidance (W_GuideProjectile, W_SteerProjectileTo):
// each frame the rocket turns at most rate_deg * dt toward a goal on the
// player's view ray, goal_ahead units beyond its own projection onto the
// ray, and speeds up toward speed_max. One step of that: dir (unit) and
// *speed updated, org moved by dt. eye/view: the view ray (view unit).
static void TouchWep_GuideStep(float org[3], float dir[3], float *speed, const float eye[3], const float view[3],
	float goal_ahead, float rate_deg, float accel, float speed_max, float dt)
{
	float rel[3], want[3], along, len, c, ang, maxang;
	int k;
	for (k = 0; k < 3; k++)
		rel[k] = org[k] - eye[k];
	along = tw_dot(rel, view) + goal_ahead;
	for (k = 0; k < 3; k++)
		want[k] = eye[k] + along * view[k] - org[k];
	len = sqrt(tw_dot(want, want));
	if (len > 1e-3f)
	{
		for (k = 0; k < 3; k++)
			want[k] /= len;
		c = tw_dot(dir, want);
		c = c > 1 ? 1 : c < -1 ? -1 : c;
		ang = acos(c);
		maxang = rate_deg * dt * (float)(M_PI / 180.0);
		if (ang <= maxang)
			memcpy(dir, want, sizeof(want));
		else if (c > -0.9998f)
		{
			// rotate dir toward want by maxang, in their plane
			float sa = sin(ang), a = sin(ang - maxang) / sa, b = sin(maxang) / sa;
			for (k = 0; k < 3; k++)
				dir[k] = a * dir[k] + b * want[k];
			len = sqrt(tw_dot(dir, dir));
			for (k = 0; k < 3; k++)
				dir[k] /= len;
		}
	}
	if (*speed < speed_max)
		*speed = *speed + accel * dt < speed_max ? *speed + accel * dt : speed_max;
	for (k = 0; k < 3; k++)
		org[k] += dir[k] * *speed * dt;
}

// A rocket flying straight (no longer guided) past a target that keeps its
// velocity: the closest they come within tmax seconds, and when (*t).
// rel: target minus rocket; relv: target velocity minus rocket velocity.
static float TouchWep_PassDistance(const float rel[3], const float relv[3], float tmax, float *t)
{
	float vv = tw_dot(relv, relv), tc = vv > 1e-6f ? -tw_dot(rel, relv) / vv : 0, d[3];
	int k;
	tc = tc < 0 ? 0 : tc > tmax ? tmax : tc;
	for (k = 0; k < 3; k++)
		d[k] = rel[k] + relv[k] * tc;
	*t = tc;
	return sqrt(tw_dot(d, d));
}

// ---------------------------------------------------------------------------
// Headshots
// ---------------------------------------------------------------------------

// What the server pays for a head hit (Headshot() in server/weapons/
// tracing.qc): a box 0.6 of the player's width, from 1.3 * view_ofs_z -
// 0.3 * maxs_z up to the top of the box -- the top 13 units of a standing
// player. Only bullets fired with a headshot multiplier get more damage, and
// in Xonotic's code that is the Rifle alone (fireBullet_falloff's
// headshot_multiplier: 0 for the MachineGun, Shotgun and Overkill guns; the
// Vortex and Vaporizer only announce a headshot, no extra damage). The stock
// balance sets the Rifle's to 0 too, so by default no weapon has a bonus;
// rifle_mult is the server's g_balance_rifle_primary_headshot_multiplier
// when the player knows it differs (1.25 in the Nexuiz 2.5 balance).
#define TW_HEAD_ZONE_H   13.0f
#define TW_HEAD_ZONE_W   0.6f

static float TouchWep_HeadMultiplier(const char *netname, int mode, float rifle_mult)
{
	if (netname && !strcmp(netname, "rifle") && mode == TW_MODE_PRIMARY)
		return rifle_mult;
	return 0;
}

// Aim error (one standard deviation, in units at the target) of a thumb
// with aim assist against a target moving that way, plus the weapon's own
// spread (radians): the model behind the expected-damage table.
static float TouchWep_AimSigma(float dist, int mclass, float spread)
{
	static const float deg[4] = { 0.35f, 0.7f, 1.2f, 0.9f };
	float d = deg[mclass < 0 || mclass > 3 ? 1 : mclass] * (float)(M_PI / 180.0) + spread;
	return dist * d;
}

// Probability that a normal error of deviation sigma around c falls in [a, b].
static float tw_pin(float a, float b, float c, float sigma)
{
	if (sigma <= 1e-4f)
		return c >= a && c <= b ? 1.0f : 0.0f;
	return 0.5f * (float)(erf((b - c) / (sigma * 1.41421356f)) - erf((a - c) / (sigma * 1.41421356f)));
}

// Expected damage of one shot (base damage 1) aimed at height z on a body
// of half width hw from bmin_z to bmax_z, with a head bonus mult.
static float TouchWep_ShotValue(float mult, float hw, float bmin_z, float bmax_z, float z, float sigma)
{
	float pbody = tw_pin(-hw, hw, 0, sigma) * tw_pin(bmin_z, bmax_z, z, sigma);
	float phead = tw_pin(-TW_HEAD_ZONE_W * hw, TW_HEAD_ZONE_W * hw, 0, sigma) * tw_pin(bmax_z - TW_HEAD_ZONE_H, bmax_z, z, sigma);
	return pbody + (mult > 1 ? (mult - 1) * phead : 0);
}

// Whether to aim at the head instead of the chest: only with a real bonus,
// and only where the smaller zone still pays more on average (5 % margin).
static qbool TouchWep_HeadBetter(float mult, float hw, float bmin_z, float bmax_z, float sigma)
{
	float chest, head;
	if (mult <= 1)
		return false;
	chest = TouchWep_ShotValue(mult, hw, bmin_z, bmax_z, 0.5f * (bmin_z + bmax_z), sigma);
	head = TouchWep_ShotValue(mult, hw, bmin_z, bmax_z, bmax_z - 0.5f * TW_HEAD_ZONE_H, sigma);
	return head > 1.05f * chest;
}

// ---------------------------------------------------------------------------
// Splash and groups
// ---------------------------------------------------------------------------

// Gap from a point to a box (0 inside), as RadiusDamage measures it.
static float TouchWep_BoxGap(const float p[3], const float bmin[3], const float bmax[3])
{
	float d[3];
	int k;
	for (k = 0; k < 3; k++)
		d[k] = p[k] < bmin[k] ? bmin[k] - p[k] : p[k] > bmax[k] ? p[k] - bmax[k] : 0;
	return sqrt(tw_dot(d, d));
}

// Splash damage at a gap from the blast: core at 0, falling linearly to edge
// at the radius, nothing beyond (RadiusDamageForSource).
static float TouchWep_SplashDamage(float gap, float core, float edge, float radius)
{
	float f;
	if (radius <= 0 || gap > radius)
		return 0;
	f = gap / radius;
	return core * (1 - f) + edge * f;
}

// A body the splash may catch: its box (relative to anything, as long as the
// blast point uses the same origin) and how much its damage counts:
// > 0 an enemy (1, or more for the one the player aims at), < 0 a teammate
// or the player (a penalty).
typedef struct touch_wep_body_s
{
	float bmin[3], bmax[3];
	float weight;
} touch_wep_body_t;

// Expected splash damage of a blast at p over the bodies, weighted.
static float TouchWep_GroupScore(const float p[3], const touch_wep_body_t *bodies, int n, float core, float edge, float radius)
{
	float sum = 0;
	int i;
	for (i = 0; i < n; i++)
		sum += bodies[i].weight * TouchWep_SplashDamage(TouchWep_BoxGap(p, bodies[i].bmin, bodies[i].bmax), core, edge, radius);
	return sum;
}

// Hysteresis for a new group aim point: it must score this much more than
// the one in use.
#define TW_GROUP_HYSTERESIS 1.15f

// Index of the best of the candidate blast points, or -1 when none scores
// above 0. cur: the candidate in use last time (-1 none), kept unless
// another beats it by TW_GROUP_HYSTERESIS.
static int TouchWep_BestImpact(const float (*cands)[3], int ncand, const touch_wep_body_t *bodies, int n, float core, float edge, float radius, int cur, float *score_out)
{
	int i, best = -1;
	float best_s = 0, cur_s = -1;
	for (i = 0; i < ncand; i++)
	{
		float sc = TouchWep_GroupScore(cands[i], bodies, n, core, edge, radius);
		if (i == cur)
			cur_s = sc;
		if (sc > best_s)
		{
			best_s = sc;
			best = i;
		}
	}
	if (cur >= 0 && cur < ncand && cur_s > 0 && best_s < cur_s * TW_GROUP_HYSTERESIS)
	{
		best = cur;
		best_s = cur_s;
	}
	*score_out = best_s;
	return best;
}

// Remote detonation by total damage (Xonotic's own Devastator bot rule,
// with latency): the blast happens where the server has the rocket when the
// press arrives, latency seconds from what the client sees, and everyone
// moves meanwhile. Detonate when the enemies' share is worth having (at
// least 10 % of the core damage), beats 1.5 times the damage to the player
// and teammates (negative weights), and will not grow by waiting one more
// frame (dt) -- unless the rocket is about to hit someone directly, which
// does more. Never with the player within the radius plus a margin of the
// blast point (self: the player's box, relative to the same origin).
#define TW_REMOTE_CORE   70.0f
#define TW_REMOTE_EDGE   35.0f
static qbool TouchWep_DetonateGroup(const float rocket[3], const float rvel[3], const touch_wep_body_t *bodies, const float (*vels)[3], int n,
	const float selfmin[3], const float selfmax[3], float radius, float latency, float dt)
{
	float x0[3], x1[3], good0 = 0, good1 = 0, bad0 = 0;
	int i, k;
	for (k = 0; k < 3; k++)
	{
		x0[k] = rocket[k] + rvel[k] * latency;
		x1[k] = x0[k] + rvel[k] * dt;
	}
	if (TouchWep_BoxGap(x0, selfmin, selfmax) < radius + TW_SPLASH_MARGIN)
		return false;
	for (i = 0; i < n; i++)
	{
		touch_wep_body_t b0 = bodies[i], b1 = bodies[i];
		float d0, d1;
		for (k = 0; k < 3; k++)
		{
			b0.bmin[k] += vels[i][k] * latency;
			b0.bmax[k] += vels[i][k] * latency;
			b1.bmin[k] += vels[i][k] * (latency + dt);
			b1.bmax[k] += vels[i][k] * (latency + dt);
		}
		d0 = TouchWep_SplashDamage(TouchWep_BoxGap(x0, b0.bmin, b0.bmax), TW_REMOTE_CORE, TW_REMOTE_EDGE, radius);
		d1 = TouchWep_SplashDamage(TouchWep_BoxGap(x1, b1.bmin, b1.bmax), TW_REMOTE_CORE, TW_REMOTE_EDGE, radius);
		if (bodies[i].weight > 0)
		{
			// a direct hit within the next frame beats any remote blast
			if (TouchWep_BoxGap(x1, b1.bmin, b1.bmax) <= 0)
				return false;
			good0 += bodies[i].weight * d0;
			good1 += bodies[i].weight * d1;
		}
		else
			bad0 -= bodies[i].weight * d0;
	}
	return good0 >= 0.1f * TW_REMOTE_CORE && good0 > 1.5f * bad0 && good1 < good0 + 2;
}

// ---------------------------------------------------------------------------
// Weapon choice by expected damage
// ---------------------------------------------------------------------------

// How the target moves.
static const char *const touch_wep_class_name[] = { "standing", "steady strafe", "erratic", "airborne" };

// Expected damage per second against one target at 150 / 400 / 800 / 1500 /
// 2500 units, per motion class, from the stock balance and a thumb-with-
// assist aim model (RESEARCH-tactics.md 1.6; splash weapons aimed at the
// feet, airborne targets led with gravity). 0 where the weapon's own blast
// would reach the player. The weapon's usual fire mode (TouchWep_FireMode).
typedef struct touch_wep_dps_s { const char *netname; float v[4][5]; } touch_wep_dps_t;
static const touch_wep_dps_t touch_wep_dps[] =
{
	//                standing                      steady strafe                 erratic                      airborne
	{ "blaster",      { { 29, 29, 28, 22, 14 },     { 29, 28, 23, 14, 8 },        { 25, 19, 10, 4, 1 },        { 29, 28, 20, 10, 4 } } },
	{ "shotgun",      { { 61, 30, 10, 3, 1 },       { 60, 29, 10, 3, 1 },         { 58, 25, 8, 2, 1 },         { 60, 28, 9, 3, 1 } } },
	{ "machinegun",   { { 96, 86, 51, 20, 8 },      { 96, 80, 44, 16, 6 },        { 95, 61, 25, 8, 3 },        { 96, 77, 40, 15, 6 } } },
	{ "arc",          { { 100, 100, 94, 66, 77 },   { 100, 98, 74, 37, 35 },      { 100, 73, 34, 12, 0 },      { 100, 94, 64, 28, 21 } } },
	{ "mortar",       { { 0, 68, 66, 60, 50 },      { 0, 67, 61, 49, 31 },        { 0, 46, 16, 0, 0 },         { 0, 66, 47, 22, 10 } } },
	{ "electro",      { { 0, 66, 64, 57, 45 },      { 0, 65, 59, 44, 27 },        { 0, 48, 21, 1, 0 },         { 0, 64, 47, 22, 9 } } },
	{ "crylink",      { { 86, 68, 55, 48, 37 },     { 85, 62, 50, 37, 23 },       { 66, 43, 22, 3, 0 },        { 85, 62, 40, 18, 8 } } },
	{ "vortex",       { { 53, 53, 50, 36, 20 },     { 53, 52, 39, 20, 9 },        { 53, 39, 18, 6, 2 },        { 53, 50, 34, 15, 6 } } },
	{ "hagar",        { { 150, 150, 146, 115, 76 }, { 150, 142, 104, 62, 35 },    { 118, 67, 18, 0, 0 },       { 150, 144, 105, 50, 21 } } },
	{ "devastator",   { { 0, 72, 70, 64, 52 },      { 0, 70, 64, 48, 29 },        { 0, 31, 0, 0, 0 },          { 0, 68, 47, 22, 9 } } },
	{ "vaporizer",    { { 150, 150, 141, 101, 57 }, { 150, 147, 111, 56, 24 },    { 150, 111, 50, 18, 6 },     { 150, 142, 95, 42, 18 } } },
	{ "hlac",         { { 128, 127, 106, 65, 35 },  { 128, 122, 87, 48, 23 },     { 113, 81, 40, 14, 4 },      { 128, 120, 78, 33, 14 } } },
	{ "rifle",        { { 67, 67, 63, 44, 25 },     { 67, 65, 49, 25, 11 },       { 67, 49, 23, 8, 3 },        { 67, 63, 42, 19, 8 } } },
	{ "okhmg",        { { 600, 501, 269, 98, 40 },  { 599, 471, 233, 82, 34 },    { 590, 363, 146, 49, 17 },   { 599, 449, 213, 72, 28 } } },
	{ "okmachinegun", { { 250, 224, 135, 54, 21 },  { 250, 210, 113, 42, 16 },    { 248, 160, 65, 22, 8 },     { 250, 200, 105, 40, 15 } } },
	{ "oknex",        { { 100, 100, 94, 67, 37 },   { 100, 98, 74, 37, 16 },      { 100, 74, 34, 12, 4 },      { 100, 94, 63, 29, 12 } } },
	{ "okrpc",        { { 0, 150, 147, 141, 132 },  { 0, 148, 142, 131, 115 },    { 0, 136, 116, 70, 26 },     { 0, 145, 106, 51, 22 } } },
	{ "okshotgun",    { { 226, 174, 83, 30, 11 },   { 226, 164, 74, 25, 10 },     { 222, 128, 48, 15, 6 },     { 226, 158, 69, 23, 9 } } },
};
static const float touch_wep_dps_dist[5] = { 150, 400, 800, 1500, 2500 };

// Expected DPS of a weapon at a distance against a target of that motion
// class, interpolated; -1 when the weapon is not in the table.
static float TouchWep_ExpectedDPS(const char *netname, float dist, int mclass)
{
	int i, k;
	if (!netname || mclass < 0 || mclass > 3)
		return -1;
	for (i = 0; i < (int)(sizeof(touch_wep_dps) / sizeof(touch_wep_dps[0])); i++)
	{
		const float *v;
		if (strcmp(touch_wep_dps[i].netname, netname))
			continue;
		v = touch_wep_dps[i].v[mclass];
		if (dist <= touch_wep_dps_dist[0])
			return v[0];
		for (k = 1; k < 5; k++)
			if (dist <= touch_wep_dps_dist[k])
			{
				float f = (dist - touch_wep_dps_dist[k - 1]) / (touch_wep_dps_dist[k] - touch_wep_dps_dist[k - 1]);
				return v[k - 1] + f * (v[k] - v[k - 1]);
			}
		return v[4];
	}
	return -1;
}

// ---------------------------------------------------------------------------
// Ammo: how long the weapon in hand lasts, and what to do about it
// ---------------------------------------------------------------------------

// Seconds between shots of each fire (stock balance), for the time until a
// weapon runs dry. 0: unknown (treated as 1 s).
typedef struct touch_wep_refire_s { const char *netname; float pri, sec; } touch_wep_refire_t;
static const touch_wep_refire_t touch_wep_refire[] =
{
	{ "blaster", 0.7f, 0 }, { "shotgun", 0.75f, 1.25f }, { "machinegun", 0.0625f, 0.625f }, { "arc", 0.1667f, 0.167f },
	{ "mortar", 0.8f, 0.7f }, { "minelayer", 1.0f, 0 }, { "electro", 0.6f, 1.2f }, { "crylink", 0.7f, 0.7f },
	{ "hlac", 0.078f, 1.0f }, { "vortex", 1.5f, 0 }, { "rifle", 1.2f, 0.9f }, { "vaporizer", 1.0f, 0.7f },
	{ "hagar", 0.167f, 0.5f }, { "seeker", 0.75f, 0.1f }, { "devastator", 1.1f, 0 }, { "fireball", 2.0f, 1.5f },
	{ "okhmg", 0.05f, 0.7f }, { "okmachinegun", 0.1f, 0.7f }, { "oknex", 1.0f, 0.7f }, { "okrpc", 1.0f, 0.7f }, { "okshotgun", 0.75f, 0.7f },
};

static float TouchWep_Refire(const char *netname, qbool secondary)
{
	int i;
	if (!netname)
		return 1;
	for (i = 0; i < (int)(sizeof(touch_wep_refire) / sizeof(touch_wep_refire[0])); i++)
		if (!strcmp(touch_wep_refire[i].netname, netname))
		{
			float r = secondary ? touch_wep_refire[i].sec : touch_wep_refire[i].pri;
			return r > 0 ? r : 1;
		}
	return 1;
}

// Shots the weapon can still pay for: what is loaded (clip >= 0 on a
// weapon that reloads) plus the reserve, at cost per shot. ammo < 0 or
// cost <= 0: unlimited (a large number).
#define TW_SHOTS_UNLIMITED 9999
static int TouchWep_ShotsLeft(int ammo, int clip, int cost)
{
	int total;
	if (ammo < 0 || cost <= 0)
		return TW_SHOTS_UNLIMITED;
	total = ammo + (clip > 0 ? clip : 0);
	return total / cost;
}

// Seconds until the weapon in hand is dry, firing at its refire.
static float TouchWep_TimeToDry(int shots, float refire)
{
	if (shots >= TW_SHOTS_UNLIMITED)
		return 1e6f;
	return shots * refire;
}

// What to do as the weapon in hand runs low, over the horizon (TW_HORIZON):
// keep firing it until dry and switch then (losing the switch time after),
// or switch now. held_dps / alt_dps: expected damage per second of the
// weapon in hand and of the best other weapon with ammo (0: none).
#define TW_DRY_KEEP   0
#define TW_DRY_SWITCH 1 // switch now: the other weapon does more over the horizon, or the hand is empty
#define TW_DRY_EVADE  2 // nothing worth having left: keep away and dodge rather than trade
static int TouchWep_DryPlan(float held_dps, int shots, float refire, float alt_dps, qbool alt_is_fallback, float horizon, float switch_cost)
{
	float t_dry = TouchWep_TimeToDry(shots, refire), keep, now;
	if (shots <= 0)
		return alt_dps > 0 && !alt_is_fallback ? TW_DRY_SWITCH : TW_DRY_EVADE;
	if (alt_dps <= 0 || t_dry > horizon)
		return TW_DRY_KEEP; // not running low: the ordinary weapon choice decides
	keep = held_dps * (t_dry < horizon ? t_dry : horizon);
	if (t_dry + switch_cost < horizon)
		keep += alt_dps * (horizon - t_dry - switch_cost);
	now = horizon > switch_cost ? alt_dps * (horizon - switch_cost) : 0;
	return now > keep * 1.1f ? TW_DRY_SWITCH : TW_DRY_KEEP;
}

// Share (0..1) of the weapon's best expected DPS against this motion class
// that it has at this distance; -1 when the weapon is not in the table.
static float TouchWep_DPSShare(const char *netname, float dist, int mclass)
{
	float top = 0, d;
	int k;
	for (k = 0; k < 5; k++)
	{
		float v = TouchWep_ExpectedDPS(netname, touch_wep_dps_dist[k], mclass);
		if (v < 0)
			return -1;
		if (v > top)
			top = v;
	}
	d = TouchWep_ExpectedDPS(netname, dist, mclass);
	return top > 0 ? d / top : 0;
}

// ---------------------------------------------------------------------------
// Several enemies: which one first
// ---------------------------------------------------------------------------

// What the choice between several visible enemies looks at. The angle from
// the crosshair decides; these shift it by at most TW_PRIO_CAP degrees.
typedef struct touch_wep_prio_s
{
	float dist;        // eye to the enemy
	float dps_share;   // TouchWep_DPSShare of the weapon in hand, -1 unknown
	qbool facing;      // looking toward the player
	float range_rate;  // units/s away from the player (< 0 closing in)
	qbool instant;     // the weapon in hand hits at once (hitscan, beam)
	float dealt;       // damage done to it in the last few seconds
	int group;         // other enemies near enough to share a blast (splash weapon)
} touch_wep_prio_t;

#define TW_PRIO_CAP        2.5f   // degrees, either way
#define TW_PRIO_THREAT_DIST 1200.0f
#define TW_PRIO_REACH      1000.0f // beyond this a fleeing enemy is out of a projectile's reach
#define TW_PRIO_SPEED      150.0f  // units/s that count as closing in or running away
#define TW_PRIO_HURT       20.0f   // damage that marks an enemy as hurt

// Degrees by which an enemy counts as nearer the crosshair than it is:
// more for one the weapon does more damage to (+-1), one facing the player
// up close (+1) or rushing in (+0.5), one already hurt (+1), a group for a
// splash weapon (+0.5 each other enemy, up to 3); less for one running out
// of a projectile's reach (-1.5).
static float TouchWep_PriorityBonus(const touch_wep_prio_t *p)
{
	float b = 0;
	if (p->dps_share >= 0)
		b += 2 * (p->dps_share - 0.5f);
	if (p->facing && p->dist < TW_PRIO_THREAT_DIST)
		b += 1;
	if (p->range_rate < -TW_PRIO_SPEED)
		b += 0.5f;
	if (p->dealt >= TW_PRIO_HURT)
		b += 1;
	if (p->group > 0)
		b += 0.5f * (p->group < 3 ? p->group : 3);
	if (!p->instant && p->range_rate > TW_PRIO_SPEED && p->dist > TW_PRIO_REACH)
		b -= 1.5f;
	return b > TW_PRIO_CAP ? TW_PRIO_CAP : b < -TW_PRIO_CAP ? -TW_PRIO_CAP : b;
}

// Seconds a switch costs before the new weapon fires: the old weapon's
// drop and the new one's raise (0.2 each in the stock balance).
#define TW_SWITCH_COST 0.4f
// Horizon over which weapons are compared, seconds.
#define TW_HORIZON     1.5f
// A switch must win by this factor plus TW_SWITCH_EXTRA damage.
#define TW_SWITCH_GAIN  1.25f
#define TW_SWITCH_EXTRA 5.0f

// Expected damage of a weapon over the horizon: its DPS for the situation
// over the time left after t_ready (0 for the weapon in hand, the switch
// cost otherwise); splash weapons count every enemy within reach of the
// blast (group: enemies besides the target near enough to share it).
static float TouchWep_Utility(const char *netname, float dist, int mclass, int group, float t_ready)
{
	const touch_wep_t *w = TouchWep_Find(netname);
	float dps = TouchWep_ExpectedDPS(netname, dist, mclass), left = TW_HORIZON - t_ready;
	if (dps <= 0 || left <= 0)
		return 0;
	if (w && w->pri.splash >= 100 && group > 0)
		dps *= 1 + 0.8f * group;
	return dps * left;
}

// The best owned weapon by expected damage: index into names[] or -1, and
// its utility and the held one's. held: index of the weapon in hand in
// names[] (-1 none). ready[i]: has ammo for a shot.
static int TouchWep_ChooseSmart(const char *const *names, const qbool *ready, int n, int held, float dist, int mclass, int group, qbool fallback_last, float *best_u, float *held_u)
{
	int i, best = -1, pass;
	float bu = 0;
	*held_u = 0;
	if (held >= 0 && held < n && ready[held])
	{
		const touch_wep_t *w = TouchWep_Find(names[held]);
		if (w && TouchWep_Usable(w, dist))
			*held_u = TouchWep_Utility(names[held], dist, mclass, group, 0);
	}
	// fallback_last: a fallback (the Blaster) only when no real weapon can shoot here
	for (pass = fallback_last ? 0 : 1; pass < 2 && best < 0; pass++)
		for (i = 0; i < n; i++)
		{
			const touch_wep_t *w = TouchWep_Find(names[i]);
			float u;
			if (!ready[i] || !w || (w->flags & TW_NO_SWITCH_TO) || !TouchWep_Usable(w, dist))
				continue;
			if (!pass && (w->flags & TW_FALLBACK))
				continue;
			u = TouchWep_Utility(names[i], dist, mclass, group, i == held ? 0 : TW_SWITCH_COST);
			if (pass && fallback_last && u <= 0)
				u = 0.01f; // nothing else at all: still a weapon that fires
			if (u > bu)
			{
				bu = u;
				best = i;
			}
		}
	*best_u = bu;
	return best;
}

// Whether the best weapon's utility is worth leaving the held one for.
static qbool TouchWep_ShouldSwitchSmart(float held_u, qbool held_can_shoot, float best_u)
{
	if (best_u <= 0)
		return false;
	if (!held_can_shoot)
		return true;
	return best_u >= TW_SWITCH_GAIN * held_u + TW_SWITCH_EXTRA;
}

// ---------------------------------------------------------------------------
// Offline practice: what only a match against bots on the player's own
// machine may use
// ---------------------------------------------------------------------------

// Where the game runs. Filled by the engine every frame.
typedef struct touch_practice_env_s
{
	qbool server_active;   // this process runs the server (sv.active)
	qbool local_connection; // and this client is connected to it over the loopback
	qbool demo;            // a demo is playing
	int other_humans;      // clients of that server besides this one that are not bots
} touch_practice_env_t;

#define TW_GATE_OPEN          0
#define TW_GATE_NO_SERVER     1
#define TW_GATE_REMOTE        2
#define TW_GATE_DEMO          3
#define TW_GATE_HUMANS        4
static const char *const touch_practice_why[] =
{
	"offline match", "not running the server here", "connected to another machine", "demo playback", "another player is connected",
};

// The one check every offline-only helper goes through: this client plays
// on its own local server in the same process, over the loopback, with no
// other human connected, and no demo plays. Anything else: closed.
static int TouchWep_PracticeGate(const touch_practice_env_t *e)
{
	if (e->demo)
		return TW_GATE_DEMO;
	if (!e->server_active)
		return TW_GATE_NO_SERVER;
	if (!e->local_connection)
		return TW_GATE_REMOTE;
	if (e->other_humans > 0)
		return TW_GATE_HUMANS;
	return TW_GATE_OPEN;
}

// The offline helpers in effect: the player's preferences, each only when
// the gate is open. With it closed every one is off, whatever was asked.
typedef struct touch_practice_s
{
	qbool seethrough;      // enemies behind walls: markers, pre-aim, splash and rockets
	qbool full360;         // enemies all around may be the target
	qbool instant;         // the pull moves the whole way at once (no rate cap)
} touch_practice_t;

static touch_practice_t TouchWep_PracticeEffective(int gate, qbool want_seethrough, qbool want_360, qbool want_instant)
{
	touch_practice_t p;
	qbool open = gate == TW_GATE_OPEN;
	p.seethrough = open && want_seethrough;
	p.full360 = open && want_360;
	p.instant = open && want_instant;
	return p;
}

#endif
