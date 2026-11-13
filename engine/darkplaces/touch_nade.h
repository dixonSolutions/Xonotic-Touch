#ifndef TOUCH_NADE_H
#define TOUCH_NADE_H

// Godnotic: the offhand grenade (the nades mutator) from the touch
// controls, for both HUDs: held to cook, let go to throw, thrown just before
// it would blow in the hand. See touch_nade.c.

#define TOUCH_NADE_HIDDEN   0  // no nades, or switched off
#define TOUCH_NADE_READY    1  // one to throw
#define TOUCH_NADE_COOKING  2  // in the hand
#define TOUCH_NADE_WAITING  3  // thrown, the next not yet in (estimated)

void TouchNade_Init(void);
void TouchNade_ProgsChanged(void);
void TouchNade_Frame(void);
// button6 (+hook) while the grenade is held.
int TouchNade_Buttons(void);
// For the engine HUD's NADE row.
int TouchNade_State(void);
float TouchNade_Timer(void);     // 0..1 of the fuse while cooking
int TouchNade_Bonus(void);       // bonus grenades in stock
void TouchNade_SetHudButton(qbool down);

#endif
