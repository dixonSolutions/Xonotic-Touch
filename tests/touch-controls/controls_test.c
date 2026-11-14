/*
Checks the pure parts of the in-match touch controls:
  - the top bar (engine/darkplaces/touch_topbar.h): at 1920x1200, 2340x1080
    and 1280x720, right- and left-handed, with and without safe-area insets,
    no two members intersect, every member is at least 36 dp tall and on
    screen, the row stays clear of the match clock, and the game's panels
    start below it;
  - the look (touch_feel.h): the same swipe in centimetres turns the same
    angle at two densities, nothing a finger moved is lost when it lifts, a
    flick turns further than a slow drag of the same length and never less,
    acceleration off is exactly linear;
  - the move stick: it floats to the thumb, has a dead zone, reaches full
    speed at the run deflection, keeps its direction, and follows a thumb
    that passes the rim.
Run through scripts/test-touch-controls.sh.
*/
#include <stdio.h>
#include <string.h>

#include "touch_hudmode.h"
#include "touch_topbar.h"
#include "touch_feel.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static float fabsf_(float x) { return x < 0 ? -x : x; }

// --- top bar ---------------------------------------------------------------
typedef struct { const char *name; float px_w, px_h, dpi; } screen_t;
static const screen_t screens[] = {
	{ "tablet 1920x1200", 1920, 1200, 283 },
	{ "phone 2340x1080", 2340, 1080, 395 },
	{ "desk 1280x720", 1280, 720, 0 },
	{ "small phone 1280x720 at 320 dpi", 1280, 720, 320 },
};

static void check_topbar(void)
{
	size_t i;
	int hand, inset, vis;
	for (i = 0; i < sizeof(screens) / sizeof(screens[0]); i++)
	for (hand = 0; hand < 2; hand++)
	for (inset = 0; inset < 2; inset++)
	for (vis = 0; vis < 8; vis++)
	{
		const screen_t *s = &screens[i];
		touchbar_in_t in;
		touchbar_t b;
		const touchbar_rect_t *r[5];
		const char *names[5] = { "chip", "console", "MENU", "SCORE", "CHAT" };
		touchbar_rect_t clock;
		float density = TouchDp_Density(s->dpi, s->px_h);
		// the 2D space the engine uses on these screens: the pixels (vid_conwidthauto)
		float w = s->px_w, h = s->px_h, u = density;
		int a, c;
		memset(&in, 0, sizeof(in));
		in.w = w; in.h = h; in.u = u;
		in.in_l = in.in_r = inset ? w * 0.035f : 0;   // a cutout: 82 px on the 2340 px phone
		in.in_t = inset ? h * 0.02f : 0;
		in.pill_h = (h * 0.060f > TOUCHDP_PILL_HEIGHT * u) ? h * 0.060f : TOUCHDP_PILL_HEIGHT * u;
		in.aspect = 2.4f;
		in.console = 48 * u > in.pill_h ? 48 * u : in.pill_h;
		in.menu = (vis & 1) == 0; in.score = (vis & 2) == 0; in.chat = (vis & 4) == 0;
		in.left_handed = hand;
		TouchTopBar_Layout(&in, &b);
		r[0] = &b.chip; r[1] = &b.console; r[2] = &b.menu; r[3] = &b.score; r[4] = &b.chat;
		clock.w = 2 * TOUCHBAR_CLOCK_DP * u * 0.75f; clock.x = w * 0.5f - clock.w * 0.5f; clock.y = 0; clock.h = h * 0.056f;
		for (a = 0; a < 5; a++)
		{
			if (r[a]->w <= 0)
				continue;
			CHECK(r[a]->x >= in.in_l && r[a]->x + r[a]->w <= w - in.in_r && r[a]->y >= in.in_t, "%s hand %d inset %d vis %d: %s is off the safe area", s->name, hand, inset, vis, names[a]);
			CHECK(r[a]->h >= TOUCHDP_PILL_HEIGHT * u - 0.01f, "%s: %s is under 36 dp tall", s->name, names[a]);
			CHECK(r[a]->w >= TOUCHDP_PILL_HEIGHT * u * TOUCHBAR_MIN_ASPECT - 0.01f || a < 2, "%s: %s is too narrow", s->name, names[a]);
			CHECK(!TouchTopBar_Intersect(r[a], &clock), "%s hand %d inset %d vis %d: %s is on the match clock", s->name, hand, inset, vis, names[a]);
			CHECK(r[a]->y + r[a]->h <= b.bottom || a == 0, "%s: %s reaches below the row's bottom", s->name, names[a]);
			for (c = a + 1; c < 5; c++)
			{
				CHECK(!TouchTopBar_Intersect(r[a], r[c]), "%s hand %d inset %d vis %d: %s and %s intersect", s->name, hand, inset, vis, names[a], names[c]);
				if (r[c]->w > 0)
				{
					// at least the gap apart
					float gap = r[a]->x > r[c]->x ? r[a]->x - (r[c]->x + r[c]->w) : r[c]->x - (r[a]->x + r[a]->w);
					if (a == 0 && r[0]->y >= r[c]->y + r[c]->h)
						gap = r[0]->y - (r[c]->y + r[c]->h);   // the chip under the row (a narrow screen)
					CHECK(gap >= TOUCHBAR_GAP_DP * u * 0.5f - 0.01f, "%s: %s and %s are %.1f apart, under the gap", s->name, names[a], names[c], gap);
				}
			}
		}
		CHECK(b.bottom / h < 0.25f, "%s: the row takes %.0f%% of the height", s->name, 100 * b.bottom / h);
	}
}

// --- look ------------------------------------------------------------------
static touchlook_cfg_t cfg(float accel, int filter)
{
	touchlook_cfg_t c;
	c.sens = 3.6f; c.y_mult = 0.85f; c.accel = accel; c.accel_lo = 150; c.accel_hi = 900;
	c.filter = filter; c.fcmin = 4.0f; c.beta = 0.05f; c.max_rate = 900 * (3.6f / 2.8f) * (1 + accel);
	return c;
}

// A swipe of `cm` to the right over `seconds`, on a screen of `dpi`, at 60 Hz;
// returns the yaw turned, the finger's lift included.
static float swipe(const touchlook_cfg_t *c, float cm, float seconds, float dpi, int flush)
{
	touchlook_t s;
	int n = (int)(seconds * 60 + 0.5f), i;
	float px = cm / 2.54f * dpi, density = dpi / 160.0f, total = 0, yaw, pitch;
	TouchLook_Reset(&s);
	if (n < 1) n = 1;
	for (i = 0; i < n; i++)
	{
		TouchLook_Step(&s, c, px / n / density, 0, 1.0f / 60.0f, &yaw, &pitch);
		total += yaw;
	}
	if (flush)
	{
		TouchLook_Flush(&s, &yaw, &pitch);
		total += yaw;
	}
	return total;
}

static void check_look(void)
{
	touchlook_cfg_t on = cfg(0.8f, 1), off = cfg(0, 1), raw = cfg(0, 0);
	float tablet = swipe(&on, 2, 1.0f, 283, 1), phone = swipe(&on, 2, 1.0f, 395, 1);
	float expect = 2 / 2.54f * 160 * TOUCHFEEL_DEG_PER_DP * 3.6f;   // 2 cm in dp, gain 1
	float slow, flick, a, b;
	CHECK(fabsf_(tablet - phone) < 0.01f, "the same 2 cm swipe turns %.3f on the tablet and %.3f on the phone", tablet, phone);
	CHECK(fabsf_(tablet - expect) < 0.05f, "a slow 2 cm swipe turns %.3f, expected %.3f (gain 1 under the acceleration's lower speed)", tablet, expect);
	// nothing lost on lift, whatever the smoothing
	a = swipe(&off, 3, 0.05f, 283, 1);
	b = swipe(&raw, 3, 0.05f, 283, 1);
	CHECK(fabsf_(a - b) < 0.01f, "a 50 ms flick turns %.3f smoothed and %.3f unsmoothed: the lift must pay what is owed", a, b);
	CHECK(swipe(&off, 3, 0.05f, 283, 0) < a - 1, "without the lift's flush part of a flick is still owed (the old filter dropped it)");
	// acceleration off: linear in distance, whatever the speed
	CHECK(fabsf_(swipe(&off, 4, 0.1f, 283, 1) - 2 * swipe(&off, 2, 2.0f, 283, 1)) < 0.05f, "with acceleration off the turn depends on distance alone");
	// acceleration: a flick turns further, at most (1 + accel) times
	slow = swipe(&on, 3, 2.0f, 283, 1);
	flick = swipe(&on, 3, 0.08f, 283, 1);
	CHECK(flick > slow * 1.5f && flick <= slow * 1.8f + 0.05f, "a 3 cm flick turns %.1f, a slow 3 cm drag %.1f: expected 1.5 to 1.8 times", flick, slow);
	CHECK(flick >= 180 * 0.9f, "a 3 cm flick turns %.1f degrees: about a turn-around", flick);
	// monotonic: never less turn for a faster swipe of the same length
	{
		float prev = 0, t;
		for (t = 2.0f; t > 0.03f; t *= 0.8f)
		{
			float y = swipe(&on, 3, t, 283, 1);
			CHECK(y >= prev - 0.05f, "a 3 cm swipe in %.2f s turns %.2f, less than a slower one (%.2f)", t, y, prev);
			prev = y;
		}
	}
	// the vertical ratio
	{
		touchlook_t s;
		float yaw, pitch;
		TouchLook_Reset(&s);
		TouchLook_Step(&s, &raw, 10, 10, 1.0f / 60.0f, &yaw, &pitch);
		CHECK(fabsf_(pitch - yaw * 0.85f) < 0.001f, "pitch %.3f is not yaw %.3f times the vertical ratio", pitch, yaw);
	}
	// a contact that jumps across the screen is clamped, and the rest stays owed (not lost)
	{
		touchlook_t s;
		float yaw, pitch, total;
		TouchLook_Reset(&s);
		TouchLook_Step(&s, &raw, 1000, 0, 1.0f / 60.0f, &yaw, &pitch);
		CHECK(yaw <= raw.max_rate / 60.0f + 0.01f, "one frame turned %.1f degrees: over the clamp", yaw);
		total = yaw;
		TouchLook_Flush(&s, &yaw, &pitch);
		CHECK(total + yaw > 500, "the clamp dropped travel");
	}
}

// --- stick -----------------------------------------------------------------
static void check_stick(void)
{
	touchstick_t s;
	float mx, my, kx, ky, r = 100;
	TouchStick_Land(&s, 300, 800, r, 1920, 1200);
	CHECK(s.cx == 300 && s.cy == 800, "the stick's centre is not under the thumb");
	TouchStick_Land(&s, 10, 1195, r, 1920, 1200);
	CHECK(s.cx >= r * 0.5f && s.cy <= 1200 - r * 0.5f, "a thumb in the corner leaves no room to push (%.0f, %.0f)", s.cx, s.cy);
	TouchStick_Land(&s, 300, 800, r, 1920, 1200);
	TouchStick_Read(&s, 300 + 8, 800, r, 1, 0.12f, 0.6f, &mx, &my, &kx, &ky);
	CHECK(mx == 0 && my == 0, "inside the dead zone the stick moves (%.2f, %.2f)", mx, my);
	TouchStick_Read(&s, 300 + 36, 800, r, 1, 0.12f, 0.6f, &mx, &my, &kx, &ky);
	CHECK(mx > 0.45f && mx < 0.55f && my == 0, "halfway between the dead zone and the run point gives %.2f, expected 0.5", mx);
	TouchStick_Read(&s, 300 + 60, 800, r, 1, 0.12f, 0.6f, &mx, &my, &kx, &ky);
	CHECK(fabsf_(mx - 1) < 0.001f, "at the run deflection the stick gives %.2f, not full speed", mx);
	TouchStick_Read(&s, 300 + 60, 800 - 60, r, 1, 0.12f, 0.6f, &mx, &my, &kx, &ky);
	CHECK(fabsf_(mx + my) < 0.001f && fabsf_(mx * mx + my * my - 1) < 0.001f, "a diagonal push gives (%.2f, %.2f): not a unit vector at 45 degrees", mx, my);
	TouchStick_Read(&s, 300 + 20, 800 - 80, r, 1, 0.12f, 0.6f, &mx, &my, &kx, &ky);
	CHECK(fabsf_(mx / my - 20.0f / -80.0f) < 0.001f, "the move's angle is not the thumb's");
	// past the rim: the centre follows, the knob stays on the rim
	TouchStick_Land(&s, 300, 800, r, 1920, 1200);
	TouchStick_Read(&s, 300 + 250, 800, r, 1, 0.12f, 0.6f, &mx, &my, &kx, &ky);
	CHECK(fabsf_(s.cx - 450) < 0.01f && fabsf_(kx - 1) < 0.001f, "the centre did not follow the thumb past the rim (centre %.0f)", s.cx);
	TouchStick_Read(&s, 300 + 250 - 220, 800, r, 1, 0.12f, 0.6f, &mx, &my, &kx, &ky);
	CHECK(mx < -0.99f, "after following, pulling back reverses at once (got %.2f)", mx);
	// touch_stick_follow 0: the centre stays
	TouchStick_Land(&s, 300, 800, r, 1920, 1200);
	TouchStick_Read(&s, 300 + 250, 800, r, 0, 0.12f, 0.6f, &mx, &my, &kx, &ky);
	CHECK(s.cx == 300 && fabsf_(mx - 1) < 0.001f, "with following off the centre moved");
}

int main(void)
{
	check_topbar();
	check_look();
	check_stick();
	CHECK(TOUCHDP_MIN_TARGET >= 48, "the smallest touch target is under 48 dp");
	if (failures)
	{
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("touch controls: top bar, look and stick checks passed\n");
	return 0;
}
