/*
Checks the decisions behind the touch ledge guard (engine/darkplaces/touch_ledge.h)
in a world of boxes: a run off a floating platform is stopped, slowed or
turned along the edge, and the move chosen keeps the player on the ground;
a walk along a narrow bridge, down stairs, into a wall or far from any edge
is left alone; a drop onto a lower floor is let through unless the stricter
mode asks otherwise.

Run through scripts/test-touch-ledge.sh.
*/

#include <math.h>
#include <stdbool.h>
#include <stdio.h>

typedef bool qbool;

#include "touch_ledge.h"

static int failures, checks;

#define CHECK(cond, what) do { \
	checks++; \
	if (!(cond)) { \
		printf("FAIL %s\n", (what)); \
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


// A big floor (top at 0); the player stands on it (origin z 24).
static void arena(void)
{
	world_clear();
	add_box(-4000, -4000, -64, 4000, 4000, 0, K_SOLID);
}

static td_player_t standing(float x, float y)
{
	td_player_t p;
	memset(&p, 0, sizeof(p));
	v3(p.org, x, y, 24);
	v3(p.mins, -16, -16, -24);
	v3(p.maxs, 16, 16, 45);
	p.onground = 1;
	return p;
}

// Xonotic's stock movement (physicsX), the view toward the shooter (-y).
// Every field set: a field left out reads whatever the stack held, and the
// dash is modelled in the air.
static td_move_t xonotic_move(int dodging)
{
	td_move_t m;
	memset(&m, 0, sizeof(m));
	m.fwd[0] = 0;
	m.fwd[1] = -1;
	m.airstrafeaccel = 18;
	m.maxairstrafespeed = 100;
	m.maxairspeed = 360;
	m.maxspeed = 360;
	m.accel = 15;
	m.airaccel = 2;
	m.friction = 6;
	m.stopspeed = 100;
	m.jumpvel = 260;
	m.gravity = 800;
	m.dodging = dodging;
	m.dodge_speed = 400;
	m.dodge_up = 200;
	return m;
}

static const float still[2] = { 0, 0 };
static const tl_opts_t deadly = { TL_MODE_DEADLY, 0, 0.35f, NULL, NULL };

// A jump pad: the box [512..680] x [-64..64] over the void past the edge.
static int pad(void *ctx, const float p[3])
{
	(void)ctx;
	return p[0] + 16 > 512 && p[0] - 16 < 680 && p[1] + 16 > -64 && p[1] - 16 < 64 && p[2] > -100 && p[2] < 200;
}

// Whether a move held for the look-ahead keeps the box over ground.
static int move_safe(tr_world_t *w, const td_player_t *p, const td_move_t *m, const float dir[2], float mag, const tl_opts_t *o)
{
	td_move_t mm = *m;
	td_plan_t plan;
	float at[3];
	memset(&plan, 0, sizeof(plan));
	plan.dir[0] = mag > 0 ? dir[0] : 0;
	plan.dir[1] = mag > 0 ? dir[1] : 0;
	mm.maxspeed = m->maxspeed * (mag > 0 ? mag : 1);
	return tl_path_edge(w, p, &mm, &plan, (int)(o->lookahead / TD_STEP + 0.5f), o, at) == 0;
}

static td_player_t running(float x, float y, float vx, float vy)
{
	td_player_t p = standing(x, y);
	p.vel[0] = vx;
	p.vel[1] = vy;
	return p;
}

static void test_space(void)
{
	tr_world_t w;
	td_player_t p;
	td_move_t m = xonotic_move(0);
	tl_result_t r;
	const float east[2] = { 1, 0 };
	char what[160];

	space_map();
	w = world();

	// in the middle, running east: the edge is 512 units off
	p = running(0, 0, 360, 0);
	TouchLedge_Guard(&w, &p, &m, east, 1, &deadly, &r);
	CHECK(r.verdict == TL_CLEAR, "middle of the platform, running east: left alone");

	// 100 units short of the edge, running straight at it: held back, and
	// what is done instead keeps the player on the platform
	p = running(412, 0, 360, 0);
	TouchLedge_Guard(&w, &p, &m, east, 1, &deadly, &r);
	snprintf(what, sizeof(what), "running at the edge from 100 units: guarded (got %s, edge %s)", tl_verdict_name[r.verdict], tl_edge_name[r.edge]);
	CHECK(r.verdict != TL_CLEAR && r.edge == TL_EDGE_DEATH, what);
	CHECK(r.verdict == TL_BRAKE || move_safe(&w, &p, &m, r.dir, r.mag, &deadly), "the move chosen stays on the platform");

	// further back, walking: slowed rather than stopped, still safe
	p = running(380, 0, 180, 0);
	TouchLedge_Guard(&w, &p, &m, east, 0.5f, &deadly, &r);
	snprintf(what, sizeof(what), "walking at the edge from 132 units: slowed or stopped and safe (got %s mag %.2f)", tl_verdict_name[r.verdict], r.mag);
	CHECK((r.verdict == TL_SLOW || r.verdict == TL_STOP || r.verdict == TL_CLEAR) && move_safe(&w, &p, &m, r.dir, r.mag, &deadly), what);

	// angled at the edge (30 degrees off it, running north-east): turned along it
	{
		const float ne[2] = { 0.5f, 0.8660254f };
		p = running(480, 0, 180, 312);
		TouchLedge_Guard(&w, &p, &m, ne, 1, &deadly, &r);
		snprintf(what, sizeof(what), "angled at the edge: turned along it (got %s dir %.2f %.2f)", tl_verdict_name[r.verdict], r.dir[0], r.dir[1]);
		CHECK(r.verdict == TL_STEER && r.dir[1] > 0.9f && move_safe(&w, &p, &m, r.dir, r.mag, &deadly), what);
	}

	// sliding fast at the edge with the stick let go: braked against the slide
	p = running(440, 0, 700, 0);
	TouchLedge_Guard(&w, &p, &m, still, 0, &deadly, &r);
	snprintf(what, sizeof(what), "sliding at the edge, no input: braked (got %s dir %.2f)", tl_verdict_name[r.verdict], r.dir[0]);
	CHECK(r.verdict == TL_BRAKE && r.dir[0] < -0.99f, what);

	// running away from the edge: left alone
	{
		const float west[2] = { -1, 0 };
		p = running(480, 0, -360, 0);
		TouchLedge_Guard(&w, &p, &m, west, 1, &deadly, &r);
		CHECK(r.verdict == TL_CLEAR, "at the edge, running away from it: left alone");
	}

	// a jump pad hanging over the void just past the edge: walking onto it is
	// left alone; beside it the edge still holds
	{
		tl_opts_t withpad = deadly;
		withpad.launch = pad;
		p = running(440, 0, 360, 0);
		TouchLedge_Guard(&w, &p, &m, east, 1, &withpad, &r);
		CHECK(r.verdict == TL_CLEAR, "onto a jump pad over the void: left alone");
		p = running(440, 300, 360, 0);
		TouchLedge_Guard(&w, &p, &m, east, 1, &withpad, &r);
		CHECK(r.verdict != TL_CLEAR, "beside the jump pad: still guarded");
	}

	// in the air: never touched (a jump across a gap)
	p = running(480, 0, 360, 0);
	p.onground = 0;
	TouchLedge_Guard(&w, &p, &m, east, 1, &deadly, &r);
	CHECK(r.verdict == TL_CLEAR, "in the air: left alone");

	// off: left alone
	{
		tl_opts_t off = deadly;
		off.mode = TL_MODE_OFF;
		p = running(480, 0, 360, 0);
		TouchLedge_Guard(&w, &p, &m, east, 1, &off, &r);
		CHECK(r.verdict == TL_CLEAR, "mode off: left alone");
	}
}

static void test_shapes(void)
{
	tr_world_t w;
	td_player_t p;
	td_move_t m = xonotic_move(0);
	tl_result_t r;
	const float east[2] = { 1, 0 };
	tl_opts_t drops = { TL_MODE_DROPS, 64, 0.35f, NULL, NULL };
	char what[160];
	int i;

	// a 48-wide bridge between two platforms over the void: walking along
	// its middle the box always stands on it
	world_clear();
	add_box(-512, -512, -64, 0, 512, 0, K_SOLID);
	add_box(0, -24, -64, 1024, 24, 0, K_SOLID);
	add_box(1024, -512, -64, 1536, 512, 0, K_SOLID);
	w = world();
	p = running(100, 0, 360, 0);
	TouchLedge_Guard(&w, &p, &m, east, 1, &deadly, &r);
	snprintf(what, sizeof(what), "along a narrow bridge: left alone (got %s)", tl_verdict_name[r.verdict]);
	CHECK(r.verdict == TL_CLEAR, what);
	// stepping sideways off it is caught
	{
		const float north[2] = { 0, 1 };
		p = running(500, 0, 0, 200);
		TouchLedge_Guard(&w, &p, &m, north, 1, &deadly, &r);
		CHECK(r.verdict != TL_CLEAR, "sideways off the bridge: guarded");
	}

	// a floor 128 below past the edge: the default mode lets the player drop
	// onto it, the strict one with a 64-unit limit does not
	world_clear();
	add_box(-512, -512, -64, 512, 512, 0, K_SOLID);
	add_box(512, -512, -192, 1536, 512, -128, K_SOLID);
	w = world();
	p = running(440, 0, 360, 0);
	TouchLedge_Guard(&w, &p, &m, east, 1, &deadly, &r);
	CHECK(r.verdict == TL_CLEAR, "a lower floor past the edge: let through by default");
	TouchLedge_Guard(&w, &p, &m, east, 1, &drops, &r);
	snprintf(what, sizeof(what), "a lower floor past the edge, strict mode: guarded (got %s, edge %s)", tl_verdict_name[r.verdict], tl_edge_name[r.edge]);
	CHECK(r.verdict != TL_CLEAR && r.edge == TL_EDGE_DROP, what);

	// stairs down, 16 a step: never a drop, even in the strict mode
	world_clear();
	add_box(-512, -512, -64, 400, 512, 0, K_SOLID);
	for (i = 0; i < 12; i++)
		add_box(400 + i * 32, -512, -64 - 16 * (i + 1), 432 + i * 32, 512, -16 * (i + 1), K_SOLID);
	add_box(784, -512, -400, 2000, 512, -192, K_SOLID);
	w = world();
	p = running(360, 0, 360, 0);
	TouchLedge_Guard(&w, &p, &m, east, 1, &drops, &r);
	snprintf(what, sizeof(what), "down a stair, strict mode: left alone (got %s, edge %s)", tl_verdict_name[r.verdict], tl_edge_name[r.edge]);
	CHECK(r.verdict == TL_CLEAR, what);

	// a wall at the platform's edge: the player stops against it
	world_clear();
	add_box(-512, -512, -64, 512, 512, 0, K_SOLID);
	add_box(480, -512, 0, 512, 512, 256, K_SOLID);
	w = world();
	p = running(420, 0, 360, 0);
	TouchLedge_Guard(&w, &p, &m, east, 1, &deadly, &r);
	CHECK(r.verdict == TL_CLEAR, "a wall at the edge: left alone");

	// lava past the edge, a short drop down: a death zone
	world_clear();
	add_box(-512, -512, -64, 512, 512, 0, K_SOLID);
	add_box(512, -512, -200, 1536, 512, -40, K_LAVA);
	add_box(-2000, -2000, -400, 2000, 2000, -200, K_SOLID);
	w = world();
	p = running(440, 0, 360, 0);
	TouchLedge_Guard(&w, &p, &m, east, 1, &deadly, &r);
	snprintf(what, sizeof(what), "lava past the edge: guarded (got %s, edge %s)", tl_verdict_name[r.verdict], tl_edge_name[r.edge]);
	CHECK(r.verdict != TL_CLEAR && r.edge == TL_EDGE_DEATH, what);

	// a pit in a big floor (trigger_hurt at its bottom): guarded
	world_clear();
	add_box(-2000, -2000, -64, 300, 2000, 0, K_SOLID);
	add_box(500, -2000, -64, 2000, 2000, 0, K_SOLID);
	add_box(300, -2000, -600, 500, 2000, -500, K_HURT);
	add_box(-2000, -2000, -800, 2000, 2000, -700, K_SOLID);
	w = world();
	p = running(200, 0, 360, 0);
	TouchLedge_Guard(&w, &p, &m, east, 1, &deadly, &r);
	CHECK(r.verdict != TL_CLEAR && r.edge == TL_EDGE_DEATH, "a hurt pit ahead: guarded");
}

// A walk at the edge kept up frame after frame (the engine reruns the guard
// every frame): the player creeps up but never falls off.
static void test_hold(void)
{
	tr_world_t w;
	td_player_t p;
	td_move_t m = xonotic_move(0);
	tl_result_t r;
	const float east[2] = { 1, 0 };
	float maxx = -1e9f;
	int f;
	char what[160];
	space_map();
	w = world();
	p = running(300, 0, 360, 0);
	for (f = 0; f < 200; f++)
	{
		td_plan_t plan;
		td_move_t mm = m;
		float path[1][3];
		TouchLedge_Guard(&w, &p, &m, east, 1, &deadly, &r);
		memset(&plan, 0, sizeof(plan));
		plan.dir[0] = r.mag > 0 ? r.dir[0] : 0;
		plan.dir[1] = r.mag > 0 ? r.dir[1] : 0;
		mm.maxspeed = m.maxspeed * (r.mag > 0 ? r.mag : 1);
		// one engine frame (TD_STEP), the velocity from the step taken
		td_player_path(&p, &mm, &plan, 1, path);
		p.vel[0] = (path[0][0] - p.org[0]) / TD_STEP;
		p.vel[1] = (path[0][1] - p.org[1]) / TD_STEP;
		p.org[0] = path[0][0];
		p.org[1] = path[0][1];
		if (p.org[0] > maxx)
			maxx = p.org[0];
	}
	// the box (16 a side) stays over the platform (edge at 512)
	snprintf(what, sizeof(what), "held stick at the edge for 5 s: stays on (farthest x %.0f), gets close (> 440)", maxx);
	CHECK(maxx < 512 + 16 && maxx > 440, what);
}

static td_player_t flying(float x, float z, float vx, float vz)
{
	td_player_t p = standing(x, 0);
	p.org[2] = z;
	p.vel[0] = vx;
	p.vel[2] = vz;
	p.onground = 0;
	return p;
}

static void test_air(void)
{
	tr_world_t w;
	td_player_t p;
	td_move_t m = xonotic_move(0);
	tl_result_t r;
	const float east[2] = { 1, 0 }, west[2] = { -1, 0 };
	char what[200];

	m.fwd[0] = 1;
	m.fwd[1] = 0;
	space_map();
	w = world();

	// stepped off a ramp 100 units up, sliding east at 300, the stick still
	// pushed east: that comes down in the void; a brake comes down on the platform
	p = flying(400, 124, 300, 0);
	TouchLedge_GuardAir(&w, &p, &m, east, 1, &deadly, &r);
	snprintf(what, sizeof(what), "in the air, carried past the edge: braked (got %s dir %.2f, %d moves)", tl_verdict_name[r.verdict], r.dir[0], r.tried);
	CHECK(r.verdict == TL_BRAKE && r.dir[0] < -0.99f, what);

	// a jump that comes down on the platform: left alone
	p = flying(0, 25, 300, 260);
	TouchLedge_GuardAir(&w, &p, &m, east, 1, &deadly, &r);
	CHECK(r.verdict == TL_CLEAR, "a jump that lands: left alone");

	// already past the edge, nothing brings them back: left alone
	p = flying(650, 24, 300, 0);
	TouchLedge_GuardAir(&w, &p, &m, east, 1, &deadly, &r);
	CHECK(r.verdict == TL_CLEAR, "past saving: the player's own input stands");

	// on the ground the air guard does nothing
	p = running(480, 0, 360, 0);
	TouchLedge_GuardAir(&w, &p, &m, east, 1, &deadly, &r);
	CHECK(r.verdict == TL_CLEAR, "air guard on the ground: nothing");

	// a launch across a gap toward a second platform that lands with the
	// stick let go; the thumb pulling back would drop them in the gap
	world_clear();
	add_box(-512, -512, -64, 512, 512, 0, K_SOLID);
	add_box(800, -512, -64, 1800, 512, 0, K_SOLID);
	add_box(-4000, -4000, -1700, 4000, 4000, -1500, K_HURT);
	w = world();
	p = flying(400, 25, 500, 350);
	TouchLedge_GuardAir(&w, &p, &m, west, 1, &deadly, &r);
	snprintf(what, sizeof(what), "launched across a gap, pulling back: let go of (got %s mag %.2f)", tl_verdict_name[r.verdict], r.mag);
	CHECK((r.verdict == TL_STOP || r.verdict == TL_STEER) && r.edge == TR_DEATH, what);
	TouchLedge_GuardAir(&w, &p, &m, east, 1, &deadly, &r);
	CHECK(r.verdict == TL_CLEAR, "launched across a gap, pushing on: left alone");
}

int main(void)
{
	test_space();
	test_shapes();
	test_hold();
	test_air();
	printf("touch ledge: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
