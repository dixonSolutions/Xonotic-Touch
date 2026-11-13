/*
Godnotic: the console button.

A small terminal icon in the top right corner opens the engine console (the
touch console sheet, touch_ui.c) and, tapped again, closes it. It is drawn by
the engine, so it is the same on Android, Linux and the web, works in the
menu, over the console and in a match, and needs no platform UI: Android's
old ">_" View and its white dialog are gone.

Placement: the top right corner, inside the safe area (display cutouts,
rounded corners) that the Android activity reports in _touch_safe_insets,
48 dp (touch_shell_button_size) with the density it reports. In a match with
the touch controls showing, the corner belongs to the score list and the
info messages, so the button joins the top bar on MENU's inner side (away
from CHAT and the vote chip); with the controls hidden it stays in the
corner, below CHAT if they would meet. With the console sheet open it sits
under the sheet's header. Hidden while a loading screen shows, and in a
match on a keyboard-and-mouse setup.

touch_shell_button: 0 never, 1 menus and console only, 2 also in a match.
*/
#ifndef TOUCH_SHELLBTN_H
#define TOUCH_SHELLBTN_H

#include "qtypes.h"

// Draws the button (2D overlay pass, last, over the console sheet).
void TouchShell_Draw(void);
/** Registers the button's cvars; call at engine start, before configs. */
void TouchShell_Init(void);

// SDL finger events, before the finger table: true when the finger is the
// button's and must be kept from everything else. type 0 down, 1 motion,
// 2 up; id as in the finger table (finger id + 1); nx, ny normalised.
qbool TouchShell_Finger(int type, int id, float nx, float ny);

// SDL mouse button events (window pixels): true when the click is the
// button's. synthetic: SDL made it up from a finger (that finger is handled
// by TouchShell_Finger, so the click is only swallowed).
qbool TouchShell_Mouse(qbool down, int button, qbool synthetic, float wx, float wy);

#endif
