/*
Checks the in-match HUD modes (engine/darkplaces/touch_hudmode.h): the mode
and join state derived from the client state, table-driven over every state;
the controls each mode has, in particular that the end-of-match scoreboard
and the next-map vote get MENU and nothing else; the MENU sheet's action
list; and the join dialog: when it opens by itself, when a game hosted
here joins straight away, and what each of its rows sends.

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
		CHECK_INT(0, c & TOUCHCTL_JOIN, "no join control on the HUD: joining is the join dialog");
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
		CHECK_STR("touch_join_dialog", TouchAct_Command(&m, TOUCHACT_JOIN), "join without teams: the join dialog");
		m.flags = TOUCHMODE_F_TEAMS;
		CHECK_STR("touch_join_dialog", TouchAct_Command(&m, TOUCHACT_JOIN), "join in a team game: the join dialog");
		CHECK_STR("touch_join_dialog", TouchAct_Command(&m, TOUCHACT_TEAM), "change team: the join dialog");
		CHECK_STR("Spectate", TouchAct_Label(TOUCHACT_SPECTATE), "label");
		CHECK_STR("Join game", TouchAct_Label(TOUCHACT_JOIN), "label");
	}
}

// ---------------------------------------------------------------------------
// The join dialog: owed once per match to a player who starts it as an
// observer. A frame every 50 ms.
// ---------------------------------------------------------------------------
static int run_dialog(touchjoindlg_t *d, int mode, int flags, int local, int campaign, int free_screen, int enabled, double from, double to, int *joins)
{
	int opens = 0;
	double t;
	for (t = from; t < to; t += 0.05)
	{
		int r = TouchJoinDlg_Frame(d, mode, flags, local, campaign, free_screen, enabled, t);
		if (r == TOUCHJOINDLG_OPEN)
			opens++;
		if (r == TOUCHJOINDLG_JOIN && joins)
			(*joins)++;
	}
	return opens;
}

static void check_join_dialog(void)
{
	touchjoindlg_t d;
	int joins;
	// a public server, Deathmatch: connecting, then an observer
	memset(&d, 0, sizeof(d));
	run_dialog(&d, TOUCHMODE_OFF, 0, 0, 0, 1, 1, 0, 1, NULL);
	CHECK_INT(0, run_dialog(&d, TOUCHMODE_DEAD, 0, 0, 0, 1, 1, 1, 1.3, NULL), "before the first scoreboard (reads as dead): nothing");
	CHECK_INT(0, run_dialog(&d, TOUCHMODE_OBSERVING, 0, 0, 0, 1, 1, 1.3, 1.9, NULL), "an observer for half a second: not yet");
	CHECK_INT(1, run_dialog(&d, TOUCHMODE_OBSERVING, 0, 0, 0, 1, 1, 1.9, 30, NULL), "an observer at match start: the dialog, once");
	// Spectate chosen: stays away for the rest of the match
	TouchJoinDlg_Answered(&d);
	CHECK_INT(0, run_dialog(&d, TOUCHMODE_FOLLOWING, 0, 0, 0, 1, 1, 30, 90, NULL), "after Spectate: never again in this match");
	// the next match owes it again
	run_dialog(&d, TOUCHMODE_INTERMISSION, 0, 0, 0, 1, 1, 90, 95, NULL);
	run_dialog(&d, TOUCHMODE_MAPVOTE, 0, 0, 0, 1, 1, 95, 100, NULL);
	CHECK_INT(1, run_dialog(&d, TOUCHMODE_OBSERVING, TOUCHMODE_F_TEAMS, 0, 0, 1, 1, 100, 110, NULL), "the next match, a team game: the dialog again");
	// in the game, then to the spectators by choice: no dialog
	memset(&d, 0, sizeof(d));
	run_dialog(&d, TOUCHMODE_OFF, 0, 0, 0, 1, 1, 0, 1, NULL);
	run_dialog(&d, TOUCHMODE_PLAYING, 0, 0, 0, 1, 1, 1, 5, NULL);
	CHECK_INT(0, run_dialog(&d, TOUCHMODE_OBSERVING, 0, 0, 0, 1, 1, 5, 30, NULL), "spectating after playing is the player's choice: no dialog");
	// the menu is up: it waits, then opens
	memset(&d, 0, sizeof(d));
	run_dialog(&d, TOUCHMODE_OFF, 0, 0, 0, 1, 1, 0, 1, NULL);
	CHECK_INT(0, run_dialog(&d, TOUCHMODE_OBSERVING, 0, 0, 0, 0, 1, 1, 10, NULL), "the menu has the screen: waits");
	CHECK_INT(1, run_dialog(&d, TOUCHMODE_OBSERVING, 0, 0, 0, 1, 1, 10, 12, NULL), "the menu closed: opens");
	// joining a match under way: the server's questions (statistics, its message) hold the
	// screen first, the view follows a player; the dialog is owed all the same
	memset(&d, 0, sizeof(d));
	run_dialog(&d, TOUCHMODE_OFF, TOUCHMODE_F_TEAMS, 0, 0, 1, 1, 0, 1, NULL);
	run_dialog(&d, TOUCHMODE_DEAD, TOUCHMODE_F_TEAMS, 0, 0, 0, 1, 1, 1.5, NULL);
	CHECK_INT(0, run_dialog(&d, TOUCHMODE_FOLLOWING, TOUCHMODE_F_TEAMS, 0, 0, 0, 1, 1.5, 9, NULL), "mid-match join, a question has the screen: waits");
	CHECK_INT(1, run_dialog(&d, TOUCHMODE_FOLLOWING, TOUCHMODE_F_TEAMS, 0, 0, 1, 1, 9, 40, NULL), "mid-match join: the dialog once the screen is free");
	TouchJoinDlg_Answered(&d);   // Spectate
	CHECK_INT(0, run_dialog(&d, TOUCHMODE_OBSERVING, TOUCHMODE_F_TEAMS, 0, 0, 1, 1, 40, 90, NULL), "mid-match join, Spectate chosen: no nagging");
	// switched off (touch_join_dialog_auto 0, or a desk without a touchscreen)
	memset(&d, 0, sizeof(d));
	run_dialog(&d, TOUCHMODE_OFF, 0, 0, 0, 1, 0, 0, 1, NULL);
	CHECK_INT(0, run_dialog(&d, TOUCHMODE_OBSERVING, 0, 0, 0, 1, 0, 1, 20, NULL), "not enabled here: never by itself");
	// a game hosted here, no teams: straight in, no dialog
	memset(&d, 0, sizeof(d));
	joins = 0;
	run_dialog(&d, TOUCHMODE_OFF, 0, 1, 0, 1, 1, 0, 1, NULL);
	CHECK_INT(0, run_dialog(&d, TOUCHMODE_OBSERVING, 0, 1, 0, 1, 1, 1, 3.4, &joins), "hosted here, no teams: no dialog");
	CHECK_INT(2, joins, "hosted here: join asked, again after 1.5 s while still an observer");
	run_dialog(&d, TOUCHMODE_PLAYING, 0, 1, 0, 1, 1, 3.4, 5, &joins);
	CHECK_INT(2, joins, "in the game: no more joins");
	// ... that the server never takes: the dialog in the end
	memset(&d, 0, sizeof(d));
	joins = 0;
	run_dialog(&d, TOUCHMODE_OFF, 0, 1, 0, 1, 1, 0, 1, NULL);
	CHECK_INT(1, run_dialog(&d, TOUCHMODE_OBSERVING, 0, 1, 0, 1, 1, 1, 30, &joins), "hosted here, joins never taken: the dialog after the tries");
	CHECK_INT(TOUCHJOINDLG_TRIES, joins, "all the tries first");
	// hosted here, a team game: the dialog (a team has to be picked)
	memset(&d, 0, sizeof(d));
	joins = 0;
	run_dialog(&d, TOUCHMODE_OFF, 0, 1, 0, 1, 1, 0, 1, NULL);
	CHECK_INT(1, run_dialog(&d, TOUCHMODE_OBSERVING, TOUCHMODE_F_TEAMS, 1, 0, 1, 1, 1, 10, &joins), "hosted here, a team game: the dialog");
	CHECK_INT(0, joins, "and no blind join");
	// the campaign joins by itself
	memset(&d, 0, sizeof(d));
	joins = 0;
	run_dialog(&d, TOUCHMODE_OFF, 0, 1, 1, 1, 1, 0, 1, NULL);
	CHECK_INT(0, run_dialog(&d, TOUCHMODE_OBSERVING, 0, 1, 1, 1, 1, 1, 10, &joins) + joins, "campaign: nothing from here");
	// what the rows send: the stock dialog's commands
	CHECK_STR("cmd join", TouchJoinDlg_Command("auto", 0), "Join, no teams");
	CHECK_STR("cmd selectteam auto; cmd join", TouchJoinDlg_Command("auto", 1), "auto-select");
	CHECK_STR("cmd selectteam red; cmd join red", TouchJoinDlg_Command("red", 1), "red");
	CHECK_STR("cmd selectteam pink; cmd join pink", TouchJoinDlg_Command("pink", 1), "pink");
	CHECK_STR("cmd spectate", TouchJoinDlg_Command("spectate", 1), "spectate");
	CHECK_STR("", TouchJoinDlg_Command("quit", 1), "anything else: nothing");
}

int main(void)
{
	check_modes();
	check_controls();
	check_actions();
	check_join_dialog();
	if (failures)
	{
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("touch hud modes: all checks passed\n");
	return 0;
}
