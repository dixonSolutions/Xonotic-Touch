/*
Checks what the touch controls do while spectating
(engine/darkplaces/touch_spectate.h): each button's spectator meaning in each
state, that nothing but JOIN can send +jump (the join request), and the held
JOIN with its refusal lock.

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

static const char *label(int state, int slot)
{
	return TouchSpec_ActionLabel(TouchSpec_SlotAction(state, slot));
}

int main(void)
{
	touchspec_join_t j;
	int state, slot, frames, jumped;
	double t;

	// what each button says, state by state
	CHECK_STR("FOLLOW", label(TOUCHSPEC_OBSERVING, TOUCHSPEC_SLOT_FIRE), "observing FIRE");
	CHECK_STR("", label(TOUCHSPEC_OBSERVING, TOUCHSPEC_SLOT_ALT), "observing ALT");
	CHECK_STR("UP", label(TOUCHSPEC_OBSERVING, TOUCHSPEC_SLOT_HOP), "observing HOP");
	CHECK_STR("DOWN", label(TOUCHSPEC_OBSERVING, TOUCHSPEC_SLOT_DUCK), "observing DUCK");
	CHECK_STR("NEXT", label(TOUCHSPEC_FOLLOWING, TOUCHSPEC_SLOT_FIRE), "following FIRE");
	CHECK_STR("PREV", label(TOUCHSPEC_FOLLOWING, TOUCHSPEC_SLOT_ALT), "following ALT");
	CHECK_STR("FREE", label(TOUCHSPEC_FOLLOWING, TOUCHSPEC_SLOT_HOP), "following HOP");
	CHECK_STR("", label(TOUCHSPEC_FOLLOWING, TOUCHSPEC_SLOT_DUCK), "following DUCK");
	CHECK_STR("NEXT", label(TOUCHSPEC_OUT, TOUCHSPEC_SLOT_FIRE), "out FIRE");
	CHECK_STR("PREV", label(TOUCHSPEC_OUT, TOUCHSPEC_SLOT_ALT), "out ALT");
	CHECK_STR("", label(TOUCHSPEC_OUT, TOUCHSPEC_SLOT_HOP), "out HOP");
	CHECK_STR("FOLLOW", label(TOUCHSPEC_OUT_FREE, TOUCHSPEC_SLOT_FIRE), "out free FIRE");
	CHECK_STR("UP", label(TOUCHSPEC_OUT_FREE, TOUCHSPEC_SLOT_HOP), "out free HOP");
	for (slot = 0; slot < TOUCHSPEC_SLOTS; slot++)
		CHECK_INT(TOUCHSPEC_ACT_NONE, TouchSpec_SlotAction(TOUCHSPEC_NONE, slot), "playing: no spectator meaning");

	// the stick flies only when free-flying; JOIN only for a real spectator
	CHECK_INT(1, TouchSpec_StickShown(TOUCHSPEC_OBSERVING), "stick observing");
	CHECK_INT(1, TouchSpec_StickShown(TOUCHSPEC_OUT_FREE), "stick out free");
	CHECK_INT(0, TouchSpec_StickShown(TOUCHSPEC_FOLLOWING), "stick following");
	CHECK_INT(1, TouchSpec_JoinShown(TOUCHSPEC_OBSERVING), "join observing");
	CHECK_INT(1, TouchSpec_JoinShown(TOUCHSPEC_FOLLOWING), "join following");
	CHECK_INT(0, TouchSpec_JoinShown(TOUCHSPEC_OUT), "join out of the round");
	CHECK_INT(0, TouchSpec_JoinShown(TOUCHSPEC_OUT_FREE), "join out free");
	CHECK_INT(0, TouchSpec_JoinShown(TOUCHSPEC_NONE), "join playing");

	// a quick tap on JOIN does not join
	memset(&j, 0, sizeof(j));
	jumped = 0;
	for (t = 1.0, frames = 0; frames < 3; frames++, t += 1.0 / 30)
		jumped |= TouchSpec_JoinFrame(&j, TOUCHSPEC_OBSERVING, 1, t);
	jumped |= TouchSpec_JoinFrame(&j, TOUCHSPEC_OBSERVING, 0, t);
	CHECK_INT(0, jumped, "a tap on JOIN");
	CHECK_INT(0, (int)(TouchSpec_JoinProgress(&j, t) * 100), "progress cleared on release");

	// held long enough, +jump goes out for TOUCHSPEC_JOIN_PRESS, once
	memset(&j, 0, sizeof(j));
	jumped = 0;
	for (t = 1.0, frames = 0; frames < 60; frames++, t += 1.0 / 30)
		jumped += TouchSpec_JoinFrame(&j, TOUCHSPEC_OBSERVING, 1, t);
	CHECK_INT(1, jumped >= 6 && jumped <= 9, "held JOIN sends +jump for about 0.25 s");
	// the join landed: playing, nothing locked, nothing more sent
	CHECK_INT(0, TouchSpec_JoinFrame(&j, TOUCHSPEC_NONE, 0, t), "joined");
	CHECK_INT(0, TouchSpec_JoinLocked(&j, t + 5), "not locked after a join");

	// refused: still observing after the wait, JOIN shows locked for a while
	memset(&j, 0, sizeof(j));
	for (t = 1.0, frames = 0; frames < 20; frames++, t += 1.0 / 30)
		TouchSpec_JoinFrame(&j, TOUCHSPEC_OBSERVING, 1, t);
	for (frames = 0; frames < 120; frames++, t += 1.0 / 30)
		TouchSpec_JoinFrame(&j, TOUCHSPEC_OBSERVING, 0, t);
	CHECK_INT(1, TouchSpec_JoinLocked(&j, t), "refused join shows locked");
	CHECK_INT(0, TouchSpec_JoinLocked(&j, t + TOUCHSPEC_JOIN_LOCKED + 1), "lock wears off");

	// JOIN is not reachable out of the round
	memset(&j, 0, sizeof(j));
	jumped = 0;
	for (state = TOUCHSPEC_OUT; state <= TOUCHSPEC_OUT_FREE; state++)
		for (t = 1.0, frames = 0; frames < 60; frames++, t += 1.0 / 30)
			jumped |= TouchSpec_JoinFrame(&j, state, 1, t);
	CHECK_INT(0, jumped, "no join out of the round");

	if (failures)
	{
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("touch spectate: all checks passed\n");
	return 0;
}
