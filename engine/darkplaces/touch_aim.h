/*
Xonotic Touch: auto-shoot and aim helpers for touch players.

Lives in the engine client, not in QuakeC, because on a public server the
client runs that server's own CSQC; the same code has to work there and in a
local match. Everything is inactive unless vid_touchscreen is on (no physical
keyboard, or touch mode forced to Always).

- Auto-shoot: while the crosshair is on a visible enemy player, the attack
  button bit is added to the outgoing move (CL_SendMove). It never touches the
  +attack key state, so a manual press always fires and an auto release can
  never cancel a finger that is holding FIRE.
- Aim friction: the touch look drag turns slower while the crosshair is on or
  near an enemy (engine HUD: look_apply; port CSQC: _cl_touch_aim_lookscale).
- Magnetism: while a finger is on the aim control and the player is turning
  or moving, the view is pulled a capped fraction toward the nearest visible
  enemy inside a small cone.
*/
#ifndef TOUCH_AIM_H
#define TOUCH_AIM_H

#include "qtypes.h"

void TouchAim_Init(void);

// Once per client frame, after the look input has been applied (end of
// CL_Input) and before the move is built.
void TouchAim_Frame(void);

// True while auto-shoot is holding fire (ORed into the move's attack bit, and
// the FIRE button glows).
qbool TouchAim_AutoFiring(void);

// Multiplier (0..1] for touch look deltas: aim friction near an enemy.
float TouchAim_LookScale(void);

// The engine-drawn HUD reports whether a finger is on the aim control (look
// drag or FIRE). The port's CSQC reports the same through _cl_touch_hud_state.
void TouchAim_SetHudAimTouch(qbool touching);

// A CSQC was loaded or unloaded: re-read which of its globals say teamplay.
void TouchAim_ProgsChanged(void);

#endif
