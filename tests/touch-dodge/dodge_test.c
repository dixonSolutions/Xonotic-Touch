/*
Checks the decisions behind touch auto dodge (engine/darkplaces/touch_dodge.h)
in a world of boxes: the damage a projectile does if the player carries on, and
the escape chosen -- away from a direct hit and out of a blast, never into a
wall or off a ledge, nothing for a shot that misses or flies away.

Run through scripts/test-touch-dodge.sh.
*/

#include <math.h>
#include <stdbool.h>
#include <stdio.h>

typedef bool qbool;

#include "touch_dodge.h"

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

static td_move_t xonotic_move(int dodging)
{
	td_move_t m;
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

static td_proj_t shot(int kind, float x, float y, float z, float vx, float vy, float vz, float gravity)
{
	td_proj_t p;
	const td_kind_t *k = TouchDodge_Kind(kind);
	memset(&p, 0, sizeof(p));
	v3(p.org, x, y, z);
	v3(p.vel, vx, vy, vz);
	p.gravity = gravity;
	p.damage = k->damage;
	p.edge = k->edge;
	p.radius = k->radius;
	p.size = k->size;
	p.world_blast = k->world_blast;
	return p;
}

static const float still[2] = { 0, 0 };

static void test_choices(void)
{
	tr_world_t w;
	td_player_t p;
	td_move_t m = xonotic_move(0);
	td_proj_t r;
	td_choice_t c;
	int go;

	// a rocket at the chest from 800 units (0.6 s at 1300 u/s): a direct hit
	// if the player stands; a sidestep clears the box and nothing behind
	// takes the blast
	arena();
	w = world();
	p = standing(0, 0);
	r = shot(TD_KIND_ROCKET, 0, -800, 40, 0, 1300, 0, 0);
	go = TouchDodge_Choose(&w, &r, &p, &m, still, 1.0f, 10, &c);
	printf("  rocket at the chest from 800: carry on %.0f, %s -> %.0f\n", c.base_damage, c.chosen.name, c.damage);
	CHECK(c.base_damage >= 79, "rocket at the chest: a direct hit standing still");
	CHECK(go && c.damage < c.base_damage * 0.5f, "rocket at the chest: an escape halves it at least");
	CHECK(c.chosen.dir[1] == 0 || fabsf(c.chosen.dir[0]) > 0.5f, "rocket at the chest: sideways, not along the shot");

	// the same rocket from 300 units (0.23 s): too close to clear the blast, still better
	r = shot(TD_KIND_ROCKET, 0, -300, 40, 0, 1300, 0, 0);
	go = TouchDodge_Choose(&w, &r, &p, &m, still, 1.0f, 10, &c);
	printf("  rocket at the chest from 300: carry on %.0f, %s -> %.0f\n", c.base_damage, c.chosen.name, c.damage);
	CHECK(go && c.damage < c.base_damage, "rocket from 300: an escape still helps");

	// a rocket at the feet: it lands 60 units in front, a splash, not a hit
	{
		float dy = 740, dz = -40, l = sqrtf(dy * dy + dz * dz);
		r = shot(TD_KIND_ROCKET, 0, -800, 40, 0, 1300 * dy / l, 1300 * dz / l, 0);
	}
	go = TouchDodge_Choose(&w, &r, &p, &m, still, 1.0f, 10, &c);
	printf("  rocket at the feet: carry on %.0f, %s -> %.0f\n", c.base_damage, c.chosen.name, c.damage);
	CHECK(c.base_damage > 30 && c.base_damage < 79, "rocket at the feet: a splash, not a hit, standing still");
	CHECK(go && c.damage < c.base_damage - 10, "rocket at the feet: an escape cuts it");

	// a rocket that passes 250 units to the side: nothing to do
	r = shot(TD_KIND_ROCKET, 250, -800, 40, 0, 1300, 0, 0);
	go = TouchDodge_Choose(&w, &r, &p, &m, still, 1.0f, 10, &c);
	CHECK(!go && c.base_damage == 0, "a rocket that misses: no dodge");

	// one flying away: nothing
	r = shot(TD_KIND_ROCKET, 0, 200, 40, 0, 1300, 0, 0);
	go = TouchDodge_Choose(&w, &r, &p, &m, still, 1.0f, 10, &c);
	CHECK(!go, "a rocket flying away: no dodge");

	// one too far to arrive inside the window: nothing yet
	r = shot(TD_KIND_ROCKET, 0, -2500, 40, 0, 1300, 0, 0);
	go = TouchDodge_Choose(&w, &r, &p, &m, still, 1.0f, 10, &c);
	CHECK(!go, "a rocket beyond the warning window: not yet");

	// a mortar grenade lobbed to land at the player's feet
	{
		float t = 0.8f, dx = 900, vz = (0 + 0.5f * 800 * t * t - 40) / t;
		r = shot(TD_KIND_GRENADE, 0, -dx, 40, 0, dx / t, vz, 800);
		go = TouchDodge_Choose(&w, &r, &p, &m, still, 1.2f, 10, &c);
		printf("  mortar grenade from 900: carry on %.0f, %s -> %.0f\n", c.base_damage, c.chosen.name, c.damage);
		CHECK(c.base_damage > 40, "mortar grenade: lands on the player");
		CHECK(go && c.damage < c.base_damage - 10, "mortar grenade: an escape cuts it");
	}

	// an electro orb rolling past a little to the side: orbs bounce, only a
	// touch counts: nothing
	r = shot(TD_KIND_ELECTRO, 60, -600, 20, 0, 1000, 0, 800);
	go = TouchDodge_Choose(&w, &r, &p, &m, still, 1.0f, 10, &c);
	CHECK(!go, "an orb that misses: no dodge");
}

// a Crylink's six pellets: 10 each, under any one-shot threshold, 60 together
static void test_pellets(void)
{
	tr_world_t w;
	td_player_t p;
	td_move_t m = xonotic_move(0);
	td_proj_t r[6];
	td_choice_t c;
	int i, go;
	arena();
	w = world();
	p = standing(0, 0);
	for (i = 0; i < 6; i++)
	{
		// one at the centre and five on a ring 0.08 rad out, from 600 units
		float a = i ? (i - 1) * 6.2832f / 5 : 0, sp = i ? 0.08f : 0;
		r[i] = shot(TD_KIND_CRYLINK, 0, -600, 40, 2000 * sinf(sp) * cosf(a), 2000 * cosf(sp), 2000 * sinf(sp) * sinf(a), 0);
	}
	go = TouchDodge_Choose(&w, &r[0], &p, &m, still, 1.0f, 7.5f, &c);
	CHECK(c.base_damage <= 10.5f, "one pellet alone: 10 at most");
	go = TouchDodge_ChooseAll(&w, r, 6, &p, &m, still, 1.0f, 7.5f, &c);
	printf("  crylink pellets from 600: carry on %.0f, %s -> %.0f\n", c.base_damage, c.chosen.name, c.damage);
	CHECK(c.base_damage > 15, "six pellets together: over the 15 threshold that no single one reaches");
	CHECK(go && c.damage <= c.base_damage * 0.5f, "six pellets: an escape halves it at least");
}

static void test_never(void)
{
	tr_world_t w;
	td_player_t p;
	td_move_t m = xonotic_move(0);
	td_proj_t r;
	td_choice_t c;
	int go;

	// a wall just to the left (the shot comes up +y, so left is -x): right
	arena();
	add_box(-60, -400, 0, -40, 400, 300, K_SOLID);
	w = world();
	p = standing(0, 0);
	r = shot(TD_KIND_ROCKET, 0, -800, 40, 0, 1300, 0, 0);
	go = TouchDodge_Choose(&w, &r, &p, &m, still, 1.0f, 10, &c);
	CHECK(go && c.chosen.dir[0] > 0.3f, "wall on the left: goes right");

	// a ledge on the right (void past x 60): left, never right
	world_clear();
	add_box(-4000, -4000, -64, 60, 4000, 0, K_SOLID);
	add_box(-8000, -8000, -2000, 8000, 8000, -1900, K_SKY);
	w = world();
	w.floor_z = -2000;
	go = TouchDodge_Choose(&w, &r, &p, &m, still, 1.0f, 10, &c);
	CHECK(go && c.chosen.dir[0] < -0.3f, "void on the right: goes left");
	// and with a wall on the left too: never right, into the void
	add_box(-60, -400, 0, -40, 400, 300, K_SOLID);
	w = world();
	w.floor_z = -2000;
	go = TouchDodge_Choose(&w, &r, &p, &m, still, 1.0f, 10, &c);
	CHECK(!go || c.chosen.dir[0] < 0.1f, "void right, wall left: never into the void");
	CHECK(c.rejected > 0, "void right, wall left: escapes ruled out");

	// a narrow bridge, void both sides: no sideways step (back, or nothing)
	world_clear();
	add_box(-40, -4000, -64, 40, 4000, 0, K_SOLID);
	add_box(-8000, -8000, -2000, 8000, 8000, -1900, K_SKY);
	w = world();
	w.floor_z = -2000;
	go = TouchDodge_Choose(&w, &r, &p, &m, still, 1.0f, 10, &c);
	CHECK(!go || fabsf(c.chosen.dir[0]) < 0.1f, "narrow bridge: never steps off the side");

	// lava beside the player: never into it
	world_clear();
	add_box(-4000, -4000, -64, 4000, 4000, 0, K_SOLID);
	add_box(60, -4000, -64, 4000, 4000, 1, K_LAVA);
	w = world();
	go = TouchDodge_Choose(&w, &r, &p, &m, still, 1.0f, 10, &c);
	CHECK(go && c.chosen.dir[0] < 0, "lava on the right: goes left");
}

static void test_dodge_and_motion(void)
{
	tr_world_t w;
	td_player_t p;
	td_move_t m;
	td_plan_t plan;
	static float path[TD_MAX_STEPS][3];
	td_proj_t r;
	td_choice_t c;

	// how far the model moves the player sideways from standing, at 360 u/s
	// (server, nexballarena: strafe 82 at 0.2 s, 178 at 0.4 s)
	arena();
	w = world();
	p = standing(0, 0);
	m = xonotic_move(0);
	memset(&plan, 0, sizeof(plan));
	plan.dir[0] = 1;
	td_player_path(&p, &m, &plan, 24, path);
	printf("  strafe model: %.0f at 0.2 s, %.0f at 0.4 s, %.0f at 0.6 s\n", path[7][0], path[15][0], path[23][0]);
	CHECK(path[7][0] > 40 && path[7][0] < 110, "strafe model: about the measured distance at 0.2 s");
	plan.jump = 1;
	td_player_path(&p, &m, &plan, 24, path);
	CHECK(path[7][2] > 30, "jump plan: leaves the ground");
	// with dodging on the server a dodge is offered and goes further
	{
		float strafe02;
		plan.jump = 0;
		td_player_path(&p, &m, &plan, 24, path);
		strafe02 = path[7][0];
		m = xonotic_move(1);
		plan.dodge = 1;
		td_player_path(&p, &m, &plan, 24, path);
		printf("  dodge model: %.0f at 0.2 s\n", path[7][0]);
		CHECK(path[7][0] > strafe02 + 5, "dodge model: further than a strafe at 0.2 s");
	}
	r = shot(TD_KIND_ROCKET, 0, -500, 40, 0, 1300, 0, 0);
	TouchDodge_Choose(&w, &r, &p, &m, still, 1.0f, 10, &c);
	CHECK(c.damage < c.base_damage, "with dodging: an escape found");

	// the kinds not worth dodging are not in the table
	CHECK(TouchDodge_Kind(10) == NULL, "blaster (6000 u/s) is not dodged");
	CHECK(TouchDodge_Kind(11) == NULL, "hlac (6000 u/s) is not dodged");
	CHECK(TouchDodge_Kind(TD_KIND_ROCKET) != NULL, "rockets are");
}

int main(void)
{
	test_choices();
	test_pellets();
	test_never();
	test_dodge_and_motion();
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
