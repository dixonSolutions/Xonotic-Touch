#ifndef TOUCH_NATIVEFE_H
#define TOUCH_NATIVEFE_H

/*
Godnotic native front end mode (touch_nativefe.c): the desktop app (the
libadwaita launcher) is the game's interface outside a match. It starts the
engine straight into a join, a hosted LAN game or a bot match; leaving the
match ends the engine with a result file the app shows. Every function is
called from one hook line in an upstream file, marked "// Touch:".

Off unless the environment says GODNOTIC_FRONTEND=native (or =manager);
then nothing here changes the engine.
*/

#include "qtypes.h"

// CL_Init: reads the environment and the secret fd, registers fe_* commands.
void TouchNativeFE_Init(void);
// CL_Frame: notices the end of the session (left, kicked, failed join) and quits.
void TouchNativeFE_Frame(void);
// touch_lan.c TouchLAN_Rejected: a password problem. True when native mode
// handles it (the engine quits and the app asks for the password), so the
// in-engine prompt must not open.
qbool TouchNativeFE_PasswordRejected(qbool wrong, const char *address);
// True in native front end mode.
qbool TouchNativeFE_Active(void);

#endif
