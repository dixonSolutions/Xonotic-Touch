/*
Checks the in-match HUD modes (engine/darkplaces/touch_hudmode.h): the mode
and join state derived from the client state, table-driven over every state;
the controls each mode has, in particular that the end-of-match scoreboard
and the next-map vote get MENU and nothing else; the MENU sheet's action
list; and the join swipe recogniser over recorded touch traces.

Run through scripts/test-touch-hudmode.sh.
*/

#include <stdio.h>
#include <string.h>

#include "touch_hudmode.h"

static int failures;

#define CHECK_INT(expected, actual, what) do { \
	int e_ = (expected), a_ = (actual); \
	if (e_ != a_) { \
		printf("FAIL %s: expected %d, got %d\n", (what), e_, a_); \
		failures++; \
	} \
} while (0)

#define CHECK_STR(expected, actual, what) do { \
	if (strcmp((expected), (actual))) { \
		printf("FAIL %s: expected \"%s\", got \"%s\"\n", (what), (expected), (actual)); \
		failures++; \
	} \
} while (0)

// ---------------------------------------------------------------------------
// Mode derivation
// ---------------------------------------------------------------------------
typedef struct
{
	const char *what;
	touchmode_in_t in;
	const char *mode;
	int flags;
	const char *join;
} mode_case_t;

// in: in_match, intermission, mapvote, spec_state, health, warmup, ready_for_me,
//     teamplay, round_live, carrying, join_pending, join_queued, join_refused
static const mode_case_t mode_cases[] = {
	{ "menu, not connected", {0, 0, 0, 0, 100, 0, 0, 0, 0, 0, 0, 0, 0}, "off", 0, "none" },
	{ "playing", {1, 0, 0, TOUCHSPEC_NONE, 100, 0, 0, 0, 0, 0, 0, 0, 0}, "playing", 0, "none" },
	{ "dead", {1, 0, 0, TOUCHSPEC_NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0}, "dead", 0, "none" },
	{ "gibbed (negative health)", {1, 0, 0, TOUCHSPEC_NONE, -40, 0, 0, 0, 0, 0, 0, 0, 0}, "dead", 0, "none" },
	{ "free-fly", {1, 0, 0, TOUCHSPEC_OBSERVING, 0, 0, 0, 0, 0, 0, 0, 0, 0}, "spectating", 0, "open" },
	{ "following", {1, 0, 0, TOUCHSPEC_FOLLOWING, 100, 0, 0, 0, 0, 0, 0, 0, 0}, "following", 0, "open" },
	{ "out of the round, watching", {1, 0, 0, TOUCHSPEC_OUT, 100, 0, 0, 1, 1, 0, 0, 0, 0}, "out", TOUCHMODE_F_TEAMS, "none" },
	{ "out of the round, free-fly", {1, 0, 0, TOUCHSPEC_OUT_FREE, 0, 0, 0, 0, 0, 0, 0, 0, 0}, "out_free", 0, "none" },
	{ "warmup, playing, ready wanted", {1, 0, 0, TOUCHSPEC_NONE, 100, 1, 1, 0, 0, 0, 0, 0, 0}, "playing", TOUCHMODE_F_WARMUP | TOUCHMODE_F_READY, "none" },
	{ "warmup, spectating (ready is for players)", {1, 0, 0, TOUCHSPEC_OBSERVING, 0, 1, 1, 0, 0, 0, 0, 0, 0}, "spectating", TOUCHMODE_F_WARMUP, "open" },
	{ "warmup, dead, ready wanted", {1, 0, 0, TOUCHSPEC_NONE, 0, 1, 1, 0, 0, 0, 0, 0, 0}, "dead", TOUCHMODE_F_WARMUP | TOUCHMODE_F_READY, "none" },
	{ "intermission", {1, 1, 0, TOUCHSPEC_NONE, 100, 0, 0, 1, 0, 0, 0, 0, 0}, "intermission", TOUCHMODE_F_TEAMS, "none" },
	{ "intermission as a spectator", {1, 1, 0, TOUCHSPEC_FOLLOWING, 100, 1, 0, 0, 0, 0, 0, 0, 0}, "intermission", 0, "none" },
	{ "next-map vote", {1, 1, 1, TOUCHSPEC_NONE, 100, 0, 0, 0, 0, 0, 0, 0, 0}, "mapvote", 0, "none" },
	{ "next-map vote without the intermission flag", {1, 0, 1, TOUCHSPEC_OBSERVING, 0, 0, 0, 0, 0, 0, 0, 0, 0}, "mapvote", 0, "none" },
	{ "team game, round live, playing", {1, 0, 0, TOUCHSPEC_NONE, 100, 0, 0, 1, 1, 0, 0, 0, 0}, "playing", TOUCHMODE_F_TEAMS | TOUCHMODE_F_ROUND, "none" },
	{ "carrying the flag", {1, 0, 0, TOUCHSPEC_NONE, 100, 0, 0, 1, 0, 1, 0, 0, 0}, "playing", TOUCHMODE_F_TEAMS | TOUCHMODE_F_CARRY, "none" },
	{ "carrying, dead (dropped)", {1, 0, 0, TOUCHSPEC_NONE, 0, 0, 0, 1, 0, 1, 0, 0, 0}, "dead", TOUCHMODE_F_TEAMS, "none" },
	{ "join sent, waiting", {1, 0, 0, TOUCHSPEC_OBSERVING, 0, 0, 0, 0, 0, 0, 1, 0, 0}, "spectating", 0, "pending" },
	{ "join queued", {1, 0, 0, TOUCHSPEC_FOLLOWING, 100, 0, 0, 1, 0, 0, 0, 1, 0}, "following", TOUCHMODE_F_TEAMS, "queued" },
	{ "join refused", {1, 0, 0, TOUCHSPEC_OBSERVING, 0, 0, 0, 0, 0, 0, 0, 0, 1}, "spectating", 0, "locked" },
	{ "queued wins over pending", {1, 0, 0, TOUCHSPEC_OBSERVING, 0, 0, 0, 0, 0, 0, 1, 1, 0}, "spectating", 0, "queued" },
};

static void check_modes(void)
{
	size_t i;
	for (i = 0; i < sizeof(mode_cases) / sizeof(mode_cases[0]); i++)
	{
		const mode_case_t *c = &mode_cases[i];
		touchmode_t m = TouchMode_Derive(&c->in);
		char what[160];
		snprintf(what, sizeof(what), "%s: mode", c->what);
		CHECK_STR(c->mode, TouchMode_Name(m.mode), what);
		snprintf(what, sizeof(what), "%s: flags", c->what);
		CHECK_INT(c->flags, m.flags, what);
		snprintf(what, sizeof(what), "%s: join", c->what);
		CHECK_STR(c->join, TouchMode_JoinName(m.join), what);
	}
}

// ---------------------------------------------------------------------------
// Controls per mode
// ---------------------------------------------------------------------------
static void check_controls(void)
{
	int mode, flags;
	// match end: MENU and nothing else, whatever the flags
	for (flags = 0; flags < 32; flags++)
	{
		CHECK_INT(TOUCHCTL_MENU, TouchMode_Controls(TOUCHMODE_INTERMISSION, flags), "intermission: MENU only");
		CHECK_INT(TOUCHCTL_MENU, TouchMode_Controls(TOUCHMODE_MAPVOTE, flags), "map vote: MENU only");
		CHECK_INT(0, TouchMode_Controls(TOUCHMODE_OFF, flags), "off: nothing");
	}
	for (mode = TOUCHMODE_OBSERVING; mode <= TOUCHMODE_OUT_FREE; mode++)
	{
		int c = TouchMode_Controls(mode, TOUCHMODE_F_WARMUP | TOUCHMODE_F_READY);
		// a spectator has no fighting controls
		CHECK_INT(0, c & (TOUCHCTL_FIRE | TOUCHCTL_ALT | TOUCHCTL_HOP | TOUCHCTL_DUCK | TOUCHCTL_WEAPONS
			| TOUCHCTL_ZOOM | TOUCHCTL_HOOK | TOUCHCTL_NADE | TOUCHCTL_TURN | TOUCHCTL_RELOAD | TOUCHCTL_DODGE
			| TOUCHCTL_TOSPEC | TOUCHCTL_RESPAWN), "spectating: no play controls");
		CHECK_INT(TOUCHCTL_SPECBAR | TOUCHCTL_MENU | TOUCHCTL_SCORE, c & (TOUCHCTL_SPECBAR | TOUCHCTL_MENU | TOUCHCTL_SCORE), "spectating: bar, MENU, SCORE");
		CHECK_INT(TouchMode_FreeFly(mode) ? TOUCHCTL_MOVE | TOUCHCTL_FLY : 0, c & (TOUCHCTL_MOVE | TOUCHCTL_FLY), "stick and rocker only free-flying");
		CHECK_INT(TouchMode_IsSpectating(mode) ? TOUCHCTL_JOIN : 0, c & TOUCHCTL_JOIN, "join handle only for a real spectator");
	}
	CHECK_INT(TOUCHCTL_TOSPEC, TouchMode_Controls(TOUCHMODE_PLAYING, 0) & TOUCHCTL_TOSPEC, "playing: move to spectators can be placed");
	CHECK_INT(0, TouchMode_Controls(TOUCHMODE_PLAYING, 0) & (TOUCHCTL_JOIN | TOUCHCTL_SPECBAR | TOUCHCTL_FLY | TOUCHCTL_READY), "playing: no spectator controls, no READY outside warmup");
	CHECK_INT(TOUCHCTL_READY, TouchMode_Controls(TOUCHMODE_PLAYING, TOUCHMODE_F_READY) & TOUCHCTL_READY, "warmup: READY");
	CHECK_INT(TOUCHCTL_RESPAWN | TOUCHCTL_LOOK, TouchMode_Controls(TOUCHMODE_DEAD, 0) & (TOUCHCTL_RESPAWN | TOUCHCTL_LOOK | TOUCHCTL_FIRE | TOUCHCTL_MOVE), "dead: RESPAWN and look");
}

// ---------------------------------------------------------------------------
// The MENU sheet's actions
// ---------------------------------------------------------------------------
static void actions_string(const touchmode_t *m, char *out, size_t size)
{
	int act;
	out[0] = 0;
	for (act = 0; act < TOUCHACT_COUNT; act++)
	{
		int st = TouchAct_State(m, act);
		if (st == TOUCHACT_HIDDEN)
			continue;
		if (out[0])
			strncat(out, " ", size - strlen(out) - 1);
		strncat(out, TouchAct_Id(act), size - strlen(out) - 1);
		if (st == TOUCHACT_DISABLED)
			strncat(out, ":disabled", size - strlen(out) - 1);
		if (st == TOUCHACT_CONFIRM)
			strncat(out, ":confirm", size - strlen(out) - 1);
	}
}

static void check_actions(void)
{
	static const struct { int mode, flags, join; const char *expected; } cases[] = {
		{ TOUCHMODE_OFF, 0, TOUCHJOIN_NONE, "" },
		{ TOUCHMODE_PLAYING, 0, TOUCHJOIN_NONE, "resume spectate settings leave" },
		{ TOUCHMODE_PLAYING, TOUCHMODE_F_TEAMS, TOUCHJOIN_NONE, "resume spectate team settings leave" },
		{ TOUCHMODE_PLAYING, TOUCHMODE_F_TEAMS | TOUCHMODE_F_CARRY, TOUCHJOIN_NONE, "resume spectate:confirm team settings leave" },
		{ TOUCHMODE_PLAYING, TOUCHMODE_F_ROUND, TOUCHJOIN_NONE, "resume spectate:confirm settings leave" },
		{ TOUCHMODE_DEAD, 0, TOUCHJOIN_NONE, "resume spectate settings leave" },
		{ TOUCHMODE_OBSERVING, 0, TOUCHJOIN_OPEN, "resume join settings leave" },
		{ TOUCHMODE_FOLLOWING, TOUCHMODE_F_TEAMS, TOUCHJOIN_OPEN, "resume join team settings leave" },
		{ TOUCHMODE_OBSERVING, 0, TOUCHJOIN_PENDING, "resume join:disabled settings leave" },
		{ TOUCHMODE_OBSERVING, 0, TOUCHJOIN_QUEUED, "resume join:disabled settings leave" },
		{ TOUCHMODE_OBSERVING, 0, TOUCHJOIN_LOCKED, "resume join settings leave" },
		{ TOUCHMODE_OUT, TOUCHMODE_F_TEAMS, TOUCHJOIN_NONE, "resume spectate team settings leave" },
		{ TOUCHMODE_INTERMISSION, TOUCHMODE_F_TEAMS, TOUCHJOIN_NONE, "resume settings leave" },
		{ TOUCHMODE_MAPVOTE, 0, TOUCHJOIN_NONE, "resume settings leave" },
	};
	size_t i;
	char got[256], what[96];
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
	{
		touchmode_t m;
		m.mode = cases[i].mode;
		m.flags = cases[i].flags;
		m.join = cases[i].join;
		actions_string(&m, got, sizeof(got));
		snprintf(what, sizeof(what), "actions in %s (flags %d, join %s)", TouchMode_Name(m.mode), m.flags, TouchMode_JoinName(m.join));
		CHECK_STR(cases[i].expected, got, what);
	}
	{
		touchmode_t m = { TOUCHMODE_PLAYING, 0, TOUCHJOIN_NONE };
		CHECK_STR("spec", TouchAct_Command(&m, TOUCHACT_SPECTATE), "spectate runs the stock spec");
		CHECK_STR("join", TouchAct_Command(&m, TOUCHACT_JOIN), "join without teams");
		m.flags = TOUCHMODE_F_TEAMS;
		CHECK_STR("menu_showteamselect", TouchAct_Command(&m, TOUCHACT_JOIN), "join in a team game: team selection");
		CHECK_STR("Move to spectators", TouchAct_Label(TOUCHACT_SPECTATE), "label");
	}
}

// ---------------------------------------------------------------------------
// The join swipe, over traces on a 2400 x 1080 phone: (x, y, t) samples,
// the first the finger's landing, the last its release.
// ---------------------------------------------------------------------------
#define W 2400.0f
#define H 1080.0f
typedef struct { float x, y; double t; } sample_t;
typedef struct
{
	const char *what;
	int in_zone;
	int expected;
	int n;
	sample_t s[12];
} trace_t;

static const trace_t traces[] = {
	{ "a quick swipe up from the handle", 1, TOUCHSWIPE_ACCEPT, 5,
		{{1200, 1040, 0}, {1202, 1000, 0.04}, {1204, 950, 0.08}, {1205, 900, 0.12}, {1206, 860, 0.16}} },
	{ "a long, slower but deliberate swipe", 1, TOUCHSWIPE_ACCEPT, 5,
		{{1180, 1050, 0}, {1185, 1000, 0.15}, {1190, 940, 0.30}, {1192, 880, 0.45}, {1195, 820, 0.60}} },
	{ "released before the threshold (cancel)", 1, TOUCHSWIPE_SHORT, 4,
		{{1200, 1040, 0}, {1200, 1010, 0.05}, {1201, 980, 0.10}, {1201, 960, 0.15}} },
	{ "dragged up then back down (cancel)", 1, TOUCHSWIPE_SHORT, 5,
		{{1200, 1040, 0}, {1200, 950, 0.1}, {1200, 880, 0.2}, {1200, 960, 0.3}, {1200, 1000, 0.4}} },
	{ "far enough but too slow", 1, TOUCHSWIPE_SLOW, 5,
		{{1200, 1040, 0}, {1200, 1000, 0.5}, {1200, 960, 1.0}, {1200, 920, 1.5}, {1200, 880, 2.0}} },
	{ "diagonal", 1, TOUCHSWIPE_SLANTED, 5,
		{{1200, 1040, 0}, {1230, 1000, 0.04}, {1260, 960, 0.08}, {1290, 920, 0.12}, {1320, 870, 0.16}} },
	{ "sideways: a look, handed over", 1, TOUCHSWIPE_LOOK, 4,
		{{1200, 1040, 0}, {1240, 1038, 0.03}, {1300, 1035, 0.06}, {1400, 1030, 0.1}} },
	{ "downwards: a look", 1, TOUCHSWIPE_LOOK, 3,
		{{1200, 1000, 0}, {1200, 1030, 0.03}, {1200, 1060, 0.06}} },
	{ "looking around, then up (sideways first)", 1, TOUCHSWIPE_LOOK, 5,
		{{1200, 1040, 0}, {1260, 1040, 0.05}, {1300, 980, 0.1}, {1310, 900, 0.15}, {1320, 820, 0.2}} },
	{ "starting outside the zone (look area)", 0, -1, 4,
		{{1200, 700, 0}, {1200, 650, 0.04}, {1200, 560, 0.08}, {1200, 480, 0.12}} },
	{ "starting beside the zone (stick side)", 0, -1, 3,
		{{700, 1040, 0}, {700, 900, 0.05}, {700, 800, 0.1}} },
	{ "a tap on the handle", 1, TOUCHSWIPE_SHORT, 2,
		{{1200, 1040, 0}, {1201, 1041, 0.08}} },
};

static void check_swipes(void)
{
	size_t i;
	for (i = 0; i < sizeof(traces) / sizeof(traces[0]); i++)
	{
		const trace_t *tr = &traces[i];
		touchswipe_t sw;
		int k, st = TOUCHSWIPE_TRACKING;
		char what[128];
		snprintf(what, sizeof(what), "%s: in the start zone", tr->what);
		CHECK_INT(tr->in_zone, TouchSwipe_InZone(tr->s[0].x, tr->s[0].y, W, H), what);
		if (!tr->in_zone)
			continue;
		TouchSwipe_Begin(&sw, tr->s[0].x, tr->s[0].y, tr->s[0].t);
		for (k = 1; k < tr->n - 1 && st == TOUCHSWIPE_TRACKING; k++)
			st = TouchSwipe_Move(&sw, tr->s[k].x, tr->s[k].y, tr->s[k].t, H);
		if (st == TOUCHSWIPE_TRACKING)
			st = TouchSwipe_End(&sw, tr->s[tr->n - 1].x, tr->s[tr->n - 1].y, tr->s[tr->n - 1].t, H);
		snprintf(what, sizeof(what), "%s: %s", tr->what, TouchSwipe_Name(tr->expected));
		CHECK_STR(TouchSwipe_Name(tr->expected), TouchSwipe_Name(st), what);
	}
	// progress follows the finger: 0 at the start, 1 at the threshold
	{
		touchswipe_t sw;
		TouchSwipe_Begin(&sw, 1200, 1040, 0);
		TouchSwipe_Move(&sw, 1200, 1040 - H * TOUCHSWIPE_DIST * 0.5f, 0.05, H);
		CHECK_INT(50, (int)(TouchSwipe_Progress(&sw, H) * 100 + 0.5f), "half way");
		TouchSwipe_Move(&sw, 1200, 1040 - H * TOUCHSWIPE_DIST, 0.1, H);
		CHECK_INT(100, (int)(TouchSwipe_Progress(&sw, H) * 100 + 0.5f), "at the threshold");
	}
	// dp
	CHECK_INT(250, (int)(TouchDp_Density(400, 1080) * 100 + 0.5f), "400 dpi: 2.5 px per dp");
	CHECK_INT(100, (int)(TouchDp_Density(0, 720) * 100 + 0.5f), "no dpi, small screen: 1");
}

int main(void)
{
	check_modes();
	check_controls();
	check_actions();
	check_swipes();
	if (failures)
	{
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("touch hud modes: all checks passed\n");
	return 0;
}
