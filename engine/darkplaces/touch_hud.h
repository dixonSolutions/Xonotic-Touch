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

// Cvars and commands (CL_InitInput).
void TouchHUD_Init(void);

// Whether the engine HUD should be running right now.
qbool TouchHUD_Active(void);

// Per-frame input: reads the engine finger table, assigns roles, emits
// movement keys, view angles, attack/jump/crouch and the chrome pills.
void TouchHUD_Frame(void);
/* Controls hidden on a touchscreen: a finger drag still turns the view. */
void TouchHUD_LookWithoutControls(void);

// Draw the overlay (called from the 2D overlay pass, key_game only).
void TouchHUD_Draw(void);

// Stock healtharmor / ammo / weapons panels on exactly when the touch
// readouts are not drawn. Every frame, under any CSQC.
void TouchHUD_SyncStockPanels(void);

// Release every key this module is holding (disconnect, menu, CSQC swap).
void TouchHUD_ReleaseAll(void);
// Called when a CSQC finished loading or was unloaded: the weapon list and the
// WEAPONS stat index are read from its globals.
void TouchHUD_ProgsChanged(void);

// Read-only weapon state for touch auto-shoot (touch_aim.c). Indices run
// 0..TouchHUD_NumWeapons()-1 over the loaded CSQC's weapon registry.
int TouchHUD_NumWeapons(void);
const char *TouchHUD_WeaponName(int index); // netname, "" when out of range
qbool TouchHUD_WeaponOwned(int index);
int TouchHUD_WeaponAmmo(int index);         // reserve; -1 = unlimited or uses none
int TouchHUD_HeldWeapon(void);              // index being held (switched to), -1 unknown
int TouchHUD_HeldClip(void);                // rounds loaded, -1 = no clip
int TouchHUD_AltFireState(void);            // TOUCH_ALTFIRE_* (touch_altfire.h)

#endif
