/*
Xonotic Touch: engine-side copy of the Touch HUD.

The on-screen controls normally live in the port's CSQC (touch_*.qc). On a
public server the client has to run that server's own CSQC, so the HUD would
vanish there. This module draws and drives the same layout from the same cvars
and the same gfx/touch masks, so a server looks and plays like a local match.
It is active only while a foreign CSQC is loaded (see cl_touch_csqc_active),
with the controls on, or off on a touchscreen (strip, grip and pills only).
Under any CSQC it also makes a finger the pointer of the CSQC's own pointer
screens, such as the end-of-match vote (TouchHUD_CursorFrame).
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

// While the loaded CSQC shows its own pointer (a vote, the quick menu,
// hud_configure) a finger is that pointer and its taps are K_MOUSE1 clicks.
// CursorFinger is called from the SDL finger down / up events, CursorFrame once
// per IN_Move: it returns true while the pointer UI is up, and the touch
// controls then take no fingers. CursorFinger is true when the finger is
// taken (the loading screen) and must not reach the menu. FingerIsPointer: a finger placed the pointer
// last, so the desktop mouse position must not overwrite it until it moves.
qbool TouchHUD_CursorFinger(int id, qbool down, float nx, float ny);
qbool TouchHUD_CursorFrame(void);
qbool TouchHUD_FingerIsPointer(void);

// The loading screen (joining, downloading, a map change): a CANCEL pill on a
// touchscreen, drawn by DrawLoading; LoadingBack is Escape / Android Back
// there, true if it cancelled the join.
void TouchHUD_DrawLoading(void);
qbool TouchHUD_LoadingBack(void);
// A new local game stops after its map loads, before its server spawns (sv_main.c).
qbool TouchHUD_LoadingCancelled(void);
// SV_SpawnServer put the loading screen up: a local game's map load is a join
// CANCEL can leave (the app's startup and a renderer restart are not).
void TouchHUD_LocalGameLoading(void);
// SCR_PushLoadingScreen: CANCEL was tapped, so the join stops here, at the
// start of the next item loaded. Does not return when it stops it.
void TouchHUD_LoadingAbortPoint(void);

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
int TouchHUD_ActiveWeapon(void);            // index actually in hand (a switch done), -1 unknown
int TouchHUD_HeldClip(void);                // rounds loaded, -1 = no clip
int TouchHUD_AltFireState(void);            // TOUCH_ALTFIRE_* (touch_altfire.h)

#endif
