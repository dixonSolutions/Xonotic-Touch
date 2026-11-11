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
When the touch HUD's ALT button (secondary fire, +fire2) is there.

touch_fire2_visible 2, the default, shows ALT only while the weapon in hand
has a secondary fire that can fire, and fades it out otherwise. A server
running this port's game code says which in STAT_TOUCH_ALTFIRE
(qcsrc/server/weapons/altfire.qc), computed from the real balance. Any other
server says nothing, so the engine HUD falls back to the stock Xonotic
balance (bal-wep-xonotic.cfg) and the ammo stats it already reads: which
weapons have a secondary at all, and what one shot of it costs. A server
that runs another balance can disagree, and the Devastator's and Mine
Layer's secondaries (detonating what is already out) are always shown,
since only the server knows what is out.

Only touch_hud.c includes this in the engine. It is a header of plain C (the
includer supplies qbool, true and false) so that
scripts/test-touch-altfire.sh can test it without the rest of the engine.
*/

#ifndef TOUCH_ALTFIRE_H
#define TOUCH_ALTFIRE_H

#include <string.h>

// Same values as TOUCH_ALTFIRE_* in qcsrc/common/stats.qh.
#define TOUCH_ALTFIRE_NONE  0
#define TOUCH_ALTFIRE_EMPTY 1
#define TOUCH_ALTFIRE_READY 2

// Seconds for a full fade, matching touch_input.qc.
#define TOUCH_ALTFIRE_FADE_IN  0.12f
#define TOUCH_ALTFIRE_FADE_OUT 0.35f

// Stock secondaries by weapon netname: the ammo one shot needs from the clip
// or the reserve (0 = none), as wr_checkammo2 counts it under the stock balance.
typedef struct touch_altfire_def_s { const char *netname; int cost; } touch_altfire_def_t;
static const touch_altfire_def_t touch_altfire_defs[] =
{
	{ "arc",          1 }, // bolt
	{ "crylink",      3 },
	{ "devastator",   0 }, // detonate rockets in flight
	{ "electro",      6 }, // an orb and the primary shot after it (combo_safeammocheck)
	{ "fireball",     0 },
	{ "hagar",        1 },
	{ "hlac",         6 },
	{ "hook",         0 },
	{ "machinegun",   1 }, // burst: 3 rounds over 3 shots
	{ "minelayer",    0 }, // detonate placed mines
	{ "mortar",       2 },
	{ "porto",        0 },
	{ "rifle",       10 },
	{ "seeker",       1 }, // flac
	{ "shotgun",      0 }, // melee
	{ "tuba",         0 },
	{ "vaporizer",    0 }, // blaster shot
	// Overkill: every secondary is a blaster shot.
	{ "okhmg",        0 },
	{ "okmachinegun", 0 },
	{ "oknex",        0 },
	{ "okrpc",        0 },
	{ "okshotgun",    0 },
	// Not listed, so no secondary fire: blaster (+fire2 switches back to the
	// last weapon) and vortex (+fire2 zooms).
};

// What +fire2 would do for the held weapon, judged without the server's stat.
// netname: the held weapon's, NULL or "" when unknown. reserve: its ammo
// reserve; clip: the rounds loaded, or -1 for a weapon without a clip;
// infinite: unlimited ammo, or a weapon that uses none.
static int TouchAltFire_Fallback(const char *netname, int reserve, int clip, qbool infinite)
{
	size_t i;
	if (!netname || !netname[0])
		return TOUCH_ALTFIRE_NONE;
	for (i = 0; i < sizeof(touch_altfire_defs) / sizeof(touch_altfire_defs[0]); i++)
	{
		const touch_altfire_def_t *d = &touch_altfire_defs[i];
		if (strcmp(d->netname, netname))
			continue;
		if (infinite || d->cost <= 0 || reserve >= d->cost || clip >= d->cost)
			return TOUCH_ALTFIRE_READY;
		return TOUCH_ALTFIRE_EMPTY;
	}
	return TOUCH_ALTFIRE_NONE;
}

// Whether ALT takes touches. mode: touch_fire2_visible (0 never, 1 always,
// 2 when the state is READY).
static qbool TouchAltFire_Available(int mode, int state)
{
	if (mode <= 0)
		return false;
	if (mode == 1)
		return true;
	return state == TOUCH_ALTFIRE_READY;
}

// Weapons with a scope, the ones whose stock definition carries a w_reticle:
// ZOOM is contextual like ALT (touch_zoom_visible 2) and shows for these.
// The CSQC asks the weapon itself (Touch_ZoomAvailable in touch_input.qc);
// on another server the engine HUD asks this list.
static const char *const touch_zoom_scoped[] = { "machinegun", "rifle", "vaporizer", "vortex" };

static qbool TouchZoom_Scoped(const char *netname)
{
	size_t i;
	if (!netname || !netname[0])
		return false;
	for (i = 0; i < sizeof(touch_zoom_scoped) / sizeof(touch_zoom_scoped[0]); i++)
		if (!strcmp(touch_zoom_scoped[i], netname))
			return true;
	return false;
}

// Whether ZOOM takes touches. mode: touch_zoom_visible (0 never, 1 always,
// 2 while the weapon in hand has a scope).
static qbool TouchZoom_Available(int mode, qbool scoped)
{
	if (mode <= 0)
		return false;
	return mode == 1 || scoped;
}

// One step of the draw fade, 0..1, towards shown or hidden.
static float TouchAltFire_Fade(float fade, qbool shown, float dt)
{
	if (dt < 0)
		dt = 0;
	if (shown)
		fade += dt / TOUCH_ALTFIRE_FADE_IN;
	else
		fade -= dt / TOUCH_ALTFIRE_FADE_OUT;
	return fade < 0 ? 0 : fade > 1 ? 1 : fade;
}

#endif
