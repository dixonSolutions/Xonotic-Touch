#ifndef TOUCH_FRONTEND_H
#define TOUCH_FRONTEND_H

/*
Godnotic: the engine side of the native front-end bridge. A native interface
(Kotlin views on Android) reads engine state as JSON snapshots and sends
intents as `fe ...` console commands; nothing outside the engine thread
touches engine structures. docs/FRONTEND_BRIDGE.md is the protocol.

Every function here runs on the engine thread. Upstream files call them from
single lines marked "// Touch:".
*/

#define TOUCHFE_PROTOCOL 1

// Where snapshots go. Android's nativeui/engine.c points this at the resident
// JNI bridge (libnativeui.so); without a sink (desktop) they are kept for
// fe_dump / fe_take and, with fe_echo 1, printed as they change.
typedef struct touchfe_sink_s
{
	void (*put)(const char *topic, const char *json, int event);
	long long (*beat_age_ms)(void); // ms since the UI's last heartbeat, -1 = never
	int (*cancel_requested)(void); // the UI asked to cancel a join (polled during loads)
} touchfe_sink_t;
void TouchFE_SetSink(const touchfe_sink_t *sink);

// For other engine modules (the UI bridge, TouchUIB_*): send a message on
// a topic of their own ("ui", ...) through the same transport, and receive
// the UI's `fe <verb> <json>` for a verb they own. A registered "ui" verb is
// what tells the native side that screens are described by the engine
// (the app topic's "ui_bridge"), so it attaches and takes the menu over.
void TouchFE_Put(const char *topic, const char *json, int event);
typedef void (*touchfe_verb_fn)(const char *json);
void TouchFE_RegisterVerb(const char *verb, touchfe_verb_fn fn);

// Cvars and commands (from TouchHook_Init).
void TouchFE_Init(void);
// Once per frame, before the SDL events (vid_sdl.c).
void TouchFE_Frame(void);
// CL_UpdateScreen (cl_screen.c): true while an opaque native screen covers
// the menu or a join, so nothing is drawn. Sound and the network go on.
qbool TouchFE_SkipDraw(void);
// CL_Frame's frame limiter (cl_main.c): cl_maxidlefps behind a native screen.
double TouchFE_MaxFPS(double maxfps);
// menu.c, MP_Draw and MP_KeyEvent: true while the native interface owns the
// menu, so the QuakeC menu is neither drawn nor given keys.
qbool TouchFE_MenuHidden(void);
// cl_screen.c, SCR_PushLoadingScreen: a load's progress for the native
// loading screen, and its Cancel, read while the engine thread is busy.
void TouchFE_Loading(const char *msg, float fraction);

#endif
