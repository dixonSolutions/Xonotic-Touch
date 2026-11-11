/*
Checks when the engine HUD shows ALT (secondary fire) on a server that does
not send STAT_TOUCH_ALTFIRE (engine/darkplaces/touch_altfire.h): which stock
weapons have a secondary, when one runs dry, and the fade.

Run through scripts/test-touch-altfire.sh.
*/

#include <math.h>
#include <stdbool.h>
#include <stdio.h>

typedef bool qbool;

#include "touch_altfire.h"

static int failures;

#define CHECK_EQ(expected, actual, what) do { \
	int e_ = (expected), a_ = (actual); \
	if (e_ != a_) { \
		printf("FAIL %s: expected %d, got %d\n", (what), e_, a_); \
		failures++; \
	} \
} while (0)

static const char *state_name(int s)
{
	return s == TOUCH_ALTFIRE_READY ? "ready" : s == TOUCH_ALTFIRE_EMPTY ? "empty" : "none";
}

typedef struct fallback_case_s
{
	const char *what;
	const char *netname;
	int reserve, clip;
	qbool infinite;
	int expected;
}
fallback_case_t;

static const fallback_case_t fallback_cases[] =
{
	// No secondary fire at all: never shown, whatever the ammo.
	{ "blaster: +fire2 switches weapon",  "blaster",     0,  -1, true,  TOUCH_ALTFIRE_NONE },
	{ "vortex: +fire2 zooms",             "vortex",     50,  -1, false, TOUCH_ALTFIRE_NONE },
	{ "unknown mod weapon",               "railgun",    50,  -1, false, TOUCH_ALTFIRE_NONE },
	{ "no weapon in hand",                "",           50,  -1, false, TOUCH_ALTFIRE_NONE },
	{ "netname not known",                NULL,         50,  -1, false, TOUCH_ALTFIRE_NONE },

	// Secondaries that cost ammo: ready down to one shot's worth, then empty.
	{ "mortar, plenty",                   "mortar",     20,  -1, false, TOUCH_ALTFIRE_READY },
	{ "mortar, exactly one bounce",       "mortar",      2,  -1, false, TOUCH_ALTFIRE_READY },
	{ "mortar, one rocket short",         "mortar",      1,  -1, false, TOUCH_ALTFIRE_EMPTY },
	{ "mortar, dry",                      "mortar",      0,  -1, false, TOUCH_ALTFIRE_EMPTY },
	{ "crylink needs 3 cells",            "crylink",     2,  -1, false, TOUCH_ALTFIRE_EMPTY },
	{ "electro keeps a primary in hand",  "electro",     5,  -1, false, TOUCH_ALTFIRE_EMPTY },
	{ "electro orb and primary",          "electro",     6,  -1, false, TOUCH_ALTFIRE_READY },
	{ "rifle burst needs 10",             "rifle",       9,  -1, false, TOUCH_ALTFIRE_EMPTY },
	{ "hlac needs 6",                     "hlac",        6,  -1, false, TOUCH_ALTFIRE_READY },
	{ "machinegun burst, last round",     "machinegun",  1,  -1, false, TOUCH_ALTFIRE_READY },
	{ "machinegun, dry",                  "machinegun",  0,  -1, false, TOUCH_ALTFIRE_EMPTY },
	{ "arc bolt, dry",                    "arc",         0,  -1, false, TOUCH_ALTFIRE_EMPTY },
	{ "seeker flac",                      "seeker",      1,  -1, false, TOUCH_ALTFIRE_READY },
	{ "hagar, dry",                       "hagar",       0,  -1, false, TOUCH_ALTFIRE_EMPTY },

	// A reloading weapon fires from the clip while the reserve is empty, and
	// reloads from the reserve while the clip is: either one is enough.
	{ "clip holds a shot, reserve dry",   "mortar",      0,   4, false, TOUCH_ALTFIRE_READY },
	{ "clip empty, reserve full",         "mortar",     10,   0, false, TOUCH_ALTFIRE_READY },
	{ "clip and reserve both short",      "mortar",      1,   1, false, TOUCH_ALTFIRE_EMPTY },

	// Unlimited ammo, or a weapon that uses none, never runs dry.
	{ "unlimited ammo",                   "mortar",      0,  -1, true,  TOUCH_ALTFIRE_READY },
	{ "shotgun melee costs nothing",      "shotgun",     0,  -1, false, TOUCH_ALTFIRE_READY },
	{ "vaporizer laser",                  "vaporizer",   0,  -1, false, TOUCH_ALTFIRE_READY },
	{ "hook bomb",                        "hook",        0,  -1, false, TOUCH_ALTFIRE_READY },
	{ "tuba",                             "tuba",        0,  -1, false, TOUCH_ALTFIRE_READY },
	{ "fireball",                         "fireball",    0,  -1, false, TOUCH_ALTFIRE_READY },
	{ "porto",                            "porto",       0,  -1, false, TOUCH_ALTFIRE_READY },
	{ "overkill blaster shot",            "okshotgun",   0,  -1, false, TOUCH_ALTFIRE_READY },

	// Detonators: only the server knows what is out, so always shown here.
	{ "devastator detonate",              "devastator",  0,  -1, false, TOUCH_ALTFIRE_READY },
	{ "mine layer detonate",              "minelayer",   0,  -1, false, TOUCH_ALTFIRE_READY },
};

static void test_fallback(void)
{
	size_t i;
	for (i = 0; i < sizeof(fallback_cases) / sizeof(fallback_cases[0]); i++)
	{
		const fallback_case_t *c = &fallback_cases[i];
		int got = TouchAltFire_Fallback(c->netname, c->reserve, c->clip, c->infinite);
		if (got != c->expected)
		{
			printf("FAIL fallback %s: expected %s, got %s\n", c->what, state_name(c->expected), state_name(got));
			failures++;
		}
	}
}

static void test_available(void)
{
	// 0: never, even with a secondary ready.
	CHECK_EQ(false, TouchAltFire_Available(0, TOUCH_ALTFIRE_READY), "mode 0, ready");
	// 1: always, the old behaviour, even with no secondary.
	CHECK_EQ(true, TouchAltFire_Available(1, TOUCH_ALTFIRE_NONE), "mode 1, none");
	CHECK_EQ(true, TouchAltFire_Available(1, TOUCH_ALTFIRE_EMPTY), "mode 1, empty");
	// 2: only while it would fire.
	CHECK_EQ(true, TouchAltFire_Available(2, TOUCH_ALTFIRE_READY), "mode 2, ready");
	CHECK_EQ(false, TouchAltFire_Available(2, TOUCH_ALTFIRE_EMPTY), "mode 2, empty");
	CHECK_EQ(false, TouchAltFire_Available(2, TOUCH_ALTFIRE_NONE), "mode 2, none");
}

static void test_fade(void)
{
	float fade = 0;
	int frames;

	// Fading in reaches full within TOUCH_ALTFIRE_FADE_IN at 60 fps.
	for (frames = 0; fade < 1 && frames < 1000; frames++)
		fade = TouchAltFire_Fade(fade, true, 1.0f / 60);
	CHECK_EQ(1, frames <= (int)ceilf(TOUCH_ALTFIRE_FADE_IN * 60) + 1, "fade in takes about FADE_IN");
	CHECK_EQ(1, fade == 1.0f, "fade in ends at 1");

	// Running dry fades it out slower than it came: visible part-way...
	fade = TouchAltFire_Fade(1, false, TOUCH_ALTFIRE_FADE_OUT * 0.5f);
	CHECK_EQ(1, fade > 0.45f && fade < 0.55f, "half way out at half FADE_OUT");
	// ...and gone, never below zero, after FADE_OUT.
	fade = TouchAltFire_Fade(fade, false, TOUCH_ALTFIRE_FADE_OUT);
	CHECK_EQ(1, fade == 0.0f, "fade out ends at 0");
	// A stalled frame (negative dt from a clock step) changes nothing.
	CHECK_EQ(1, TouchAltFire_Fade(0.5f, true, -1) == 0.5f, "negative dt ignored");
	// A fade reversed mid-way picks up where it is, without a jump.
	fade = TouchAltFire_Fade(0.3f, true, 0);
	CHECK_EQ(1, fade == 0.3f, "reversal keeps its place");
}

static void test_zoom(void)
{
	// The scoped weapons, the ones with a reticle in the stock game.
	CHECK_EQ(true, TouchZoom_Scoped("vortex"), "vortex has a scope");
	CHECK_EQ(true, TouchZoom_Scoped("rifle"), "rifle has a scope");
	CHECK_EQ(true, TouchZoom_Scoped("vaporizer"), "vaporizer has a scope");
	CHECK_EQ(true, TouchZoom_Scoped("machinegun"), "machinegun has a scope");
	CHECK_EQ(false, TouchZoom_Scoped("devastator"), "devastator has none");
	CHECK_EQ(false, TouchZoom_Scoped("blaster"), "blaster has none");
	CHECK_EQ(false, TouchZoom_Scoped(""), "no weapon in hand");
	CHECK_EQ(false, TouchZoom_Scoped(NULL), "netname not known");
	CHECK_EQ(false, TouchZoom_Scoped("vortexx"), "exact names only");

	CHECK_EQ(false, TouchZoom_Available(0, true), "zoom mode 0, scoped");
	CHECK_EQ(true, TouchZoom_Available(1, false), "zoom mode 1, unscoped");
	CHECK_EQ(true, TouchZoom_Available(2, true), "zoom mode 2, scoped");
	CHECK_EQ(false, TouchZoom_Available(2, false), "zoom mode 2, unscoped");

	// Stock Vortex: +fire2 zooms, so ALT stays hidden and ZOOM is the one shown.
	CHECK_EQ(false, TouchAltFire_Available(2, TouchAltFire_Fallback("vortex", 50, -1, false)), "vortex hides ALT");
	CHECK_EQ(true, TouchZoom_Available(2, TouchZoom_Scoped("vortex")), "vortex shows ZOOM");
}

int main(void)
{
	test_fallback();
	test_available();
	test_fade();
	test_zoom();
	if (failures)
	{
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("touch altfire: %d fallback cases, availability, fade and zoom OK\n",
		(int)(sizeof(fallback_cases) / sizeof(fallback_cases[0])));
	return 0;
}
