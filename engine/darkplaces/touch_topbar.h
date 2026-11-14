/*
Godnotic: the in-match top bar, one layout for everything that sits on the
top edge while the touch controls are on.

  [vitals]            [timer]        (i) [>_] [MENU] [SCORE] [CHAT]
                      [mode icons]        info messages   | scores
                                          kill feed

The chrome row hugs the top corner on the look hand's side (right; left when
left-handed): CHAT outermost, then SCORE, MENU, the console button and, at
the inner end, the vote chip's slot (kept free, so nothing jumps when a vote
is called). Sizes are in dp, positions are measured from the edge and the
safe-area insets, so the row is the same physical size on a phone and a
tablet and its members can never touch each other, whatever the aspect
ratio. A row that would reach the match clock in the middle narrows its
pills. Everything the game itself draws near that corner (score list, info
messages, kill feed, vote print, mode icons, radar) is placed below or
beside the row by cvar (touch_hud.c TouchHUD_SyncStockPanels), under the
port's CSQC and a server's alike.

Pure: scripts/test-touch-controls.sh compiles this alone and checks that no
two rects intersect at 1920x1200, 2340x1080 and 1280x720.
*/
#ifndef TOUCH_TOPBAR_H
#define TOUCH_TOPBAR_H

#define TOUCHBAR_MARGIN_DP   8.0f    // from the edge (after the safe inset)
#define TOUCHBAR_GAP_DP      8.0f    // between members
#define TOUCHBAR_TOP_DP      6.0f
#define TOUCHBAR_CHIP_DP     44.0f   // the vote chip's slot
#define TOUCHBAR_CLOCK_DP    64.0f   // half of what the match clock keeps in the middle
#define TOUCHBAR_MIN_ASPECT  1.6f    // a pill is never narrower than this times its height

typedef struct touchbar_rect_s { float x, y, w, h; } touchbar_rect_t;

typedef struct touchbar_in_s
{
	float w, h;          // the 2D space (console units)
	float u;             // console units per dp
	float in_l, in_t, in_r;  // safe-area insets, console units
	float pill_h;        // a pill's drawn height, console units
	float aspect;        // a pill's width / height when there is room
	float console;       // the console button's side, console units (0: not shown)
	int menu, score, chat;   // shown
	int left_handed;
} touchbar_in_t;

typedef struct touchbar_s
{
	touchbar_rect_t chip, console, menu, score, chat;   // w == 0: not there
	float aspect;    // the pill aspect used
	float bottom;    // the game's own panels start below this
	float inner;     // the row's inner end (x), for what sits beside it
} touchbar_t;

static inline int TouchTopBar_Intersect(const touchbar_rect_t *a, const touchbar_rect_t *b)
{
	if (a->w <= 0 || b->w <= 0)
		return 0;
	return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

static inline void TouchTopBar_Layout(const touchbar_in_t *in, touchbar_t *out)
{
	float u = in->u, m = TOUCHBAR_MARGIN_DP * u, g = TOUCHBAR_GAP_DP * u;
	float chip = TOUCHBAR_CHIP_DP * u;
	int pills = (in->menu != 0) + (in->score != 0) + (in->chat != 0);
	float inset = in->left_handed ? in->in_l : in->in_r;
	float rowh, cy, x, aspect = in->aspect, avail, fixed, pw;
	touchbar_rect_t *order[5];
	float width[5], height[5];
	int n = 0, i, tight = 0;
	touchbar_rect_t zero = {0, 0, 0, 0};
	out->chip = out->console = out->menu = out->score = out->chat = zero;
	rowh = in->pill_h;
	if (in->console > rowh) rowh = in->console;
	if (chip > rowh) rowh = chip;
	// how wide the pills may be before the row reaches the clock
	fixed = m + chip + (in->console > 0 ? in->console + g : 0) + pills * g;
	avail = in->w * 0.5f - TOUCHBAR_CLOCK_DP * u - inset - fixed;
	if (pills > 0 && TOUCHBAR_MIN_ASPECT * in->pill_h * pills > avail)
	{
		// A screen too narrow for the whole row even with the narrowest
		// pills (a small phone): half gaps, and the vote chip's slot goes
		// under the row's outer end instead of beside its inner one.
		tight = 1;
		g *= 0.5f;
		fixed = m + (in->console > 0 ? in->console + g : 0) + (pills - 1) * g;
		avail = in->w * 0.5f - TOUCHBAR_CLOCK_DP * u - inset - fixed;
	}
	if (pills > 0 && aspect * in->pill_h * pills > avail)
		aspect = avail / (in->pill_h * pills);
	if (aspect < TOUCHBAR_MIN_ASPECT)
		aspect = TOUCHBAR_MIN_ASPECT;
	pw = in->pill_h * aspect;
	// outermost first
	if (in->chat) { order[n] = &out->chat; width[n] = pw; height[n] = in->pill_h; n++; }
	if (in->score) { order[n] = &out->score; width[n] = pw; height[n] = in->pill_h; n++; }
	if (in->menu) { order[n] = &out->menu; width[n] = pw; height[n] = in->pill_h; n++; }
	if (in->console > 0) { order[n] = &out->console; width[n] = in->console; height[n] = in->console; n++; }
	if (!tight) { order[n] = &out->chip; width[n] = chip; height[n] = chip; n++; }
	cy = in->in_t + TOUCHBAR_TOP_DP * u + rowh * 0.5f;
	x = in->w - inset - m;
	for (i = 0; i < n; i++)
	{
		order[i]->w = width[i];
		order[i]->h = height[i];
		order[i]->x = x - width[i];
		order[i]->y = cy - height[i] * 0.5f;
		x -= width[i] + g;
	}
	out->inner = x + g;
	if (tight)
	{
		out->chip.w = out->chip.h = chip;
		out->chip.x = in->w - inset - m - chip;
		out->chip.y = cy + rowh * 0.5f + g;
		order[n++] = &out->chip;
	}
	if (in->left_handed)
	{
		for (i = 0; i < n; i++)
			order[i]->x = in->w - order[i]->x - order[i]->w;
		out->inner = in->w - out->inner;
	}
	out->aspect = aspect;
	out->bottom = in->in_t + TOUCHBAR_TOP_DP * u + rowh + TOUCHBAR_TOP_DP * u;
}

#endif
