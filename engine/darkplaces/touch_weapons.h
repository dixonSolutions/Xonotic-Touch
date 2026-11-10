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
	{ "blaster",      TW_WEAK_SPLASH,                    { TW_KIND_PROJ,      6000,    0,    0,    0,   60,  0,     TW_FAR,  20 }, TW_SEC_SWITCH,   { 0 }, 0 },
	{ "shotgun",      0,                                 { TW_KIND_HITSCAN,      0,    0,    0,    0,    0,  0.12f, 1200,    48 }, TW_SEC_MELEE,    { TW_KIND_MELEE, 0, 0, 0, 0, 0, 0, 120, 70 }, 1 },
	{ "machinegun",   0,                                 { TW_KIND_HITSCAN,      0,    0,    0,    0,    0,  0.03f, TW_FAR,   6 }, TW_SEC_SHOT,     { TW_KIND_HITSCAN, 0, 0, 0, 0, 0, 0, TW_FAR, 30 }, 1 },
	{ "arc",          0,                                 { TW_KIND_BEAM,         0,    0,    0,    0,    0,  0,     1500,    25 }, TW_SEC_SHOT,     { TW_KIND_PROJ, 2300, 0, 0, 0, 65, 0, TW_FAR, 25 }, 1 },
	{ "mortar",       0,                                 { TW_KIND_BALLISTIC, 1900,  225,    0,    0,  120,  0,     2500,    55 }, TW_SEC_SHOT,     { TW_KIND_BALLISTIC, 1400, 150, 0, 0, 120, 0, 1500, 55 }, 2 },
	{ "minelayer",    TW_MINES | TW_NO_SWITCH_TO,        { TW_KIND_BALLISTIC,  800,    0,    0,    0,  175,  0,     1000,    40 }, TW_SEC_DETONATE, { 0 }, 4 },
	{ "electro",      0,                                 { TW_KIND_PROJ,      2500,    0,    0,    0,  100,  0,     TW_FAR,  40 }, TW_SEC_SHOT,     { TW_KIND_BALLISTIC, 1000, 200, 0, 0, 150, 0, 1200, 30 }, 4 },
	{ "crylink",      0,                                 { TW_KIND_PROJ,      2000,    0,    0,    0,   80,  0.08f, 2000,    60 }, TW_SEC_SHOT,     { TW_KIND_PROJ, 3000, 0, 0, 0, 100, 0.01f, 2500, 40 }, 3 },
	{ "hlac",         0,                                 { TW_KIND_PROJ,      6000,    0,    0,    0,   60,  0.03f, TW_FAR,  10 }, TW_SEC_SHOT,     { TW_KIND_PROJ, 6000, 0, 0, 0, 60, 0.15f, 1200, 90 }, 1 },
	{ "vortex",       0,                                 { TW_KIND_HITSCAN,      0,    0,    0,    0,    0,  0,     TW_FAR,  80 }, TW_SEC_ZOOM,     { 0 }, 6 },
	{ "rifle",        0,                                 { TW_KIND_HITSCAN,      0,    0,    0,    0,    0,  0,     TW_FAR,  80 }, TW_SEC_SHOT,     { TW_KIND_HITSCAN, 0, 0, 0, 0, 0, 0.04f, 2000, 80 }, 10 },
	{ "vaporizer",    0,                                 { TW_KIND_HITSCAN,      0,    0,    0,    0,    0,  0,     TW_FAR, 150 }, TW_SEC_SHOT,     { TW_KIND_PROJ, 6000, 0, 0, 0, 60, 0, TW_FAR, 20 }, 10 },
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
	{ "oknex",        0,                                 { TW_KIND_HITSCAN,      0,    0,    0,    0,    0,  0,     TW_FAR, 100 }, TW_SEC_SHOT,     { TW_KIND_PROJ, 6000, 0, 0, 0, 60, 0, TW_FAR, 20 }, 10 },
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
	"out of range", "secondary off: primary", "no ammo",
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
static int TouchWep_Choose(const char *const *names, const qbool *ready, int n, float dist, int *rank_out)
{
	int i, best = -1, best_rank = 1 << 30;
	for (i = 0; i < n; i++)
	{
		const touch_wep_t *w = TouchWep_Find(names[i]);
		int r;
		if (!ready[i] || !w || (w->flags & TW_NO_SWITCH_TO) || !TouchWep_Usable(w, dist))
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

#endif
