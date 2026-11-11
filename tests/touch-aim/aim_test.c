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
#include <string.h>

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

static void test_air(void)
{
	const touch_wep_t *hagar = TouchWep_Find("hagar"), *mortar = TouchWep_Find("mortar");
	float rel[3], vel[3], out[3], aim[3], aim2[3], tof, tof2, tl;
	qbool ok;

	// the fall: 100 up, running 100/s, lands after 0.5 s
	tl = TouchWep_LandTime(100, 0, 800);
	CHECK_NEAR(0.5, tl, 1e-4, "land time: 100 units, no vertical speed");
	CHECK_NEAR((400 + sqrt(160000.0 + 1600)) / 800, TouchWep_LandTime(1, 400, 800), 1e-4, "land time: jumping up at 400");
	CHECK_NEAR(0, TouchWep_LandTime(0, 300, 800), 1e-6, "land time: already down");
	v3(rel, 0, 0, 100);
	v3(vel, 100, 0, 0);
	TouchWep_TargetAt(rel, vel, 800, tl, 0.25f, out);
	CHECK_NEAR(75, out[2], 1e-3, "target in the air: fallen 25 units after 0.25 s");
	TouchWep_TargetAt(rel, vel, 800, tl, 1.0f, out);
	CHECK_NEAR(0, out[2], 1e-3, "target landed: on the floor");
	CHECK_NEAR(50 + 100 * TW_LAND_SLOW * 0.5, out[0], 1e-3, "target landed: slower on the ground");
	TouchWep_TargetAt(rel, vel, 800, -1, 1.0f, out);
	CHECK_NEAR(100, out[0], 1e-6, "grounded model: straight line");
	CHECK_NEAR(100, out[2], 1e-6, "grounded model: no fall");

	// grounded: identical to the old solve
	v3(rel, 900, 0, 0);
	v3(vel, 0, 300, 0);
	TouchWep_AimPoint(&hagar->pri, rel, vel, 0.05f, 800, aim, &tof);
	TouchWep_AimPointAir(&hagar->pri, rel, vel, 0.05f, 800, -1, aim2, &tof2);
	CHECK(aim[0] == aim2[0] && aim[1] == aim2[1] && aim[2] == aim2[2] && tof == tof2, "air solve with a grounded target is the old solve");

	// Hagar at a target falling from 200 units up, 800 away: meets it on the fall
	v3(rel, 800, 0, 200);
	v3(vel, 0, 300, 0);
	tl = TouchWep_LandTime(200, 0, 800);
	ok = TouchWep_AimPointAir(&hagar->pri, rel, vel, 0.05f, 800, tl, aim, &tof);
	CHECK(ok, "hagar at a falling target: solvable");
	TouchWep_TargetAt(rel, vel, 800, tl, tof + 0.05f, out);
	CHECK(fabs(out[0] - aim[0]) < 1e-3 && fabs(out[1] - aim[1]) < 1e-3 && fabs(out[2] - aim[2]) < 1e-3, "hagar at a falling target: aims where it will be");
	CHECK_NEAR(2200 * tof, sqrt(aim[0] * aim[0] + aim[1] * aim[1] + aim[2] * aim[2]), 1.0, "hagar at a falling target: rocket flies exactly that far");
	TouchWep_AimPoint(&hagar->pri, rel, vel, 0.05f, 800, aim2, &tof2);
	CHECK(aim[2] < aim2[2] - 20, "hagar at a falling target: aims lower than the straight-line lead");

	// Mortar at a jumping target: the lob comes down where it is then
	v3(rel, 900, 0, 0);
	v3(vel, 0, 250, 300);
	tl = TouchWep_LandTime(1, 300, 800);
	ok = TouchWep_AimPointAir(&mortar->pri, rel, vel, 0.05f, 800, tl, aim, &tof);
	CHECK(ok, "mortar at a jumping target: solvable");
	if (ok)
	{
		double dx = sqrt(aim[0] * aim[0] + aim[1] * aim[1]);
		TouchWep_TargetAt(rel, vel, 800, tl, tof + 0.05f, out);
		CHECK_NEAR(out[2], lob_height(1900, 225, 800, atan2(aim[2], dx), dx), 2.0, "mortar at a jumping target: the lob reaches its height then");
	}
}

static void test_scale(void)
{
	CHECK_NEAR(1, TouchWep_LeadScale(0.4f, 0, 0), 1e-6, "lead scale: no reversals, full lead");
	CHECK_NEAR(0.5, TouchWep_LeadScale(0.25f, 0.5f, 0), 1e-6, "lead scale: half the reversal period ahead, half the lead");
	CHECK_NEAR(1, TouchWep_LeadScale(0.2f, 0.5f, 0), 1e-6, "lead scale: a shot faster than half a period keeps its full lead");
	CHECK_NEAR(0.3, TouchWep_LeadScale(0.35f, 0.5f, 0), 1e-5, "lead scale: a slow shot leads the average share");
	CHECK_NEAR(0.5, TouchWep_LeadScale(0.25f, 0.5f, 0.45f), 1e-6, "lead scale: the time since the last reversal does not cut it further");
	CHECK_NEAR(0, TouchWep_LeadScale(0.8f, 0.5f, 0), 1e-6, "lead scale: flight longer than the period, no lead");
}

static void test_group(void)
{
	touch_wep_body_t b[3];
	float cands[3][3], sc, p[3];
	int best;
	CHECK_NEAR(80, TouchWep_SplashDamage(0, 80, 40, 110), 1e-6, "splash: core at the blast");
	CHECK_NEAR(40, TouchWep_SplashDamage(110, 80, 40, 110), 1e-6, "splash: edge at the radius");
	CHECK_NEAR(60, TouchWep_SplashDamage(55, 80, 40, 110), 1e-6, "splash: halfway");
	CHECK_NEAR(0, TouchWep_SplashDamage(111, 80, 40, 110), 1e-6, "splash: nothing beyond");
	// two standing enemies 180 units apart (feet at x = 0 and 180)
	v3(b[0].bmin, -16, -16, 0); v3(b[0].bmax, 16, 16, 69); b[0].weight = 1;
	v3(b[1].bmin, 164, -16, 0); v3(b[1].bmax, 196, 16, 69); b[1].weight = 1;
	v3(p, 0, 0, 0);
	CHECK_NEAR(0, TouchWep_BoxGap(p, b[0].bmin, b[0].bmax), 1e-6, "gap: inside the box");
	v3(p, 100, 0, 0);
	CHECK_NEAR(64, TouchWep_BoxGap(p, b[1].bmin, b[1].bmax), 1e-6, "gap: beside the box");
	v3(cands[0], 0, 0, 4);
	v3(cands[1], 180, 0, 4);
	v3(cands[2], 90, 0, 4);
	best = TouchWep_BestImpact((const float (*)[3])cands, 3, b, 2, 80, 40, 110, -1, &sc);
	CHECK_EQ(2, best, "group: the floor between two enemies 180 apart beats either one's feet");
	CHECK_NEAR(2 * TouchWep_SplashDamage(74, 80, 40, 110), sc, 1e-3, "group: both caught by the splash");
	// hysteresis: having aimed at A's feet, a point only a little better does not take over
	b[1].weight = 0.1f;
	best = TouchWep_BestImpact((const float (*)[3])cands, 3, b, 2, 80, 40, 110, 0, &sc);
	CHECK_EQ(0, best, "group: keeps the point in use unless another is clearly better");
	// a teammate at the midpoint makes it worse than one enemy alone
	b[1].weight = 1;
	v3(b[2].bmin, 74, -16, 0); v3(b[2].bmax, 106, 16, 69); b[2].weight = -0.5f;
	CHECK_NEAR(2 * TouchWep_SplashDamage(74, 80, 40, 110) - 0.5 * 80, TouchWep_GroupScore(cands[2], b, 3, 80, 40, 110), 1e-3,
		"group: a teammate in the blast takes half its damage off the score");
	b[2].weight = -2;
	best = TouchWep_BestImpact((const float (*)[3])cands, 3, b, 3, 80, 40, 110, -1, &sc);
	CHECK_EQ(-1, best, "group: a heavily weighted teammate in every blast: no point is worth it");
}

static void test_detonate_group(void)
{
	touch_wep_body_t b[2];
	float vels[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } }, rocket[3], rv[3], smin[3], smax[3];
	v3(smin, -16, -16, -24); v3(smax, 16, 16, 45);
	// two enemies 150 apart at x = 1000, the rocket passes between them along x
	v3(b[0].bmin, 984, -91, -24); v3(b[0].bmax, 1016, -59, 45); b[0].weight = 1;
	v3(b[1].bmin, 984, 59, -24); v3(b[1].bmax, 1016, 91, 45); b[1].weight = 1;
	v3(rv, 1300, 0, 0);
	v3(rocket, 700, 0, 0);
	CHECK(!TouchWep_DetonateGroup(rocket, rv, b, (const float (*)[3])vels, 2, smin, smax, 110, 0, 1.0f / 60), "group detonation: far away, wait");
	v3(rocket, 960, 0, 0);
	CHECK(!TouchWep_DetonateGroup(rocket, rv, b, (const float (*)[3])vels, 2, smin, smax, 110, 0, 1.0f / 60), "group detonation: still closing on both, wait");
	v3(rocket, 1000, 0, 0);
	CHECK(TouchWep_DetonateGroup(rocket, rv, b, (const float (*)[3])vels, 2, smin, smax, 110, 0, 1.0f / 60), "group detonation: between them, at the peak");
	// with 80 ms latency the same press goes out ~100 units earlier
	v3(rocket, 900, 0, 0);
	CHECK(TouchWep_DetonateGroup(rocket, rv, b, (const float (*)[3])vels, 2, smin, smax, 110, 0.08f, 1.0f / 60), "group detonation: latency moves the press earlier");
	// player standing right there: never
	v3(smin, 984, -16, -24); v3(smax, 1016, 16, 45);
	v3(rocket, 1000, 0, 0);
	CHECK(!TouchWep_DetonateGroup(rocket, rv, b, (const float (*)[3])vels, 2, smin, smax, 110, 0, 1.0f / 60), "group detonation: never with the player in the blast");
	// heading straight into one: let it hit
	v3(smin, -16, -16, -24); v3(smax, 16, 16, 45);
	v3(rocket, 950, -75, 0);
	CHECK(!TouchWep_DetonateGroup(rocket, rv, b, (const float (*)[3])vels, 1, smin, smax, 110, 0, 0.05f), "group detonation: a direct hit is coming, wait for it");
}

static void test_smart(void)
{
	static const char *const owned[] = { "shotgun", "machinegun", "mortar", "vortex", "devastator" };
	qbool ready[5] = { true, true, true, true, true };
	float bu, hu;
	int i;
	CHECK_NEAR(142, TouchWep_ExpectedDPS("hagar", 400, TW_CLASS_STEADY), 1e-4, "dps: table value");
	CHECK_NEAR(123, TouchWep_ExpectedDPS("hagar", 600, TW_CLASS_STEADY), 1e-4, "dps: interpolated");
	CHECK_NEAR(35, TouchWep_ExpectedDPS("hagar", 4000, TW_CLASS_STEADY), 1e-4, "dps: beyond the table");
	CHECK_NEAR(-1, TouchWep_ExpectedDPS("tuba", 400, TW_CLASS_STEADY), 1e-6, "dps: unknown weapon");
	CHECK_NEAR(0, TouchWep_ExpectedDPS("devastator", 150, TW_CLASS_STAND), 1e-6, "dps: own blast range is worth nothing");
	CHECK_NEAR(142 * TW_HORIZON, TouchWep_Utility("hagar", 400, TW_CLASS_STEADY, 0, 0), 1e-3, "utility: in hand");
	CHECK_NEAR(52 * (TW_HORIZON - TW_SWITCH_COST), TouchWep_Utility("vortex", 400, TW_CLASS_STEADY, 0, TW_SWITCH_COST), 1e-3, "utility: switch cost");
	CHECK_NEAR(67 * TW_HORIZON * 1.8f, TouchWep_Utility("mortar", 400, TW_CLASS_STEADY, 1, 0), 1e-3, "utility: a second enemy in the blast");
	CHECK_NEAR(142 * TW_HORIZON, TouchWep_Utility("hagar", 400, TW_CLASS_STEADY, 1, 0), 1e-3, "utility: small splash gets no group bonus");
	// erratic strafer at 700 with nothing in hand: hitscan
	i = TouchWep_ChooseSmart(owned, ready, 5, -1, 700, TW_CLASS_ERRATIC, 0, true, &bu, &hu);
	CHECK_EQ(1, i, "smart choice: erratic strafer at 700 -> machinegun");
	// steady strafer at 700: an explosive at the feet
	i = TouchWep_ChooseSmart(owned, ready, 5, -1, 700, TW_CLASS_STEADY, 0, true, &bu, &hu);
	CHECK(i == 2 || i == 4, "smart choice: steady strafer at 700 -> mortar or devastator");
	// airborne at 400: the machinegun beats the explosives
	i = TouchWep_ChooseSmart(owned, ready, 5, -1, 400, TW_CLASS_AIR, 0, true, &bu, &hu);
	CHECK_EQ(1, i, "smart choice: airborne at 400 -> machinegun");
	// standing group at 900: an explosive
	i = TouchWep_ChooseSmart(owned, ready, 5, 1, 900, TW_CLASS_STAND, 2, true, &bu, &hu);
	CHECK(i == 2 || i == 4, "smart choice: a group of three -> an explosive");
	// point-blank: nothing explosive
	i = TouchWep_ChooseSmart(owned, ready, 5, 4, 120, TW_CLASS_STEADY, 0, true, &bu, &hu);
	CHECK(i == 0 || i == 1, "smart choice: point-blank -> shotgun or machinegun");
	// the one in hand keeps its place unless clearly beaten
	CHECK(!TouchWep_ShouldSwitchSmart(100, true, 120), "smart switch: 20 % better is not enough");
	CHECK(TouchWep_ShouldSwitchSmart(100, true, 131), "smart switch: 31 % better is");
	CHECK(TouchWep_ShouldSwitchSmart(100, false, 10), "smart switch: the held one cannot shoot");
	CHECK(!TouchWep_ShouldSwitchSmart(0, false, 0), "smart switch: nothing better to go to");
	ready[1] = false;
	i = TouchWep_ChooseSmart(owned, ready, 5, -1, 700, TW_CLASS_ERRATIC, 0, true, &bu, &hu);
	CHECK(i != 1, "smart choice: no bullets, no machinegun");
}

static void test_fallback(void)
{
	static const char *const owned[] = { "blaster", "machinegun", "shotgun", "mortar" };
	qbool ready[4] = { true, true, true, true };
	float bu, hu;
	int i, rank;
	CHECK(TouchWep_IsFallback("blaster"), "fallback: the blaster is a last resort");
	CHECK(!TouchWep_IsFallback("machinegun") && !TouchWep_IsFallback("railgun") && !TouchWep_IsFallback(NULL), "fallback: nothing else is");
	// far (1600): the bots' far list ranks the blaster above the machinegun; not any more
	i = TouchWep_Choose(owned, ready, 4, 1600, true, &rank);
	CHECK(i != 0, "list choice at 1600: never the blaster while a real weapon can shoot");
	ready[3] = false;
	i = TouchWep_Choose(owned, ready, 4, 1600, true, &rank);
	CHECK_EQ(1, i, "list choice at 1600, no mortar ammo: the machinegun");
	i = TouchWep_Choose(owned, ready, 4, 1600, false, &rank);
	CHECK_EQ(0, i, "the same with cl_touch_autoswitch_lastresort 0: the old list order (blaster)");
	ready[3] = true;
	// smart, standing target at 1500: the blaster's table DPS (22) beat the machinegun's (20)
	i = TouchWep_ChooseSmart(owned, ready, 4, -1, 1500, TW_CLASS_STAND, 0, true, &bu, &hu);
	CHECK(i != 0, "smart choice at 1500: never the blaster while a real weapon can shoot");
	// machinegun and mortar out of ammo, shotgun out of range: the blaster is all there is
	ready[1] = ready[3] = false;
	i = TouchWep_Choose(owned, ready, 4, 2000, true, &rank);
	CHECK_EQ(0, i, "list choice: nothing else can shoot at 2000 -> blaster");
	i = TouchWep_ChooseSmart(owned, ready, 4, -1, 2000, TW_CLASS_STEADY, 0, true, &bu, &hu);
	CHECK_EQ(0, i, "smart choice: nothing else can shoot at 2000 -> blaster");
	CHECK(bu > 0, "smart choice: the last resort still scores above nothing");
	// closer, the shotgun can shoot: not the blaster
	i = TouchWep_ChooseSmart(owned, ready, 4, -1, 600, TW_CLASS_STEADY, 0, true, &bu, &hu);
	CHECK_EQ(2, i, "smart choice: shotgun in reach -> shotgun, not the blaster");
	// held the blaster (the server switched to it): a real weapon wins, whatever the margin
	ready[1] = true;
	i = TouchWep_ChooseSmart(owned, ready, 4, 0, 1500, TW_CLASS_STAND, 0, true, &bu, &hu);
	CHECK_EQ(1, i, "smart choice with the blaster in hand: the machinegun");
}

static void test_guide(void)
{
	float eye[3] = { 0, 0, 0 }, org[3], dir[3], view[3], sp, prev[3], rel[3], relv[3], t, d;
	int i;
	// a rocket flying along +x, the view turned 90 degrees to +y: it turns
	// at most 90 deg/s (4.5 degrees per 0.05 s step)
	org[0] = 200; org[1] = 0; org[2] = 0;
	dir[0] = 1; dir[1] = 0; dir[2] = 0;
	view[0] = 0; view[1] = 1; view[2] = 0;
	sp = 1000;
	memcpy(prev, dir, sizeof(prev));
	TouchWep_GuideStep(org, dir, &sp, eye, view, 512, 90, 1300, 1300, 0.05f);
	CHECK(fabs(acos(tw_dot(prev, dir)) * 180 / M_PI - 4.5) < 0.01, "guide: one step turns by the rate limit");
	CHECK(fabs(sp - 1065) < 0.01, "guide: speeds up toward 1300");
	// left alone it settles onto the view ray
	for (i = 0; i < 80; i++)
		TouchWep_GuideStep(org, dir, &sp, eye, view, 512, 90, 1300, 1300, 0.05f);
	CHECK(fabs(org[0]) < 20 && dir[1] > 0.99f, "guide: the rocket ends up flying along the view ray");
	CHECK(sp == 1300, "guide: at full speed");
	// a straight flight past a target: 50 units off at its closest, 1 s out
	rel[0] = 1000; rel[1] = 50; rel[2] = 0;
	relv[0] = -1000; relv[1] = 0; relv[2] = 0;
	d = TouchWep_PassDistance(rel, relv, 3, &t);
	CHECK(fabs(d - 50) < 0.01 && fabs(t - 1) < 0.001, "pass distance: 50 units at 1 s");
	relv[0] = 1000;
	d = TouchWep_PassDistance(rel, relv, 3, &t);
	CHECK(t == 0 && d > 1000, "pass distance: flying away, closest now");
}

static void test_head(void)
{
	float s;
	// stock: no weapon pays for the head (the Rifle's multiplier is 0, the
	// Vortex has none at all)
	CHECK(TouchWep_HeadMultiplier("vortex", TW_MODE_PRIMARY, 0) == 0, "head: the vortex has no headshot bonus");
	CHECK(TouchWep_HeadMultiplier("vortex", TW_MODE_PRIMARY, 2) == 0, "head: the rifle setting does not give the vortex one");
	CHECK(TouchWep_HeadMultiplier("vaporizer", TW_MODE_PRIMARY, 2) == 0 && TouchWep_HeadMultiplier("machinegun", TW_MODE_PRIMARY, 2) == 0, "head: nor the vaporizer or machinegun");
	CHECK(TouchWep_HeadMultiplier("rifle", TW_MODE_PRIMARY, 0) == 0, "head: stock rifle: no bonus");
	CHECK(TouchWep_HeadMultiplier("rifle", TW_MODE_PRIMARY, 1.25f) == 1.25f && TouchWep_HeadMultiplier("rifle", TW_MODE_SECONDARY, 1.25f) == 0, "head: rifle primary with the server's multiplier");
	// no bonus: always the chest
	CHECK(!TouchWep_HeadBetter(0, 16, -24, 45, 1), "head: no bonus -> chest");
	CHECK(!TouchWep_HeadBetter(1, 16, -24, 45, 1), "head: multiplier 1 -> chest");
	// a big bonus, a standing target close by: the head
	s = TouchWep_AimSigma(300, TW_CLASS_STAND, 0);
	CHECK(TouchWep_HeadBetter(2, 16, -24, 45, s), "head: x2 at 300 standing -> head");
	// the Nexuiz 2.5 rifle (x1.25): head when close and still, chest far away or against a dodger
	CHECK(TouchWep_HeadBetter(1.25f, 16, -24, 45, TouchWep_AimSigma(200, TW_CLASS_STAND, 0)), "head: x1.25 at 200 standing -> head");
	CHECK(!TouchWep_HeadBetter(1.25f, 16, -24, 45, TouchWep_AimSigma(1500, TW_CLASS_STAND, 0)), "head: x1.25 at 1500 -> chest");
	CHECK(!TouchWep_HeadBetter(1.25f, 16, -24, 45, TouchWep_AimSigma(800, TW_CLASS_ERRATIC, 0)), "head: x1.25 at 800 against a dodger -> chest");
	// expected value sanity: a perfect aim at the head of a x2 weapon is worth 2
	CHECK_NEAR(2.0f, TouchWep_ShotValue(2, 16, -24, 45, 38.5f, 0), 0.001f, "head: perfect head shot worth the multiplier");
	CHECK_NEAR(1.0f, TouchWep_ShotValue(2, 16, -24, 45, 10.5f, 0), 0.001f, "head: perfect chest shot worth 1");
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
	i = TouchWep_Choose(owned, ready, 8, 2000, true, &rank);
	CHECK(i == 5 && rank == 2, "far: vortex");
	// mid: vortex is still above the devastator? no: the mid list puts the devastator second
	i = TouchWep_Choose(owned, ready, 8, 800, true, &rank);
	CHECK(i == 4 && rank == 1, "mid: devastator");
	// close: vortex
	i = TouchWep_Choose(owned, ready, 8, 300, true, &rank);
	CHECK(i == 5 && rank == 2, "close: vortex");
	// close without cells: shotgun
	ready[5] = false;
	i = TouchWep_Choose(owned, ready, 8, 300, true, &rank);
	CHECK(i == 1, "close, vortex empty: shotgun");
	// mid without rockets either: machinegun
	ready[3] = ready[4] = false;
	i = TouchWep_Choose(owned, ready, 8, 800, true, &rank);
	CHECK(i == 2, "mid, no rockets or cells: machinegun");
	// point-blank: never the devastator inside its own splash
	ready[3] = ready[4] = true;
	ready[1] = ready[2] = false;
	i = TouchWep_Choose(owned, ready, 8, 120, true, &rank);
	CHECK(i == 0, "point-blank with only splash weapons and blaster: blaster");
	// nothing usable at all
	{
		static const char *const junk[] = { "tuba", "porto", "hook", "minelayer", "seeker", "fireball", "railgun" };
		qbool r[7] = { true, true, true, true, true, true, true };
		i = TouchWep_Choose(junk, r, 7, 800, true, &rank);
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

// Several enemies: the priority shift and its parts.
static void test_priority(void)
{
	touch_wep_prio_t p;
	float base, b;
	memset(&p, 0, sizeof(p));
	p.dist = 800;
	p.dps_share = 0.5f;
	p.instant = true;
	base = TouchWep_PriorityBonus(&p);
	CHECK_NEAR(0, base, 1e-4, "priority: an average enemy gets no shift");
	p.dps_share = 1;
	CHECK_NEAR(1, TouchWep_PriorityBonus(&p), 1e-4, "priority: best damage +1 degree");
	p.dps_share = 0;
	CHECK_NEAR(-1, TouchWep_PriorityBonus(&p), 1e-4, "priority: no damage -1 degree");
	p.dps_share = -1;
	CHECK_NEAR(0, TouchWep_PriorityBonus(&p), 1e-4, "priority: weapon unknown: no damage term");
	p.facing = true;
	CHECK_NEAR(1, TouchWep_PriorityBonus(&p), 1e-4, "priority: facing the player up close +1");
	p.dist = 2000;
	CHECK_NEAR(0, TouchWep_PriorityBonus(&p), 1e-4, "priority: facing from far away counts nothing");
	p.dist = 800;
	p.facing = false;
	p.range_rate = -300;
	CHECK_NEAR(0.5f, TouchWep_PriorityBonus(&p), 1e-4, "priority: rushing in +0.5");
	p.range_rate = 300;
	p.dist = 1500;
	CHECK_NEAR(0, TouchWep_PriorityBonus(&p), 1e-4, "priority: running away from a hitscan weapon: no penalty");
	p.instant = false;
	CHECK_NEAR(-1.5f, TouchWep_PriorityBonus(&p), 1e-4, "priority: running out of a projectile's reach -1.5");
	p.range_rate = 0;
	p.dist = 800;
	p.dealt = 40;
	CHECK_NEAR(1, TouchWep_PriorityBonus(&p), 1e-4, "priority: already hurt +1");
	p.dealt = 0;
	p.group = 5;
	CHECK_NEAR(1.5f, TouchWep_PriorityBonus(&p), 1e-4, "priority: group bonus capped at three others");
	p.group = 3;
	p.dealt = 40;
	p.facing = true;
	p.range_rate = -300;
	p.dps_share = 1;
	b = TouchWep_PriorityBonus(&p);
	CHECK_NEAR(TW_PRIO_CAP, b, 1e-4, "priority: everything at once is capped (the angle still decides)");
	// the share of the weapon's best DPS
	CHECK_NEAR(1, TouchWep_DPSShare("hagar", 150, TW_CLASS_STAND), 1e-4, "dps share: hagar point-blank is its best");
	CHECK(TouchWep_DPSShare("hagar", 2500, TW_CLASS_ERRATIC) < 0.05f, "dps share: hagar far at a dodger is nothing");
	CHECK(TouchWep_DPSShare("railgun", 500, TW_CLASS_STAND) < 0, "dps share: unknown weapon");
}

// Teammates in the way of a shot.
static void test_mates(void)
{
	const touch_wep_t *mg = TouchWep_Find("machinegun"), *vortex = TouchWep_Find("vortex"), *dev = TouchWep_Find("devastator"), *mortar = TouchWep_Find("mortar");
	float eye[3] = { 0, 0, 0 }, fwd[3] = { 1, 0, 0 }, impact[3] = { 1000, 0, 0 };
	float lmin[3] = { 484, -16, -40 }, lmax[3] = { 516, 16, 29 };     // on the line, halfway
	float smin[3] = { 484, 60, -40 }, smax[3] = { 516, 92, 29 };      // beside the line, halfway
	float bmin[3] = { 1300, -16, -40 }, bmax[3] = { 1332, 16, 29 };   // on the line, behind the enemy
	float nmin[3] = { 1040, 40, -40 }, nmax[3] = { 1072, 72, 29 };    // next to the enemy
	float t;
	CHECK(TouchWep_RayBox(eye, fwd, lmin, lmax, &t) && fabs(t - 484) < 0.01f, "ray box: entry distance");
	CHECK(!TouchWep_RayBox(eye, fwd, smin, smax, &t), "ray box: miss beside");
	CHECK_EQ(TW_MATE_LINE, TouchWep_MateBlocks(&mg->pri, false, eye, fwd, 1000, impact, lmin, lmax), "mates: teammate on the machinegun's line");
	CHECK_EQ(TW_MATE_NONE, TouchWep_MateBlocks(&mg->pri, false, eye, fwd, 1000, impact, smin, smax), "mates: teammate beside the line is clear");
	CHECK_EQ(TW_MATE_NONE, TouchWep_MateBlocks(&mg->pri, false, eye, fwd, 1000, impact, bmin, bmax), "mates: teammate behind the enemy: the bullet stops at the enemy");
	CHECK_EQ(TW_MATE_LINE, TouchWep_MateBlocks(&vortex->pri, true, eye, fwd, 1000, impact, bmin, bmax), "mates: teammate behind the enemy: the vortex goes through");
	CHECK_EQ(TW_MATE_BLAST, TouchWep_MateBlocks(&dev->pri, false, eye, fwd, 1000, impact, nmin, nmax), "mates: teammate inside the rocket's blast");
	CHECK_EQ(TW_MATE_LINE, TouchWep_MateBlocks(&dev->pri, false, eye, fwd, 1000, impact, lmin, lmax), "mates: teammate in front of the rocket");
	CHECK_EQ(TW_MATE_NONE, TouchWep_MateBlocks(&mortar->pri, false, eye, fwd, 1000, impact, lmin, lmax), "mates: a lob passes over a teammate on the way");
	CHECK_EQ(TW_MATE_BLAST, TouchWep_MateBlocks(&mortar->pri, false, eye, fwd, 1000, impact, nmin, nmax), "mates: but not one next to where it lands");
	CHECK_EQ(TW_MATE_NONE, TouchWep_MateBlocks(&dev->pri, false, eye, fwd, 1000, impact, smin, smax), "mates: teammate beside the rocket's path, far from the blast");
}

// Ammo: shots left, time to dry, and keep / switch / keep away.
static void test_ammo(void)
{
	CHECK_EQ(10, TouchWep_ShotsLeft(40, -1, 4), "ammo: 40 rockets = 10 Devastator shots");
	CHECK_EQ(2, TouchWep_ShotsLeft(14, -1, 6), "ammo: 14 cells = 2 Vortex shots (no part shot)");
	CHECK_EQ(0, TouchWep_ShotsLeft(3, -1, 4), "ammo: 3 rockets cannot pay for a Devastator shot");
	CHECK_EQ(9, TouchWep_ShotsLeft(10, 80, 10), "ammo: clip and reserve both count");
	CHECK_EQ(TW_SHOTS_UNLIMITED, TouchWep_ShotsLeft(-1, -1, 1), "ammo: unlimited");
	CHECK_NEAR(0.5, TouchWep_TimeToDry(8, TouchWep_Refire("machinegun", false)), 1e-3, "ammo: 8 bullets of machinegun last 0.5 s");
	CHECK_NEAR(1.1, TouchWep_Refire("devastator", false), 1e-4, "refire: devastator");
	CHECK_NEAR(1, TouchWep_Refire("railgun", false), 1e-4, "refire: unknown weapon 1 s");
	// empty: switch to a real weapon, or keep away when only a last resort is left
	CHECK_EQ(TW_DRY_SWITCH, TouchWep_DryPlan(96, 0, 0.0625f, 150, false, 1.5f, 0.4f), "dry plan: empty hand, hagar there -> switch");
	CHECK_EQ(TW_DRY_EVADE, TouchWep_DryPlan(96, 0, 0.0625f, 29, true, 1.5f, 0.4f), "dry plan: empty hand, only the blaster -> keep away");
	CHECK_EQ(TW_DRY_EVADE, TouchWep_DryPlan(96, 0, 0.0625f, 0, false, 1.5f, 0.4f), "dry plan: nothing at all -> keep away");
	// a few shots left of a good weapon: fire them out first
	CHECK_EQ(TW_DRY_KEEP, TouchWep_DryPlan(96, 8, 0.0625f, 60, false, 1.5f, 0.4f), "dry plan: 0.5 s of machinegun left, a weaker one after: fire it out");
	// one slow shot left of a weak weapon, a much better one ready: switch now
	CHECK_EQ(TW_DRY_SWITCH, TouchWep_DryPlan(20, 1, 1.5f, 150, false, 1.5f, 0.4f), "dry plan: last weak shot, hagar ready -> switch now");
	CHECK_EQ(TW_DRY_KEEP, TouchWep_DryPlan(96, 50, 0.0625f, 150, false, 1.5f, 0.4f), "dry plan: plenty left -> keep");
}

int main(void)
{
	test_intercept();
	test_accel();
	test_ballistic();
	test_aimpoint();
	test_slew();
	test_air();
	test_scale();
	test_group();
	test_detonate_group();
	test_smart();
	test_fallback();
	test_guide();
	test_head();
	test_window();
	test_modes();
	test_choose();
	test_detonate();
	test_table();
	test_priority();
	test_mates();
	test_ammo();
	if (failures)
	{
		printf("%d of %d check(s) failed\n", failures, checks);
		return 1;
	}
	printf("touch aim: %d checks OK (%d weapons, %d fire-mode cases)\n", checks, TW_NUM_WEAPONS,
		(int)(sizeof(mode_cases) / sizeof(mode_cases[0])));
	return 0;
}
