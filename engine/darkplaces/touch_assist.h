#ifndef TOUCH_ASSIST_H
#define TOUCH_ASSIST_H

#include "qtypes.h"

struct client_s;

// What a server allows Touch clients' assists to do (sv_touch_assist bits).
#define TOUCH_ASSIST_AIM        1 // aim pull and friction
#define TOUCH_ASSIST_AUTOSHOOT  2 // auto-shoot
#define TOUCH_ASSIST_AUTOSWITCH 4 // automatic weapon switching
#define TOUCH_ASSIST_ALL        7

// Server: tells a joining client what this server allows (SV_SendServerinfo).
void TouchAssist_SendServerinfo(struct client_s *client);

// Client
void TouchAssist_Init(void);
// Once a frame: forget the last server's word once disconnected.
void TouchAssist_Frame(void);
// Whether the server allows this assist; a server that says nothing allows all.
qbool TouchAssist_Allowed(int bit);

#endif
