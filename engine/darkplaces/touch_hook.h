/*
Xonotic Touch: the grappling hook for touch players -- the HOOK button of both
touch HUDs, and the fall rescue that hooks a player back when they fall with
nothing below to land on.

Lives in the engine client, like touch_aim.c, because on a public server the
client runs that server's own CSQC; the same code works there and in a local
match. Everything is off unless vid_touchscreen is on and no mouse is aiming.

Which hook there is, read from the loaded CSQC every frame:
- the offhand hook (the server's g_grappling_hook): the CSQC's active_mutators
  global carries it (bit 8 in 0.8.6 and today's code), set from the server's
  welcome message whatever cl_welcome says. Held with button6 (+hook).
- the Hook weapon, owned (the WEAPONS stat) with fuel: switched to, fired with
  attack while held, and the previous weapon taken back on release.
A server with the nades or offhand blaster mutator gives +hook to those, so
there only the Hook weapon counts.
*/
#ifndef TOUCH_HOOK_H
#define TOUCH_HOOK_H

#include "qtypes.h"

// What hook the player has now (_cl_touch_hook_avail).
#define TOUCH_HOOK_NONE    0
#define TOUCH_HOOK_OFFHAND 1
#define TOUCH_HOOK_WEAPON  2

// Cvars and commands; called from TouchAim_Init.
void TouchHook_Init(void);
// A CSQC was loaded or unloaded (after touch_hud.c rebound its weapon list).
void TouchHook_ProgsChanged(void);

// Once per client frame, before the aim helpers (TouchAim_Frame): which hook
// there is, the rescue's decision and its view turn, the hold.
void TouchHook_Frame(void);
// After the aim helpers: the view the next frame's player turn is measured from.
void TouchHook_EndFrame(void);

// Button bits for the outgoing move (CL_SendMove): 32 (button6, +hook) for the
// offhand hook, 1 (attack) for the Hook weapon in hand.
int TouchHook_Buttons(void);

// TOUCH_HOOK_* while the touch controls may use it, else TOUCH_HOOK_NONE.
int TouchHook_Available(void);
// HOOK is on screen and takes touches (touch_hook_visible, availability, touch mode).
qbool TouchHook_ButtonShown(void);
// The hook is held now, by the HOOK button or by the rescue (the button glows).
qbool TouchHook_Held(void);
// Auto-shoot and auto-switch stand down: the hook is held, its weapon switch
// is on the way, or the previous weapon is being taken back.
qbool TouchHook_InUse(void);
// The rescue owns the view (aiming, holding or handing it back): the aim pull
// stands down.
qbool TouchHook_Rescuing(void);

// The engine HUD's HOOK finger.
void TouchHook_SetHudButton(qbool down);

#endif
