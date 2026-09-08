/*
Xonotic Touch: engine-side copy of the Touch HUD.

The on-screen controls normally live in the port's CSQC (touch_*.qc). On a
public server the client has to run that server's own CSQC, so the HUD would
vanish there. This module draws and drives the same layout from the same cvars
and the same gfx/touch masks, so a server looks and plays like a local match.
It is active only while a foreign CSQC is loaded (see cl_touch_csqc_active).
*/
#ifndef TOUCH_HUD_H
#define TOUCH_HUD_H

#include "qtypes.h"

// Whether the engine HUD should be running right now.
qbool TouchHUD_Active(void);

// Per-frame input: reads the engine finger table, assigns roles, emits
// movement keys, view angles, attack/jump/crouch and the chrome pills.
void TouchHUD_Frame(void);

// Draw the overlay (called from the 2D overlay pass, key_game only).
void TouchHUD_Draw(void);

// Release every key this module is holding (disconnect, menu, CSQC swap).
void TouchHUD_ReleaseAll(void);

#endif
