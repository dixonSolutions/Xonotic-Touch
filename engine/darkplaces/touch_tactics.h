/*
Godnotic: offline tactics. Only in a match against bots on this
machine (TouchWep_PracticeGate: this process runs the server, this client is
on it over the loopback, no other human is connected, no demo). See
touch_tactics.c.
*/
#ifndef TOUCH_TACTICS_H
#define TOUCH_TACTICS_H

#include "qtypes.h"

void TouchTactics_Init(void);

// Once a client frame, from the hook's frame before auto dodge: reads every
// enemy from the local server and, with the autopilot on, writes the move.
void TouchTactics_Frame(void);

// Button bits for the outgoing move (a jump over a low obstacle).
int TouchTactics_Buttons(void);

// Degrees by which the aim helpers count enemy `entnum` as nearer the
// crosshair: one that is nearly dead, aiming at the player, or carrying a
// strong weapon. 0 with the gate closed.
float TouchTactics_TargetBonus(int entnum);

// For the auto-switch: of the n owned weapons (netnames, ready flags), the
// position of one that finishes target `entnum` at `dist` with one shot, or,
// when it is behind a wall, of the best splash weapon. -1 = no opinion (the
// usual choice stands). Only with the gate open.
int TouchTactics_PickWeapon(int entnum, float dist, const char *const *names, const qbool *ready, int n, int heldpos);

// cl_touch_esp_enabled applied to an offline-gate result g: "off" closes it,
// "local" leaves it (there is no server mode).
int TouchTactics_EspGate(int g);

// True while the offline gate is open this frame (and the helpers may act).
qbool TouchTactics_Open(void);

// Offline only (cl_touch_practice_exact): the local server's own velocity of
// player n into vel, exact where the client only has drawn positions to
// smooth. False (vel untouched) with the gate closed or n not a live player.
qbool TouchTactics_ExactVelocity(int n, float *vel);

// Labels over every enemy: health, armour, weapon, and whether it aims at
// the player. Only with the gate open.
void TouchTactics_Draw(void);

#endif
