/*
Checks what the touch controls do while spectating
(engine/darkplaces/touch_spectate.h): the spectator bar's parts state by
state, that no spectator control can send +jump (the join request: only the
join swipe does, touch_hudmode.h), and the join's refusal lock.

Run through scripts/test-touch-spectate.sh.
*/

#include <stdio.h>
#include <string.h>

#include "touch_spectate.h"

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

int main(void)
{
	touchspec_join_t j;
	int state, part, jumped;
	double t;

	// the bar, state by state
	CHECK_STR("<", TouchSpec_BarLabel(TOUCHSPEC_OBSERVING, TOUCHSPEC_BAR_PREV), "free-fly prev");
	CHECK_STR("FOLLOW", TouchSpec_BarLabel(TOUCHSPEC_OBSERVING, TOUCHSPEC_BAR_MAIN), "free-fly main");
	CHECK_STR(">", TouchSpec_BarLabel(TOUCHSPEC_OBSERVING, TOUCHSPEC_BAR_NEXT), "free-fly next");
	for (part = 0; part < TOUCHSPEC_BAR_PARTS; part++)
	{
		CHECK_INT(TOUCHSPEC_ACT_FOLLOW, TouchSpec_BarAction(TOUCHSPEC_OBSERVING, part), "free-fly: every part follows");
		CHECK_INT(TOUCHSPEC_ACT_FOLLOW, TouchSpec_BarAction(TOUCHSPEC_OUT_FREE, part), "out free: every part follows");
		CHECK_INT(TOUCHSPEC_ACT_NONE, TouchSpec_BarAction(TOUCHSPEC_NONE, part), "playing: no spectator meaning");
	}
	CHECK_INT(TOUCHSPEC_ACT_PREV, TouchSpec_BarAction(TOUCHSPEC_FOLLOWING, TOUCHSPEC_BAR_PREV), "following prev");
	CHECK_INT(TOUCHSPEC_ACT_FREE, TouchSpec_BarAction(TOUCHSPEC_FOLLOWING, TOUCHSPEC_BAR_MAIN), "following main: free-fly");
	CHECK_INT(TOUCHSPEC_ACT_NEXT, TouchSpec_BarAction(TOUCHSPEC_FOLLOWING, TOUCHSPEC_BAR_NEXT), "following next");
	CHECK_STR("FREE", TouchSpec_BarLabel(TOUCHSPEC_FOLLOWING, TOUCHSPEC_BAR_MAIN), "following main label");
	CHECK_INT(TOUCHSPEC_ACT_PREV, TouchSpec_BarAction(TOUCHSPEC_OUT, TOUCHSPEC_BAR_PREV), "out prev");
	CHECK_INT(TOUCHSPEC_ACT_NONE, TouchSpec_BarAction(TOUCHSPEC_OUT, TOUCHSPEC_BAR_MAIN), "out main: the server keeps the view");
	CHECK_STR("WATCHING", TouchSpec_BarLabel(TOUCHSPEC_OUT, TOUCHSPEC_BAR_MAIN), "out main label");
	CHECK_INT(TOUCHSPEC_ACT_NEXT, TouchSpec_BarAction(TOUCHSPEC_OUT, TOUCHSPEC_BAR_NEXT), "out next");
	CHECK_STR("", TouchSpec_BarLabel(TOUCHSPEC_NONE, TOUCHSPEC_BAR_MAIN), "playing: no bar");

	// no part can mean jump (the join): bar actions are follow, next, prev, free only
	for (state = TOUCHSPEC_NONE; state <= TOUCHSPEC_OUT_FREE; state++)
		for (part = 0; part < TOUCHSPEC_BAR_PARTS; part++)
		{
			int act = TouchSpec_BarAction(state, part);
			CHECK_INT(1, act >= TOUCHSPEC_ACT_NONE && act <= TOUCHSPEC_ACT_FREE, "bar actions are follow / next / prev / free only");
		}

	CHECK_INT(1, TouchSpec_StickShown(TOUCHSPEC_OBSERVING), "stick observing");
	CHECK_INT(1, TouchSpec_StickShown(TOUCHSPEC_OUT_FREE), "stick out free");
	CHECK_INT(0, TouchSpec_StickShown(TOUCHSPEC_FOLLOWING), "stick following");
	CHECK_INT(0, TouchSpec_StickShown(TOUCHSPEC_OUT), "stick out");
	CHECK_INT(1, TouchSpec_JoinShown(TOUCHSPEC_OBSERVING), "join observing");
	CHECK_INT(1, TouchSpec_JoinShown(TOUCHSPEC_FOLLOWING), "join following");
	CHECK_INT(0, TouchSpec_JoinShown(TOUCHSPEC_OUT), "join out of the round");
	CHECK_INT(0, TouchSpec_JoinShown(TOUCHSPEC_OUT_FREE), "join out free");
	CHECK_INT(0, TouchSpec_JoinShown(TOUCHSPEC_NONE), "join playing");

	// nothing sent, nothing held
	memset(&j, 0, sizeof(j));
	CHECK_INT(0, TouchSpec_JoinFrame(&j, TOUCHSPEC_OBSERVING, 5.0), "no join: no jump");

	// sent: +jump for TOUCHSPEC_JOIN_PRESS, then released; taken, it clears
	memset(&j, 0, sizeof(j));
	TouchSpec_JoinSend(&j, 10.0);
	CHECK_INT(1, TouchSpec_JoinPending(&j), "pending after send");
	jumped = 0;
	for (t = 10.0; t < 10.0 + TOUCHSPEC_JOIN_PRESS + 0.1; t += 1.0 / 60)
		jumped += TouchSpec_JoinFrame(&j, TOUCHSPEC_OBSERVING, t);
	CHECK_INT(1, jumped >= 14 && jumped <= 16, "+jump held about 0.25 s at 60 fps");
	CHECK_INT(0, TouchSpec_JoinFrame(&j, TOUCHSPEC_NONE, 10.5), "playing: taken");
	CHECK_INT(0, TouchSpec_JoinPending(&j), "taken: not pending");
	CHECK_INT(0, TouchSpec_JoinLocked(&j, 11.0), "taken: not locked");

	// refused: still spectating after TOUCHSPEC_JOIN_WAIT, then LOCKED for a while
	memset(&j, 0, sizeof(j));
	TouchSpec_JoinSend(&j, 20.0);
	for (t = 20.0; t < 20.0 + TOUCHSPEC_JOIN_WAIT + 0.1; t += 1.0 / 30)
		TouchSpec_JoinFrame(&j, TOUCHSPEC_FOLLOWING, t);
	CHECK_INT(0, TouchSpec_JoinPending(&j), "refused: no longer pending");
	CHECK_INT(1, TouchSpec_JoinLocked(&j, 25.0), "refused: locked");
	CHECK_INT(0, TouchSpec_JoinLocked(&j, 20.0 + TOUCHSPEC_JOIN_WAIT + TOUCHSPEC_JOIN_LOCKED + 1), "lock expires");
	TouchSpec_JoinSend(&j, 26.0);
	CHECK_INT(0, TouchSpec_JoinLocked(&j, 26.5), "a retry clears the lock");

	// a slow server: the join is answered after the wait, so the lock went on;
	// once playing, the lock goes, and spectating later shows the handle open
	memset(&j, 0, sizeof(j));
	TouchSpec_JoinSend(&j, 40.0);
	for (t = 40.0; t < 44.0; t += 1.0 / 30)
		TouchSpec_JoinFrame(&j, TOUCHSPEC_OBSERVING, t);
	CHECK_INT(1, TouchSpec_JoinLocked(&j, 44.0), "slow answer: locked for now");
	TouchSpec_JoinFrame(&j, TOUCHSPEC_NONE, 44.5);
	CHECK_INT(0, TouchSpec_JoinLocked(&j, 50.0), "playing clears the lock");

	if (failures)
	{
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("touch spectate: all checks passed\n");
	return 0;
}
