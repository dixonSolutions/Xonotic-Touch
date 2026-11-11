/*
Checks the decisions touch auto-shoot makes from the weapon in hand
(engine/darkplaces/touch_weapons.h): lead and lob maths against hand-computed
cases and a simulated flight, the fire window, primary or secondary per
weapon, the weapon chosen per distance band, Devastator detonation geometry,
and that every row of the weapon table is sane.

Run through scripts/test-touch-aim.sh.
*/

#include <math.h>
#include <stdbool.h>
#include <stdio.h>

typedef bool qbool;

#include "touch_altfire.h"
#include "touch_weapons.h"

static int failures, checks;

#define CHECK(cond, what) do { \
	checks++; \
	if (!(cond)) { \
		printf("FAIL %s\n", (what)); \
		failures++; \
	} \
} while (0)

#define CHECK_EQ(expected, actual, what) do { \
	int e_ = (expected), a_ = (actual); \
	checks++; \
	if (e_ != a_) { \
		printf("FAIL %s: expected %d, got %d\n", (what), e_, a_); \
		failures++; \
	} \
} while (0)

#define CHECK_NEAR(expected, actual, tol, what) do { \
	double e_ = (expected), a_ = (actual); \
	checks++; \
	if (fabs(e_ - a_) > (tol)) { \
		printf("FAIL %s: expected %.4f, got %.4f\n", (what), e_, a_); \
		failures++; \
	} \
} while (0)

static void v3(float *v, float x, float y, float z)
{
	v[0] = x;
	v[1] = y;
	v[2] = z;
}

static void test_intercept(void)
{
	float rel[3], vel[3], t = -1;
	qbool ok;

	v3(rel, 1000, 0, 0);
	v3(vel, 0, 0, 0);
	ok = TouchWep_InterceptTime(rel, vel, 2000, TW_LEAD_MAX_TIME, &t);
	CHECK(ok, "stationary: solvable");
	CHECK_NEAR(0.5, t, 1e-4, "stationary: 1000 units at 2000/s");

	// crossing at 300/s: t = 1000 / sqrt(2000^2 - 300^2)
	v3(vel, 0, 300, 0);
	ok = TouchWep_InterceptTime(rel, vel, 2000, TW_LEAD_MAX_TIME, &t);
	CHECK(ok, "crossing: solvable");
	CHECK_NEAR(1000.0 / sqrt(4e6 - 9e4), t, 1e-4, "crossing: 300/s across");

	// coming straight at the shooter: they meet at 800 units after 0.4 s
	v3(vel, -500, 0, 0);
	ok = TouchWep_InterceptTime(rel, vel, 2000, TW_LEAD_MAX_TIME, &t);
	CHECK(ok, "approaching: solvable");
	CHECK_NEAR(0.4, t, 1e-4, "approaching at 500/s");

	// same speed as the shot, head on: the linear case
	v3(vel, -2000, 0, 0);
	ok = TouchWep_InterceptTime(rel, vel, 2000, TW_LEAD_MAX_TIME, &t);
	CHECK(ok, "approaching at shot speed: solvable");
	CHECK_NEAR(0.25, t, 1e-4, "approaching at shot speed");

	// running away at half the shot's speed: caught after 1 s at 2000 units
	v3(vel, 1000, 0, 0);
	ok = TouchWep_InterceptTime(rel, vel, 2000, TW_LEAD_MAX_TIME, &t);
	CHECK(ok, "receding slower: solvable");
	CHECK_NEAR(1.0, t, 1e-4, "receding at 1000/s");

	// running away faster than the shot: no solution
	v3(vel, 2500, 0, 0);
	CHECK(!TouchWep_InterceptTime(rel, vel, 2000, TW_LEAD_MAX_TIME, &t), "receding faster than the shot: no lead");
	v3(vel, 2000, 0, 0);
	CHECK(!TouchWep_InterceptTime(rel, vel, 2000, TW_LEAD_MAX_TIME, &t), "receding at shot speed: no lead");

	// too far: 2.5 s of flight is beyond the cap
	v3(rel, 5000, 0, 0);
	v3(vel, 0, 0, 0);
	CHECK(!TouchWep_InterceptTime(rel, vel, 2000, TW_LEAD_MAX_TIME, &t), "flight longer than the cap: no lead");
	CHECK(!TouchWep_InterceptTime(rel, vel, 0, TW_LEAD_MAX_TIME, &t), "zero speed: no lead");
}

static void test_accel(void)
{
	const touch_wep_t *dev = TouchWep_Find("devastator");
	float rel[3], vel[3], t = -1, ta = 300.0f / 1300.0f;
	CHECK(dev != NULL, "devastator in the table");
	if (!dev)
		return;
	CHECK_NEAR(0, TouchWep_FlightDist(&dev->pri, 0), 1e-4, "devastator flies 0 at t=0");
	CHECK_NEAR(100 + 0.5 * 1300 * 0.01, TouchWep_FlightDist(&dev->pri, 0.1f), 1e-2, "devastator accelerating at 0.1 s");
	CHECK_NEAR(1000 * ta + 650 * ta * ta + 1300 * (1 - ta), TouchWep_FlightDist(&dev->pri, 1), 1e-2, "devastator at full speed by 1 s");
	{
		const touch_wep_t *hag = TouchWep_Find("hagar");
		CHECK_NEAR(2200 * 0.3, TouchWep_FlightDist(&hag->pri, 0.3f), 1e-3, "hagar: constant speed");
	}

	// a stationary target exactly 1 s of flight away
	v3(rel, TouchWep_FlightDist(&dev->pri, 1), 0, 0);
	v3(vel, 0, 0, 0);
	CHECK(TouchWep_InterceptTimeAccel(&dev->pri, rel, vel, TW_LEAD_MAX_TIME, &t), "devastator stationary: solvable");
	CHECK_NEAR(1.0, t, 1e-3, "devastator stationary target at 1 s");

	// crossing target: the solution satisfies flown distance = target distance
	v3(rel, 800, 0, 0);
	v3(vel, 0, 400, 0);
	CHECK(TouchWep_InterceptTimeAccel(&dev->pri, rel, vel, TW_LEAD_MAX_TIME, &t), "devastator crossing: solvable");
	CHECK_NEAR(sqrt(800.0 * 800 + (400.0 * t) * (400.0 * t)), TouchWep_FlightDist(&dev->pri, t), 0.5, "devastator crossing: rocket meets target");

	// faster than the rocket ever goes: no solution
	v3(vel, 1500, 0, 0);
	CHECK(!TouchWep_InterceptTimeAccel(&dev->pri, rel, vel, TW_LEAD_MAX_TIME, &t), "devastator: target outruns rocket");
}

// Height of a lob at horizontal distance dx, launched at elevation elev
// (radians) with speed along the view and up along the view's up vector.
static double lob_height(double speed, double up, double g, double elev, double dx)
{
	double vx = speed * cos(elev) - up * sin(elev);
	double vz = speed * sin(elev) + up * cos(elev);
	double t = dx / vx;
	return vz * t - 0.5 * g * t * t;
}

static void test_ballistic(void)
{
	float elev = 0, t = 0;
	qbool ok;

	// textbook: 1000/s, flat 1000 units away under 800: tan = 0.5
	ok = TouchWep_Ballistic(1000, 0, 1000, 0, 800, &elev, &t);
	CHECK(ok, "lob 1000 units: solvable");
	CHECK_NEAR(atan(0.5), elev, 1e-4, "lob 1000 units: low arc tan 0.5");
	CHECK_NEAR(1000 / (1000 * cos(atan(0.5))), t, 1e-3, "lob 1000 units: flight time");

	// Mortar with its up-speed: the lob passes through the target
	ok = TouchWep_Ballistic(1500, 100, 1900, 225, 800, &elev, &t);
	CHECK(ok, "mortar 1500 out, 100 up: solvable");
	CHECK_NEAR(100, lob_height(1900, 225, 800, elev, 1500), 0.5, "mortar lob reaches the target height");
	ok = TouchWep_Ballistic(600, -200, 1900, 225, 800, &elev, &t);
	CHECK(ok, "mortar down a ledge: solvable");
	CHECK_NEAR(-200, lob_height(1900, 225, 800, elev, 600), 0.5, "mortar lob down a ledge");
	CHECK(elev < 0, "mortar close and below: aims below the horizon (the up-speed lifts it)");

	// Electro orbs at 1000 + 200 up
	ok = TouchWep_Ballistic(800, 0, 1000, 200, 800, &elev, &t);
	CHECK(ok, "orb 800 out: solvable");
	CHECK_NEAR(0, lob_height(1000, 200, 800, elev, 800), 0.5, "orb lob lands on target");

	// out of reach: 500/s carries at most 312 units on the flat
	CHECK(!TouchWep_Ballistic(1000, 0, 500, 0, 800, &elev, &t), "lob out of reach");
	CHECK(!TouchWep_Ballistic(0.5f, 0, 1000, 0, 800, &elev, &t), "lob straight up: no solution");

	// no gravity: straight at it
	ok = TouchWep_Ballistic(1000, 1000, 1000, 0, 0, &elev, &t);
	CHECK(ok && fabs(elev - atan(1.0)) < 1e-4, "no gravity: aims straight");
}

static void test_aimpoint(void)
{
	const touch_wep_t *vortex = TouchWep_Find("vortex"), *hagar = TouchWep_Find("hagar");
	const touch_wep_t *mortar = TouchWep_Find("mortar");
	float rel[3], vel[3], aim[3], tof;
	qbool ok;

	// instant hit: the body itself, no flight time
	v3(rel, 1000, 50, 20);
	v3(vel, 0, 400, 0);
	ok = TouchWep_AimPoint(&vortex->pri, rel, vel, 0.1f, 800, aim, &tof);
	CHECK(ok && aim[0] == 1000 && aim[1] == 50 && aim[2] == 20 && tof == 0, "vortex: no lead");

	// Hagar at a strafing target, 100 ms latency: the target first moves
	// 40 units, then the rocket and it meet
	v3(rel, 1000, 0, 0);
	ok = TouchWep_AimPoint(&hagar->pri, rel, vel, 0.1f, 800, aim, &tof);
	CHECK(ok, "hagar lead: solvable");
	CHECK_NEAR(40 + 400 * tof, aim[1], 1e-2, "hagar lead: aim is where the target will be");
	CHECK_NEAR(2200 * tof, sqrt(aim[0] * aim[0] + aim[1] * aim[1] + aim[2] * aim[2]), 0.5, "hagar lead: rocket flies exactly that far");

	// Mortar at a strafing target: simulate the lob and check where it lands
	v3(rel, 1200, 0, -20);
	v3(vel, 0, 300, 0);
	ok = TouchWep_AimPoint(&mortar->pri, rel, vel, 0.05f, 800, aim, &tof);
	CHECK(ok, "mortar lead: solvable");
	if (ok)
	{
		double dx = sqrt(aim[0] * aim[0] + aim[1] * aim[1]);
		double elev = atan2(aim[2], dx);
		double ty = 300 * (0.05 + tof);
		CHECK_NEAR(ty, aim[1], 1.0, "mortar lead: yaw toward where the target will be");
		CHECK_NEAR(-20, lob_height(1900, 225, 800, elev, sqrt(1200.0 * 1200 + ty * ty)), 2.0, "mortar lead: lob lands at the target height");
	}

	// beyond the lead cap: no lead
	v3(rel, 3500, 0, 0);
	v3(vel, 0, 0, 0);
	CHECK(!TouchWep_AimPoint(&hagar->pri, rel, vel, 0, 800, aim, &tof), "hagar at 3500 (1.6 s): no lead");
}

static void test_slew(void)
{
	float cur[3], want[3], t;
	int frames;
	// a steady strafe: the wanted lead holds still, the slewed one reaches it
	v3(cur, 0, 0, 0);
	v3(want, 0, 120, 0);
	for (frames = 0; frames < 60; frames++)
		TouchWep_LeadSlew(cur, want, 360, 1.0f / 60);
	CHECK_NEAR(120, cur[1], 1e-3, "slew: a steady lead is reached and held exactly");
	TouchWep_LeadSlew(cur, want, 360, 1.0f / 60);
	CHECK_NEAR(120, cur[1], 1e-6, "slew: no drift once there");

	// a reversal: the lead flips to the other side; it may move at most
	// (2 x 360 + 100) units per second, so 240 units take about 0.29 s
	v3(want, 0, -120, 0);
	TouchWep_LeadSlew(cur, want, 360, 0.1f);
	CHECK_NEAR(120 - 82, cur[1], 1e-3, "slew: one 0.1 s step moves 82 units toward the new side");
	for (frames = 0, t = 0.1f; cur[1] > -120 + 1e-3f && frames < 1000; frames++, t += 1.0f / 60)
		TouchWep_LeadSlew(cur, want, 360, 1.0f / 60);
	CHECK_NEAR(240.0 / 820.0, t, 0.02, "slew: a full 240-unit flip takes 240 / 820 s");

	// a standing target still slews at the floor rate
	v3(cur, 0, 0, 0);
	v3(want, 50, 0, 0);
	TouchWep_LeadSlew(cur, want, 0, 0.1f);
	CHECK_NEAR(10, cur[0], 1e-3, "slew: 100 u/s floor for a target that stopped");
	// nothing moves with no time
	TouchWep_LeadSlew(cur, want, 360, 0);
	CHECK_NEAR(10, cur[0], 1e-6, "slew: dt 0 moves nothing");
	TouchWep_LeadSlew(cur, want, 360, -1);
	CHECK_NEAR(10, cur[0], 1e-6, "slew: negative dt moves nothing");
	// a spike passes: the wanted lead falls from 2500 back to 120 units;
	// the slewed one may not stay longer than the wanted one
	v3(cur, 0, 2500, 0);
	v3(want, 0, 120, 0);
	TouchWep_LeadSlew(cur, want, 360, 1.0f / 60);
	CHECK(cur[1] <= 120 + 1e-3f, "slew: never longer than the wanted lead after a spike");
	// at a reversal the slewed lead stays on its side but shrinks with the wanted one
	v3(cur, 0, 120, 0);
	v3(want, 0, -60, 0);
	TouchWep_LeadSlew(cur, want, 360, 1.0f / 60);
	CHECK(cur[1] > 0 && cur[1] <= 60 + 1e-3f, "slew: reversing lead shrinks to the wanted length, on its way across");
	// direction is kept: a diagonal move stays on the line to the target
	v3(cur, 0, 0, 0);
	v3(want, 300, 400, 0);
	TouchWep_LeadSlew(cur, want, 0, 1);
	CHECK(fabs(cur[0] - 60) < 1e-3 && fabs(cur[1] - 80) < 1e-3, "slew: moves along the line toward the wanted lead");
}

static void test_window(void)
{
	const touch_wep_t *w;
	CHECK_NEAR(0, TouchWep_FireWindow(&TouchWep_Find("vortex")->pri, 1000, 1.5f), 1e-6, "vortex: only on the body");
	CHECK_NEAR(0, TouchWep_FireWindow(&TouchWep_Find("shotgun")->pri, 500, 0), 1e-6, "radius 0: only on the body");
	CHECK_NEAR(1.5, TouchWep_FireWindow(&TouchWep_Find("shotgun")->pri, 500, 1.5f), 1e-6, "shotgun: pellets cover the radius");
	CHECK_NEAR(atan(0.015) * TW_DEG, TouchWep_FireWindow(&TouchWep_Find("machinegun")->pri, 500, 1.5f), 1e-4, "machinegun: half its spread");
	w = TouchWep_Find("devastator");
	CHECK_NEAR(1.5, TouchWep_FireWindow(&w->pri, 1000, 1.5f), 1e-6, "devastator at 1000: splash covers the radius");
	CHECK_NEAR(atan(55.0 / 5000) * TW_DEG, TouchWep_FireWindow(&w->pri, 5000, 1.5f), 1e-4, "devastator at 5000: half the splash");
	CHECK_NEAR(6, TouchWep_FireWindow(&w->pri, 100, 6), 1e-6, "radius 6 respected when the splash covers it");
}

typedef struct mode_case_s
{
	const char *what, *netname;
	float dist, impact;
	qbool pri, alt, use_alt;
	int mode, why;
} mode_case_t;

static const mode_case_t mode_cases[] =
{
	{ "unknown weapon: primary",         "railgun",      500,  500, true,  true,  true,  TW_MODE_PRIMARY,   TW_WHY_DEFAULT },
	{ "tuba never",                      "tuba",         500,  500, true,  true,  true,  TW_MODE_NONE,      TW_WHY_NOAUTO },
	{ "porto never",                     "porto",        500,  500, true,  true,  true,  TW_MODE_NONE,      TW_WHY_NOAUTO },
	{ "hook never",                      "hook",         500,  500, true,  true,  true,  TW_MODE_NONE,      TW_WHY_NOAUTO },
	{ "shotgun melee point-blank",       "shotgun",       80,   80, true,  true,  true,  TW_MODE_SECONDARY, TW_WHY_MELEE },
	{ "shotgun melee off: primary",      "shotgun",       80,   80, true,  true,  false, TW_MODE_PRIMARY,   TW_WHY_DEFAULT },
	{ "shotgun at 300: primary",         "shotgun",      300,  300, true,  true,  true,  TW_MODE_PRIMARY,   TW_WHY_DEFAULT },
	{ "shotgun beyond its range",        "shotgun",     1500, 1500, true,  true,  true,  TW_MODE_NONE,      TW_WHY_RANGE },
	{ "shotgun out of shells: melee",    "shotgun",       90,   90, false, true,  true,  TW_MODE_SECONDARY, TW_WHY_MELEE },
	{ "shotgun out of shells, far",      "shotgun",      500,  500, false, true,  true,  TW_MODE_NONE,      TW_WHY_NO_AMMO },
	{ "crylink secondary",               "crylink",      600,  600, true,  true,  true,  TW_MODE_SECONDARY, TW_WHY_CRYLINK },
	{ "crylink secondary off",           "crylink",      600,  600, true,  true,  false, TW_MODE_PRIMARY,   TW_WHY_DEFAULT },
	{ "crylink secondary empty",         "crylink",      600,  600, true,  false, true,  TW_MODE_PRIMARY,   TW_WHY_DEFAULT },
	{ "crylink close: primary is safe",  "crylink",      130,  130, true,  true,  true,  TW_MODE_PRIMARY,   TW_WHY_OWN_SPLASH },
	{ "crylink touching: nothing safe",  "crylink",       90,   90, true,  true,  true,  TW_MODE_NONE,      TW_WHY_OWN_SPLASH },
	{ "machinegun mid: primary",         "machinegun",   800,  800, true,  true,  true,  TW_MODE_PRIMARY,   TW_WHY_DEFAULT },
	{ "machinegun far: burst",           "machinegun",  2000, 2000, true,  true,  true,  TW_MODE_SECONDARY, TW_WHY_BURST },
	{ "machinegun far, burst empty",     "machinegun",  2000, 2000, true,  false, true,  TW_MODE_PRIMARY,   TW_WHY_DEFAULT },
	{ "arc beam in range",               "arc",         1000, 1000, true,  true,  true,  TW_MODE_PRIMARY,   TW_WHY_DEFAULT },
	{ "arc bolt beyond the beam",        "arc",         2000, 2000, true,  true,  true,  TW_MODE_SECONDARY, TW_WHY_BOLT },
	{ "arc beyond the beam, alt off",    "arc",         2000, 2000, true,  true,  false, TW_MODE_NONE,      TW_WHY_ALT_OFF },
	{ "vaporizer with cells",            "vaporizer",   1500, 1500, true,  true,  true,  TW_MODE_PRIMARY,   TW_WHY_DEFAULT },
	{ "vaporizer out of cells",          "vaporizer",   1500, 1500, false, true,  true,  TW_MODE_SECONDARY, TW_WHY_EMPTY_ALT },
	{ "vortex: zoom is no shot",         "vortex",      1500, 1500, true,  true,  true,  TW_MODE_PRIMARY,   TW_WHY_DEFAULT },
	{ "vortex out of cells",             "vortex",      1500, 1500, false, true,  true,  TW_MODE_NONE,      TW_WHY_NO_AMMO },
	{ "electro primary",                 "electro",      700,  700, true,  true,  true,  TW_MODE_PRIMARY,   TW_WHY_DEFAULT },
	{ "hagar primary (load is manual)",  "hagar",        700,  700, true,  true,  true,  TW_MODE_PRIMARY,   TW_WHY_DEFAULT },
	{ "hlac primary",                    "hlac",         700,  700, true,  true,  true,  TW_MODE_PRIMARY,   TW_WHY_DEFAULT },
	{ "mortar primary",                  "mortar",       700,  700, true,  true,  true,  TW_MODE_PRIMARY,   TW_WHY_DEFAULT },
	{ "rifle primary",                   "rifle",       1500, 1500, true,  true,  true,  TW_MODE_PRIMARY,   TW_WHY_DEFAULT },
	{ "blaster primary",                 "blaster",      700,  700, true,  true,  true,  TW_MODE_PRIMARY,   TW_WHY_DEFAULT },
	{ "blaster point-blank (weak)",      "blaster",       50,   50, true,  true,  true,  TW_MODE_PRIMARY,   TW_WHY_DEFAULT },
	{ "devastator far",                  "devastator",   900,  900, true,  true,  true,  TW_MODE_PRIMARY,   TW_WHY_DEFAULT },
	{ "devastator too close",            "devastator",   140,  140, true,  true,  true,  TW_MODE_NONE,      TW_WHY_OWN_SPLASH },
	{ "devastator lead lands close",     "devastator",   400,  120, true,  true,  true,  TW_MODE_NONE,      TW_WHY_OWN_SPLASH },
	{ "mortar inside its splash",        "mortar",       150,  150, true,  true,  true,  TW_MODE_NONE,      TW_WHY_OWN_SPLASH },
	{ "electro inside: orbs no safer",   "electro",      120,  120, true,  true,  true,  TW_MODE_NONE,      TW_WHY_OWN_SPLASH },
	{ "okhmg primary",                   "okhmg",        800,  800, true,  true,  true,  TW_MODE_PRIMARY,   TW_WHY_DEFAULT },
	{ "oknex empty: blaster shot",       "oknex",        800,  800, false, true,  true,  TW_MODE_SECONDARY, TW_WHY_EMPTY_ALT },
	{ "okrpc inside 340",                "okrpc",        300,  300, true,  true,  true,  TW_MODE_SECONDARY, TW_WHY_OWN_SPLASH },
};

static void test_modes(void)
{
	size_t i;
	for (i = 0; i < sizeof(mode_cases) / sizeof(mode_cases[0]); i++)
	{
		const mode_case_t *c = &mode_cases[i];
		touch_wep_situation_t s;
		int why = -1, mode;
		char what[160];
		s.dist = c->dist;
		s.impact_dist = c->impact;
		s.pri_ready = c->pri;
		s.alt_ready = c->alt;
		s.use_alt = c->use_alt;
		mode = TouchWep_FireMode(TouchWep_Find(c->netname), &s, &why);
		snprintf(what, sizeof(what), "fire mode: %s", c->what);
		CHECK_EQ(c->mode, mode, what);
		snprintf(what, sizeof(what), "fire mode reason: %s (%s)", c->what, why >= 0 ? touch_wep_why[why] : "?");
		CHECK_EQ(c->why, why, what);
	}
}

static void test_choose(void)
{
	static const char *const owned[] = { "blaster", "shotgun", "machinegun", "mortar", "devastator", "vortex", "tuba", "minelayer" };
	qbool ready[8] = { true, true, true, true, true, true, true, true };
	int rank = -2, i;

	// far (> 1050): vortex
	i = TouchWep_Choose(owned, ready, 8, 2000, &rank);
	CHECK(i == 5 && rank == 2, "far: vortex");
	// mid: vortex is still above the devastator? no: the mid list puts the devastator second
	i = TouchWep_Choose(owned, ready, 8, 800, &rank);
	CHECK(i == 4 && rank == 1, "mid: devastator");
	// close: vortex
	i = TouchWep_Choose(owned, ready, 8, 300, &rank);
	CHECK(i == 5 && rank == 2, "close: vortex");
	// close without cells: shotgun
	ready[5] = false;
	i = TouchWep_Choose(owned, ready, 8, 300, &rank);
	CHECK(i == 1, "close, vortex empty: shotgun");
	// mid without rockets either: machinegun
	ready[3] = ready[4] = false;
	i = TouchWep_Choose(owned, ready, 8, 800, &rank);
	CHECK(i == 2, "mid, no rockets or cells: machinegun");
	// point-blank: never the devastator inside its own splash
	ready[3] = ready[4] = true;
	ready[1] = ready[2] = false;
	i = TouchWep_Choose(owned, ready, 8, 120, &rank);
	CHECK(i == 0, "point-blank with only splash weapons and blaster: blaster");
	// nothing usable at all
	{
		static const char *const junk[] = { "tuba", "porto", "hook", "minelayer", "seeker", "fireball", "railgun" };
		qbool r[7] = { true, true, true, true, true, true, true };
		i = TouchWep_Choose(junk, r, 7, 800, &rank);
		CHECK(i == -1 && rank == -1, "only weapons a thumb should not get: none");
	}

	CHECK_EQ(0, TouchWep_Band(250), "band: 250 is close");
	CHECK_EQ(0, TouchWep_Band(500), "band: 500 is close (d' 300)");
	CHECK_EQ(1, TouchWep_Band(501), "band: 501 is mid");
	CHECK_EQ(2, TouchWep_Band(1051), "band: 1051 is far");
	CHECK_EQ(-1, TouchWep_Rank("tuba", 800), "tuba is never ranked");

	CHECK(TouchWep_ShouldSwitch(5, false, 9), "switch when the current weapon cannot shoot");
	CHECK(TouchWep_ShouldSwitch(-1, true, 9), "switch away from an unranked weapon");
	CHECK(TouchWep_ShouldSwitch(5, true, 3), "switch for a clear win");
	CHECK(!TouchWep_ShouldSwitch(5, true, 4), "no switch for one place");
	CHECK(!TouchWep_ShouldSwitch(5, true, -1), "no switch to nothing");
	CHECK(!TouchWep_Usable(TouchWep_Find("devastator"), 120), "devastator unusable inside its splash");
	CHECK(TouchWep_Usable(TouchWep_Find("arc"), 2000), "arc usable beyond the beam: bolts");
	CHECK(!TouchWep_Usable(TouchWep_Find("shotgun"), 2000), "shotgun unusable at 2000");
}

static void test_detonate(void)
{
	float rocket[3], rv[3], enemy[3], self[3];
	v3(self, 0, 0, 0);
	v3(enemy, 1000, 0, 0);

	// far from the enemy: no
	v3(rocket, 500, 0, 0);
	v3(rv, 1300, 0, 0);
	CHECK(!TouchWep_Detonate(rocket, rv, enemy, self, 110), "rocket 500 short: wait");
	// inside 0.8 R but still closing: wait for a direct hit or the pass
	v3(rocket, 920, 0, 0);
	CHECK(!TouchWep_Detonate(rocket, rv, enemy, self, 110), "closing head-on at 80: wait for the hit");
	// passing beside the enemy at 60 units, alongside now: detonate
	v3(rocket, 1000, 60, 0);
	CHECK(TouchWep_Detonate(rocket, rv, enemy, self, 110), "alongside at 60: detonate");
	// about to pass at 100 units (outside 0.8 R) and moving away: detonate
	v3(rocket, 1010, 100, 0);
	CHECK(TouchWep_Detonate(rocket, rv, enemy, self, 110), "passing at 100: detonate before it leaves the radius");
	// approaching at 100 units, will get closer: wait
	v3(rocket, 900, 30, 0);
	CHECK(!TouchWep_Detonate(rocket, rv, enemy, self, 110), "approaching at 100: wait");
	// out of the radius: no
	v3(rocket, 1000, 150, 0);
	CHECK(!TouchWep_Detonate(rocket, rv, enemy, self, 110), "passing at 150: out of reach");
	// enemy next to the player: never
	v3(enemy, 100, 0, 0);
	v3(rocket, 100, 60, 0);
	CHECK(!TouchWep_Detonate(rocket, rv, enemy, self, 110), "self inside the radius: never");
}

static void test_table(void)
{
	int i, j;
	for (i = 0; i < TW_NUM_WEAPONS; i++)
	{
		const touch_wep_t *w = &touch_weapons[i];
		char what[128];
		const touch_wep_fire_t *fires[2];
		int k;
		fires[0] = &w->pri;
		fires[1] = &w->sec;
		snprintf(what, sizeof(what), "table %s: unique netname", w->netname);
		for (j = 0; j < i; j++)
			CHECK(strcmp(touch_weapons[j].netname, w->netname), what);
		for (k = 0; k < 2; k++)
		{
			const touch_wep_fire_t *f = fires[k];
			snprintf(what, sizeof(what), "table %s %s: sane numbers", w->netname, k ? "secondary" : "primary");
			CHECK(f->kind >= TW_KIND_NONE && f->kind <= TW_KIND_MELEE, what);
			CHECK(f->splash >= 0 && f->splash <= 400 && f->spread >= 0 && f->spread <= 0.5f && f->damage >= 0, what);
			if (f->kind == TW_KIND_PROJ || f->kind == TW_KIND_BALLISTIC)
				CHECK(f->speed >= 500 && f->speed <= 10000 && f->speed_up >= 0, what);
			if (f->accel > 0)
				CHECK(f->speed_max > f->speed, what);
			if (f->kind != TW_KIND_NONE)
				CHECK(f->range >= 100, what);
		}
		snprintf(what, sizeof(what), "table %s: primary ammo cost 0..10", w->netname);
		CHECK(w->cost >= 0 && w->cost <= 10, what);
		snprintf(what, sizeof(what), "table %s: secondary described iff it is a shot", w->netname);
		CHECK((w->sec_what == TW_SEC_SHOT || w->sec_what == TW_SEC_MELEE) == (w->sec.kind != TW_KIND_NONE), what);
		// agrees with the ALT button's stock-balance list
		snprintf(what, sizeof(what), "table %s: secondary agrees with touch_altfire.h", w->netname);
		{
			qbool has_alt = TouchAltFire_Fallback(w->netname, 100, -1, false) != TOUCH_ALTFIRE_NONE;
			qbool shoots = w->sec_what == TW_SEC_SHOT || w->sec_what == TW_SEC_MELEE || w->sec_what == TW_SEC_DETONATE
				|| w->sec_what == TW_SEC_LOAD || (w->flags & TW_NO_AUTOFIRE);
			CHECK(has_alt == shoots, what);
		}
	}
	// every weapon in the preference lists exists and may be switched to
	{
		const char *const *lists[3];
		lists[0] = touch_wep_far;
		lists[1] = touch_wep_mid;
		lists[2] = touch_wep_close;
		for (i = 0; i < 3; i++)
			for (j = 0; lists[i][j]; j++)
			{
				const touch_wep_t *w = TouchWep_Find(lists[i][j]);
				char what[128];
				snprintf(what, sizeof(what), "preference list %d: %s known and switchable", i, lists[i][j]);
				CHECK(w && !(w->flags & TW_NO_SWITCH_TO), what);
			}
	}
	CHECK(TouchWep_Find(NULL) == NULL && TouchWep_Find("") == NULL && TouchWep_Find("railgun") == NULL, "unknown netnames");
}

int main(void)
{
	test_intercept();
	test_accel();
	test_ballistic();
	test_aimpoint();
	test_slew();
	test_window();
	test_modes();
	test_choose();
	test_detonate();
	test_table();
	if (failures)
	{
		printf("%d of %d check(s) failed\n", failures, checks);
		return 1;
	}
	printf("touch aim: %d checks OK (%d weapons, %d fire-mode cases)\n", checks, TW_NUM_WEAPONS,
		(int)(sizeof(mode_cases) / sizeof(mode_cases[0])));
	return 0;
}
