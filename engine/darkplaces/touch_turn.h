/*
Godnotic: the quick-turn button (TURN). One tap spins the view round by
touch_turn_angle degrees (180: face what was behind) over touch_turn_time
seconds, eased in and out, pitch kept. Done here, in the engine, for both touch
HUDs: the engine HUD calls TouchTurn_Start, the port's CSQC sends `touch_turn`.
The spin adds to whatever a look drag does in the same frames, and the aim
pull stands down while it runs (TouchHook_Rescuing).
*/
#ifndef TOUCH_TURN_H
#define TOUCH_TURN_H

#include "qtypes.h"

void TouchTurn_Init(void);
// Begin a turn (a second tap during one adds another turn).
void TouchTurn_Start(void);
// Once per client frame, before the aim helpers.
void TouchTurn_Frame(void);
// A turn is under way.
qbool TouchTurn_Spinning(void);
// TURN is on screen (touch_turn_visible, touch mode, no mouse aiming).
qbool TouchTurn_ButtonShown(void);

#endif
