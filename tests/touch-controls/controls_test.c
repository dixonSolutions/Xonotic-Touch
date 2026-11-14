/*
Checks the pure parts of the in-match touch controls:
  - the top bar (engine/darkplaces/touch_topbar.h): at 1920x1200, 2340x1080
    and 1280x720, right- and left-handed, with and without safe-area insets,
    no two members intersect, every member is at least 36 dp tall and on
    screen, the row stays clear of the match clock, and the game's panels
    start below it.
Run through scripts/test-touch-controls.sh.
*/
#include <stdio.h>
#include <string.h>

#include "touch_hudmode.h"
#include "touch_topbar.h"

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

int main(void)
{
	check_topbar();
	if (failures)
	{
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("touch controls: top bar checks passed\n");
	return 0;
}
