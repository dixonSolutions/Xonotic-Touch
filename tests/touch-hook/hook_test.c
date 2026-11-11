/*
Checks the decisions behind the touch fall rescue
(engine/darkplaces/touch_rescue.h) in a world of boxes: where a fall comes
down (ground, void or a death zone) for walk-offs, jumps, drops to a lower
floor, jump pad arcs, water and lava; which anchor the hook is sent to; and
the rescue's steps -- when it fires, when it lets go, and the cases where it
must stay out of the way.

Run through scripts/test-touch-hook.sh.
*/

#include <math.h>
#include <stdbool.h>
#include <stdio.h>

typedef bool qbool;

#include "touch_rescue.h"

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

// ---------------------------------------------------------------------------
// A world of axis-aligned boxes, traced with the player's box (Xonotic's
// -16 -16 -24 .. 16 16 45) or a point.
// ---------------------------------------------------------------------------
#define K_SOLID 0
#define K_SKY   1
#define K_WATER 2
#define K_LAVA  3
#define K_HURT  4 // a trigger_hurt: not solid, a death volume

typedef struct box_s { float mins[3], maxs[3]; int kind; } box_t;

#define MAX_BOXES 32
static box_t boxes[MAX_BOXES];
static int nboxes;

static const float hull_mins[3] = { -16, -16, -24 };
static const float hull_maxs[3] = { 16, 16, 45 };

static void world_clear(void)
{
	nboxes = 0;
}

static void add_box(float x0, float y0, float z0, float x1, float y1, float z1, int kind)
{
	box_t *b = &boxes[nboxes++];
	b->mins[0] = x0; b->mins[1] = y0; b->mins[2] = z0;
	b->maxs[0] = x1; b->maxs[1] = y1; b->maxs[2] = z1;
	b->kind = kind;
}

// Entry fraction of start->end into box b grown by the hull, with the face's
// normal; -1 when it misses. A start inside counts as 0.
static float sweep(const box_t *b, const float start[3], const float end[3], int hull, float normal[3])
{
	float tmin = -1e9f, tmax = 1e9f;
	int i, axis = 2;
	float sign = 1;
	for (i = 0; i < 3; i++)
	{
		float lo = b->mins[i] - (hull ? hull_maxs[i] : 0);
		float hi = b->maxs[i] - (hull ? hull_mins[i] : 0);
		float d = end[i] - start[i];
		if (fabsf(d) < 1e-6f)
		{
			if (start[i] <= lo || start[i] >= hi)
				return -1;
			continue;
		}
		{
			float t0 = (lo - start[i]) / d, t1 = (hi - start[i]) / d;
			float s = -1;
			if (t0 > t1)
			{
				float t = t0;
				t0 = t1;
				t1 = t;
				s = 1;
			}
			if (t0 > tmin)
			{
				tmin = t0;
				axis = i;
				sign = s;
			}
			if (t1 < tmax)
				tmax = t1;
		}
	}
	if (tmin > tmax || tmax < 0 || tmin > 1)
		return -1;
	normal[0] = normal[1] = normal[2] = 0;
	normal[axis] = sign;
	return tmin < 0 ? 0 : tmin;
}

static void world_trace(void *ctx, const float start[3], const float end[3], int hull, tr_trace_t *out)
{
	int i;
	float best = 2, n[3];
	(void)ctx;
	out->fraction = 1;
	out->hit = TR_HIT_NONE;
	out->startsolid = 0;
	out->normal[0] = out->normal[1] = 0;
	out->normal[2] = 1;
	for (i = 0; i < nboxes; i++)
	{
		float f;
		if (boxes[i].kind == K_HURT)
			continue;
		f = sweep(&boxes[i], start, end, hull, n);
		if (f == 0 && sweep(&boxes[i], start, start, hull, n) == 0)
		{
			out->startsolid = 1; // begins inside
			out->fraction = 0;
			out->hit = boxes[i].kind == K_SOLID ? TR_HIT_SOLID : boxes[i].kind == K_WATER ? TR_HIT_WATER : TR_HIT_DEATH;
			break;
		}
		if (f >= 0 && f < best)
		{
			best = f;
			out->fraction = f;
			out->normal[0] = n[0];
			out->normal[1] = n[1];
			out->normal[2] = n[2];
			out->hit = boxes[i].kind == K_SOLID ? TR_HIT_SOLID : boxes[i].kind == K_WATER ? TR_HIT_WATER : TR_HIT_DEATH;
		}
	}
	for (i = 0; i < 3; i++)
		out->endpos[i] = start[i] + (end[i] - start[i]) * out->fraction;
}

static float world_death(void *ctx, const float start[3], const float end[3])
{
	int i;
	float best = 2, n[3];
	(void)ctx;
	for (i = 0; i < nboxes; i++)
	{
		float f;
		if (boxes[i].kind != K_HURT)
			continue;
		f = sweep(&boxes[i], start, end, 1, n);
		if (f >= 0 && f < best)
			best = f;
	}
	return best;
}

static tr_world_t world(void)
{
	tr_world_t w;
	memset(&w, 0, sizeof(w));
	w.trace = world_trace;
	w.death = world_death;
	w.floor_z = -2000;
	w.gravity = 800;
	return w;
}

static void v3(float *v, float x, float y, float z)
{
	v[0] = x;
	v[1] = y;
	v[2] = z;
}

// A floating platform 1024 wide (top at z 0), a hurt slab and sky far below:
// space-elevator's shape. A player standing on it has origin z 24.
static void space_map(void)
{
	world_clear();
	add_box(-512, -512, -64, 512, 512, 0, K_SOLID);
	add_box(-4000, -4000, -1700, 4000, 4000, -1500, K_HURT);
	add_box(-4000, -4000, -2000, 4000, 4000, -1900, K_SKY);
}

static int predict(const float org[3], const float vel[3], tr_fall_t *f)
{
	tr_world_t w = world();
	return TouchRescue_Predict(&w, org, vel, f);
}

static void test_predict(void)
{
	float org[3], vel[3];
	tr_fall_t f;
	tr_world_t w;

	space_map();
	// a standing jump on the platform comes back down on it
	v3(org, 0, 0, 24);
	v3(vel, 0, 0, 270);
	CHECK_EQ(TR_LAND, predict(org, vel, &f), "standing jump lands on the platform");
	CHECK(fabsf(f.point[2] - 24) < 2, "standing jump: on the top");
	// the same jump, descending, running toward the middle
	v3(org, 300, 0, 60);
	v3(vel, -320, 0, -150);
	CHECK_EQ(TR_LAND, predict(org, vel, &f), "jump toward the middle lands");
	// landing: the origin estimated a unit into the floor, still moving down
	v3(org, 0, 0, 23);
	v3(vel, 0, 0, -250);
	CHECK_EQ(TR_LAND, predict(org, vel, &f), "landing frame, origin a unit into the floor");
	v3(org, 500, 0, 20);
	CHECK_EQ(TR_LAND, predict(org, vel, &f), "landing at the edge, 4 units into the floor");
	// a running jump near the edge that still comes down on the platform
	v3(org, 380, 0, 40);
	v3(vel, 320, 0, -50);
	CHECK_EQ(TR_LAND, predict(org, vel, &f), "jump that lands before the edge");
	// walked off the edge: the hurt slab first
	v3(org, 540, 0, 20);
	v3(vel, 320, 0, -100);
	CHECK_EQ(TR_DEATH, predict(org, vel, &f), "walk-off falls into the hurt slab");
	CHECK(f.point[2] < -1400 && f.point[2] > -1800, "walk-off: stopped at the slab");
	// pushed off sideways
	v3(org, 0, 560, -40);
	v3(vel, 0, 450, -200);
	CHECK_EQ(TR_DEATH, predict(org, vel, &f), "pushed off the side");
	// without the slab the sky kills
	world_clear();
	add_box(-512, -512, -64, 512, 512, 0, K_SOLID);
	add_box(-4000, -4000, -2000, 4000, 4000, -1900, K_SKY);
	v3(org, 540, 0, 20);
	v3(vel, 320, 0, -100);
	CHECK_EQ(TR_DEATH, predict(org, vel, &f), "walk-off onto sky");
	// with nothing at all below: the void
	world_clear();
	add_box(-512, -512, -64, 512, 512, 0, K_SOLID);
	CHECK_EQ(TR_VOID, predict(org, vel, &f), "walk-off into nothing");

	// a drop to a lower floor beside the platform is a landing
	space_map();
	add_box(512, -512, -464, 1400, 512, -400, K_SOLID);
	v3(org, 540, 0, 20);
	v3(vel, 320, 0, -100);
	CHECK_EQ(TR_LAND, predict(org, vel, &f), "drop to a lower floor");
	CHECK(fabsf(f.point[2] - (-400 + 24)) < 2, "drop: on the lower floor");
	// a far lower floor, 1300 below: still ground (fall damage, not death)
	space_map();
	add_box(512, -512, -1364, 2400, 512, -1300, K_SOLID);
	CHECK_EQ(TR_LAND, predict(org, vel, &f), "long drop to a floor far below");

	// a jump pad arc across a 900-unit gap onto a second platform: judged
	// from every point of its descent, it lands
	space_map();
	add_box(1412, -512, -64, 2436, 512, 0, K_SOLID);
	{
		float p[3], v[3];
		int ok = 1, step;
		v3(p, 400, 0, 24);
		v3(v, 700, 0, 800); // up 800/s, across 700/s: 2 s of flight, lands about 1400 on
		for (step = 0; step < 40; step++)
		{
			float dt = 0.05f;
			p[0] += v[0] * dt;
			p[2] += v[2] * dt - 0.5f * 800 * dt * dt;
			v[2] -= 800 * dt;
			if (v[2] < -TR_MIN_FALL_SPEED && p[2] > 30 && predict(p, v, &f) != TR_LAND)
			{
				printf("  jump pad arc judged %s at x %.0f z %.0f vz %.0f\n", tr_outcome_name[f.outcome], p[0], p[2], v[2]);
				ok = 0;
			}
		}
		CHECK(ok, "jump pad arc lands on the far platform all the way down");
	}
	// the same arc too short for the gap is a death
	{
		float p[3], v[3];
		v3(p, 700, 0, 200);
		v3(v, 350, 0, -100);
		CHECK_EQ(TR_DEATH, predict(p, v, &f), "jump pad arc that falls short");
	}

	// water below the edge: a landing; lava: a death
	space_map();
	add_box(512, -512, -900, 1400, 512, -600, K_WATER);
	v3(org, 540, 0, 20);
	v3(vel, 320, 0, -100);
	CHECK_EQ(TR_LAND, predict(org, vel, &f), "water below is a landing");
	space_map();
	add_box(512, -512, -900, 1400, 512, -600, K_LAVA);
	CHECK_EQ(TR_DEATH, predict(org, vel, &f), "lava below is a death");

	// falling along a wall that ends above ground: slide down to it
	world_clear();
	add_box(600, -512, -400, 700, 512, 600, K_SOLID);   // a wall
	add_box(-512, -512, -464, 1200, 512, -400, K_SOLID); // the floor under it all
	v3(org, 500, 0, 300);
	v3(vel, 400, 0, -100);
	CHECK_EQ(TR_LAND, predict(org, vel, &f), "wall slide onto the floor");
	// a steep ramp into the void is not ground
	world_clear();
	add_box(-512, -512, -64, 512, 512, 0, K_SOLID);
	add_box(-4000, -4000, -2000, 4000, 4000, -1900, K_SKY);
	w = world();
	{
		tr_fall_t b;
		v3(org, 0, 0, 100);
		CHECK_EQ(TR_LAND, TouchRescue_Below(&w, org, 192, &b), "ground within a short drop");
		v3(org, 0, 0, 400);
		CHECK_EQ(TR_VOID, TouchRescue_Below(&w, org, 192, &b), "no ground within a short drop above a high point");
		v3(org, 700, 0, 100);
		CHECK_EQ(TR_DEATH, TouchRescue_Below(&w, org, 0, &b), "beside the platform: sky all the way down");
	}

	// a landing on the lip, still moving outward, is not one to let go on
	space_map();
	w = world();
	v3(org, 470, 0, 120);
	v3(vel, 250, 0, -100);
	CHECK_EQ(TR_LAND, TouchRescue_Predict(&w, org, vel, &f), "lip: the arc touches the platform");
	CHECK(!TouchRescue_LandsClear(&w, &f, vel), "lip: not a clear landing while heading out");
	v3(vel, -250, 0, -100);
	TouchRescue_Predict(&w, org, vel, &f);
	CHECK(TouchRescue_LandsClear(&w, &f, vel), "lip: a clear landing heading in");

	// the cost: a fall judged in a few traces
	space_map();
	w = world();
	v3(org, 540, 0, 20);
	v3(vel, 320, 0, -100);
	TouchRescue_Predict(&w, org, vel, &f);
	printf("  a walk-off judged in %d traces\n", w.traces);
	CHECK(w.traces <= TR_MAX_SEGS + TR_MAX_SLIDES + 4, "prediction stays within its trace budget");
}

static tr_from_t from_eye(const float eye[3], float vx, float vz, float home_z)
{
	tr_from_t f;
	f.org[0] = eye[0];
	f.org[1] = eye[1];
	f.org[2] = eye[2] - 35;
	f.vel[0] = vx;
	f.vel[1] = 0;
	f.vel[2] = vz;
	f.eye_h = 35;
	f.home_z = home_z;
	return f;
}

static void test_anchor(void)
{
	float eye[3], home[3];
	tr_anchor_t a;
	tr_world_t w;
	tr_from_t from;

	// the platform with a tall pillar near the edge: a player who has just
	// walked off (where the rescue starts: 20 below the edge, falling at
	// 160) catches the pillar, and the pull ends over the platform
	v3(home, 450, 0, 24);
	space_map();
	add_box(300, -64, 0, 400, 64, 600, K_SOLID);
	w = world();
	v3(eye, 560, 0, 39);
	from = from_eye(eye, 300, -160, 24);
	CHECK(TouchRescue_FindAnchor(&w, &from, eye, home, 180, 1600, &a), "pillar: an anchor");
	CHECK(a.over_ground, "pillar: the pull ends over the platform");
	CHECK(a.point[0] > 390 && a.point[0] < 410, "pillar: on the pillar's face");
	printf("  pillar anchor at %.0f %.0f %.0f (%.0f units, %d traces)\n", a.point[0], a.point[1], a.point[2], a.dist, w.traces);

	// a low wall at the far side: by the time a hook gets there the player
	// is well below the edge, and the line to it runs into the lip: the
	// player would wedge under it, so it is not taken
	space_map();
	add_box(-512, -512, 0, -400, 512, 160, K_SOLID);
	w = world();
	v3(eye, 560, 0, 39);
	from = from_eye(eye, 300, -160, 24);
	CHECK(!TouchRescue_FindAnchor(&w, &from, eye, home, 180, 1600, &a), "low far wall: the pull would wedge under the lip");

	// a tall wall across the platform, 300 in from the edge: its high part
	// pulls the player clear over the lip
	space_map();
	add_box(150, -512, 0, 210, 512, 2000, K_SOLID);
	w = world();
	v3(eye, 560, 0, 39);
	from = from_eye(eye, 300, -160, 24);
	CHECK(TouchRescue_FindAnchor(&w, &from, eye, home, 180, 1600, &a), "tall wall: an anchor");
	CHECK(a.point[0] > 209 && a.point[0] < 211 && a.point[2] > 24 + TR_ANCHOR_ABOVE_HOME, "tall wall: on the wall, high over the ledge");
	printf("  tall wall anchor at %.0f %.0f %.0f (%s, %d traces)\n", a.point[0], a.point[1], a.point[2], a.over_ground ? "over ground" : "above home", w.traces);

	// only the platform's underside in reach: nothing that pulls back up
	space_map();
	w = world();
	v3(eye, 560, 0, -300);
	from = from_eye(eye, 100, -300, 24);
	CHECK(!TouchRescue_FindAnchor(&w, &from, eye, home, 180, 1600, &a), "underside only: nothing that pulls back up");

	// a ceiling of sky above: the hook would be dropped, never an anchor
	world_clear();
	add_box(-512, -512, -64, 512, 512, 0, K_SOLID);
	add_box(-4000, -4000, 900, 4000, 4000, 1000, K_SKY);
	w = world();
	v3(eye, 650, 0, -150);
	from = from_eye(eye, 100, -300, 24);
	CHECK(!TouchRescue_FindAnchor(&w, &from, eye, home, 180, 1600, &a), "sky above: no anchor");

	// the search keeps to its budget, whatever the world
	space_map();
	add_box(300, -64, 0, 400, 64, 600, K_SOLID);
	add_box(-300, -400, 0, -200, 400, 900, K_SOLID);
	add_box(-100, 300, 0, 300, 400, 700, K_SOLID);
	w = world();
	v3(eye, 560, 0, 39);
	from = from_eye(eye, 300, -160, 24);
	TouchRescue_FindAnchor(&w, &from, eye, home, 180, 1600, &a);
	printf("  a cluttered search took %d traces\n", w.traces);
	CHECK(w.traces <= TR_ANCHOR_BUDGET + 40, "anchor search stays within its budget");

	// out of reach: nothing
	space_map();
	add_box(300, -64, 0, 400, 64, 600, K_SOLID);
	w = world();
	v3(eye, 560, 0, 39);
	from = from_eye(eye, 300, -160, 24);
	CHECK(!TouchRescue_FindAnchor(&w, &from, eye, home, 180, 200, &a), "pillar out of reach");
}

// ---------------------------------------------------------------------------
// The state machine, frame by frame.
// ---------------------------------------------------------------------------
static tr_frame_t falling(double now)
{
	tr_frame_t f;
	memset(&f, 0, sizeof(f));
	f.now = now;
	f.allowed = 1;
	f.available = 1;
	f.vz = -300;
	f.fall = TR_VOID;
	f.anchor = 1;
	f.aim_err = 40;
	f.attach_time = 0.6f;
	f.ground_below = -1;
	f.speed = 400;
	f.hook_out = 1;
	f.cooldown = 1;
	return f;
}

// Steps until the phase changes or `frames` pass; returns the frames taken.
static int run(tr_state_t *s, tr_frame_t *f, int frames, tr_out_t *o)
{
	int i, start = s->phase;
	for (i = 0; i < frames; i++)
	{
		TouchRescue_Step(s, f, o);
		f->now += 1.0 / 60;
		if (s->phase != start)
			return i + 1;
	}
	return frames;
}

static void test_rescue(void)
{
	tr_state_t s;
	tr_out_t o;
	tr_frame_t f;
	int n, held;

	// the whole rescue
	TouchRescue_Reset(&s);
	f = falling(10);
	n = run(&s, &f, 30, &o);
	CHECK_EQ(TR_AIM, s.phase, "void: aims");
	CHECK(n >= 3 && n <= 6, "void: after the confirm time, not at once");
	CHECK(o.find_anchor, "void: looks for an anchor");
	n = run(&s, &f, 5, &o);
	CHECK_EQ(TR_AIM, s.phase, "aim: keeps turning while off the anchor");
	CHECK(o.aim && !o.hold, "aim: turns, does not fire yet");
	f.aim_err = 1;
	run(&s, &f, 1, &o);
	CHECK_EQ(TR_HOLD, s.phase, "aim: fires on the anchor");
	CHECK(o.hold, "aim: hook held from the shot");
	f.pull_speed = 900;
	f.vz = 400;
	f.fall = TR_VOID;
	held = run(&s, &f, 20, &o);
	CHECK_EQ(TR_HOLD, s.phase, "hold: keeps holding while still over the void");
	CHECK(o.hold && held == 20, "hold: held every frame");
	f.ground_below = 120;
	f.fall = TR_LAND;
	run(&s, &f, 1, &o);
	CHECK_EQ(TR_RESTORE, s.phase, "hold: lets go over ground");
	CHECK(!o.hold, "hold: released");
	f.aim_err = 30;
	f.turn_rate = 0;
	run(&s, &f, 3, &o);
	CHECK(o.restore, "restore: turns the view back");
	f.aim_err = 0.2f;
	run(&s, &f, 1, &o);
	CHECK_EQ(TR_COOLDOWN, s.phase, "restore: done when the view is back");
	f.fall = TR_VOID;
	f.onground = 0;
	n = run(&s, &f, 50, &o);
	CHECK_EQ(TR_COOLDOWN, s.phase, "cooldown: no new rescue within it");
	n = run(&s, &f, 30, &o);
	CHECK_EQ(TR_IDLE, s.phase, "cooldown ends");
	// standing on ground ends the cooldown early: a slide off the ledge can
	// be caught again
	TouchRescue_Reset(&s);
	f = falling(200);
	run(&s, &f, 30, &o);
	f.anchor = 0;
	run(&s, &f, 1, &o);
	CHECK_EQ(TR_COOLDOWN, s.phase, "ground reset: in the cooldown");
	f.onground = 1;
	n = run(&s, &f, 60, &o);
	CHECK_EQ(TR_IDLE, s.phase, "ground reset: ready again after standing");
	CHECK(n >= 12 && n <= 14, "ground reset: after TR_GROUND_RESET on ground");

	// ground ahead: never
	TouchRescue_Reset(&s);
	f = falling(10);
	f.fall = TR_LAND;
	run(&s, &f, 120, &o);
	CHECK_EQ(TR_IDLE, s.phase, "a fall onto ground never triggers");
	// rising (a jump, a jump pad): never, whatever is below
	f.fall = TR_VOID;
	f.vz = 400;
	run(&s, &f, 120, &o);
	CHECK_EQ(TR_IDLE, s.phase, "rising never triggers");
	// on the ground: never
	f.vz = -300;
	f.onground = 1;
	run(&s, &f, 120, &o);
	CHECK_EQ(TR_IDLE, s.phase, "on the ground never triggers");
	// hooking by hand: never
	f.onground = 0;
	f.manual = 1;
	run(&s, &f, 120, &o);
	CHECK_EQ(TR_IDLE, s.phase, "hooking by hand: never");
	// no hook: silent
	f.manual = 0;
	f.available = 0;
	run(&s, &f, 120, &o);
	CHECK_EQ(TR_IDLE, s.phase, "no hook: nothing");
	// keyboard, menu, dead: nothing
	f.available = 1;
	f.allowed = 0;
	run(&s, &f, 120, &o);
	CHECK_EQ(TR_IDLE, s.phase, "not allowed: nothing");
	// one stray frame of void between ground frames does not trigger
	f.allowed = 1;
	{
		int i, triggered = 0;
		for (i = 0; i < 120; i++)
		{
			f.fall = (i % 3 == 0) ? TR_VOID : TR_LAND;
			TouchRescue_Step(&s, &f, &o);
			f.now += 1.0 / 60;
			triggered |= s.phase != TR_IDLE;
		}
		CHECK(!triggered, "flickering prediction does not trigger");
	}

	// the player turns the view hard while it aims: cancelled
	TouchRescue_Reset(&s);
	f = falling(20);
	run(&s, &f, 30, &o);
	f.turn_rate = 300;
	run(&s, &f, 1, &o);
	CHECK_EQ(TR_COOLDOWN, s.phase, "a hard turn while aiming cancels");
	// a small drift of the thumb does not
	TouchRescue_Reset(&s);
	f = falling(30);
	run(&s, &f, 30, &o);
	f.turn_rate = 30;
	run(&s, &f, 5, &o);
	CHECK_EQ(TR_AIM, s.phase, "a slow drift keeps aiming");

	// the player presses HOOK while held: theirs at once
	TouchRescue_Reset(&s);
	f = falling(40);
	run(&s, &f, 30, &o);
	f.aim_err = 0;
	run(&s, &f, 1, &o);
	f.manual = 1;
	run(&s, &f, 1, &o);
	CHECK_EQ(TR_COOLDOWN, s.phase, "HOOK pressed: rescue steps aside");
	CHECK(!o.hold, "HOOK pressed: no hold from the rescue");

	// the hook does not catch: let go after the attach time
	TouchRescue_Reset(&s);
	f = falling(50);
	run(&s, &f, 30, &o);
	f.aim_err = 0;
	run(&s, &f, 1, &o);
	f.pull_speed = -300;
	n = run(&s, &f, 120, &o);
	CHECK_EQ(TR_RESTORE, s.phase, "no catch: released");
	CHECK(n >= 34 && n <= 38, "no catch: after the attach time");

	// no release over ground before the hook caught (a ledge passing under
	// the falling player while the hook flies)
	TouchRescue_Reset(&s);
	f = falling(60);
	run(&s, &f, 30, &o);
	f.aim_err = 0;
	run(&s, &f, 1, &o);
	f.ground_below = 100;
	f.fall = TR_LAND;
	run(&s, &f, 10, &o);
	CHECK_EQ(TR_HOLD, s.phase, "not released before the hook caught");

	// landing during the hold releases; dying ends it without a restore
	f.onground = 1;
	run(&s, &f, 1, &o);
	CHECK_EQ(TR_RESTORE, s.phase, "landed: released");
	TouchRescue_Reset(&s);
	f = falling(70);
	run(&s, &f, 30, &o);
	f.aim_err = 0;
	run(&s, &f, 1, &o);
	f.allowed = 0;
	run(&s, &f, 1, &o);
	CHECK_EQ(TR_COOLDOWN, s.phase, "died while held: over");

	// a stuck hang is let go after TR_MAX_HOLD
	TouchRescue_Reset(&s);
	f = falling(80);
	run(&s, &f, 30, &o);
	f.aim_err = 0;
	run(&s, &f, 1, &o);
	f.pull_speed = 500;
	n = run(&s, &f, 400, &o);
	CHECK_EQ(TR_RESTORE, s.phase, "a hang is let go");
	CHECK(n >= 235 && n <= 245, "a hang lasts TR_MAX_HOLD");

	// the Hook weapon: ready while aiming, the view kept on the anchor until
	// the switch lands and the hook leaves; the catch is timed from then
	TouchRescue_Reset(&s);
	f = falling(84);
	run(&s, &f, 30, &o);
	CHECK(o.prepare, "weapon: switched to while aiming");
	f.aim_err = 0;
	f.hook_out = 0;
	run(&s, &f, 1, &o);
	CHECK_EQ(TR_HOLD, s.phase, "weapon: decided");
	f.pull_speed = -300;
	run(&s, &f, 30, &o);
	CHECK_EQ(TR_HOLD, s.phase, "weapon: still waiting on the switch past the attach time");
	CHECK(o.hold && o.aim, "weapon: holding, view kept on the anchor");
	f.hook_out = 1;
	run(&s, &f, 1, &o);
	CHECK(!o.aim, "weapon: hook out, the view is left alone");
	n = run(&s, &f, 120, &o);
	CHECK_EQ(TR_RESTORE, s.phase, "weapon: no catch, released");
	CHECK(n >= 34 && n <= 38, "weapon: catch timed from the hook leaving");
	// a switch that never lands
	TouchRescue_Reset(&s);
	f = falling(87);
	run(&s, &f, 30, &o);
	f.aim_err = 0;
	f.hook_out = 0;
	run(&s, &f, 1, &o);
	n = run(&s, &f, 120, &o);
	CHECK_EQ(TR_RESTORE, s.phase, "weapon: never fired, given up");
	CHECK(n >= 70 && n <= 74, "weapon: after TR_SWITCH_TIMEOUT");

	// caught, then wedged motionless under a lip: let go after TR_STUCK_TIME
	TouchRescue_Reset(&s);
	f = falling(85);
	run(&s, &f, 30, &o);
	f.aim_err = 0;
	run(&s, &f, 1, &o);
	f.pull_speed = 500;
	run(&s, &f, 20, &o);
	f.speed = 10;
	n = run(&s, &f, 120, &o);
	CHECK_EQ(TR_RESTORE, s.phase, "wedged: let go");
	CHECK(n >= 60 && n <= 63, "wedged: after TR_STUCK_TIME");
	// slow at the top of a swing, over ground on the way: not stuck
	TouchRescue_Reset(&s);
	f = falling(86);
	run(&s, &f, 30, &o);
	f.aim_err = 0;
	run(&s, &f, 1, &o);
	f.pull_speed = 500;
	run(&s, &f, 20, &o);
	f.speed = 10;
	run(&s, &f, 40, &o);
	f.speed = 300;
	run(&s, &f, 30, &o);
	CHECK_EQ(TR_HOLD, s.phase, "a slow moment is not stuck");

	// no anchor: gives up at once
	TouchRescue_Reset(&s);
	f = falling(90);
	run(&s, &f, 30, &o);
	f.anchor = 0;
	run(&s, &f, 1, &o);
	CHECK_EQ(TR_COOLDOWN, s.phase, "no anchor: gives up");

	// the player turned the view during the hold: the restore is skipped
	TouchRescue_Reset(&s);
	f = falling(100);
	run(&s, &f, 30, &o);
	f.aim_err = 0;
	run(&s, &f, 1, &o);
	f.pull_speed = 900;
	f.turn_rate = 60;
	run(&s, &f, 2, &o);
	f.turn_rate = 0;
	f.ground_below = 50;
	f.fall = TR_LAND;
	run(&s, &f, 1, &o);
	f.aim_err = 40;
	run(&s, &f, 1, &o);
	CHECK_EQ(TR_COOLDOWN, s.phase, "turned during the hold: view left alone");
	CHECK(!o.restore, "turned during the hold: no restore");
}

int main(void)
{
	test_predict();
	test_anchor();
	test_rescue();
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
