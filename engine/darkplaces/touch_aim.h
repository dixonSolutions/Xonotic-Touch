/*
Godnotic: auto-shoot and aim helpers for touch players.

Lives in the engine client, not in QuakeC, because on a public server the
client runs that server's own CSQC; the same code has to work there and in a
local match. Everything is inactive unless vid_touchscreen is on (no physical
keyboard, or touch mode forced to Always).

- Auto-shoot: while the crosshair is on (or, within cl_touch_autoshoot_radius,
  near) a visible enemy player -- where a projectile will meet them -- the
  attack or attack2 button bit is added to the outgoing move (CL_SendMove),
  chosen per weapon (touch_weapons.h). It never touches the key state, so a
  manual press always fires and an auto release can never cancel a finger
  that is holding FIRE or ALT. Devastator rockets are steered and detonated.
- Auto-switch: the best owned weapon for the distance, under guards.
- Aim friction: the touch look drag turns slower while the crosshair is on or
  near an enemy (engine HUD: look_apply; port CSQC: _cl_touch_aim_lookscale).
- Magnetism: while a finger is on the aim control (or auto-shoot is firing at
  the target, or a guided rocket flies), the view follows part of the nearest
  visible enemy's motion inside a small cone and closes the gap to it -- to
  where the shot will meet it -- at a capped rate in degrees per second (it
  never snaps).
*/
#ifndef TOUCH_AIM_H
#define TOUCH_AIM_H

#include "qtypes.h"
// The hook's button bits go out with auto-shoot's (CL_SendMove).
#include "touch_hook.h"

void TouchAim_Init(void);

// Once per client frame, after the look input has been applied (end of
// CL_Input) and before the move is built.
void TouchAim_Frame(void);

// True while auto-shoot is holding fire (ORed into the move's attack bit, and
// the FIRE button glows).
qbool TouchAim_AutoFiring(void);

// True while auto-shoot is holding secondary fire (the move's button3 bit,
// +fire2): Shotgun melee, Crylink secondary, a Devastator detonation.
qbool TouchAim_AutoFire2(void);

// Multiplier (0..1] for touch look deltas: aim friction near an enemy.
float TouchAim_LookScale(void);

// The engine-drawn HUD reports whether a finger is on the aim control (look
// drag or FIRE). The port's CSQC reports the same through _cl_touch_hud_state.
void TouchAim_SetHudAimTouch(qbool touching);

// A CSQC was loaded or unloaded: re-read which of its globals say teamplay.
void TouchAim_ProgsChanged(void);

// Why the touch helpers may not act this frame (keyboard, mouse aiming, dead,
// spectating, menu...), or NULL when they may. The hook (touch_hook.c) obeys
// the same rules.
const char *TouchAim_BlockReason(void);

// Offline practice markers (enemies behind walls, off the screen in Full
// 360): drawn over either touch HUD, and only while the offline gate is open.
void TouchAim_Draw(void);

#endif
