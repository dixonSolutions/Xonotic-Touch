/*
Godnotic: Lua 5.4 client mods and server mods (the code calls them plugins) in a sandbox.

Client mods live in <userdir>/mods/<name>/init.lua and server mods in
<userdir>/plugins/<name>/init.lua, on the real filesystem only: nothing is ever
loaded from the game's virtual filesystem, so a pk3 a server sends can never
carry code. A new folder starts disabled; lua_mod_enable / sv_lua_mod_enable
(or the Mods dialog) turn it on, and the choice is kept in enabled.txt next to
the folders.

Each mod runs in its own lua_State with only base, coroutine, table, string,
math and utf8; text-only load; require limited to its own folder; a memory
limit, an instruction limit and a time limit per call; cvar writes and console
commands by allowlist. A mod that errors or hits a limit is stopped and marked
with the error; the game goes on. docs/MODDING.md is the API reference.

The engine reaches this code through four single-line hooks in upstream files,
each marked // Touch: (a draw call in cl_screen.c, a key call in keys.c, the
builtin table in sv_main.c, server text in csprogs.c) plus one include in the
server QC; everything else,
including the console commands and cvars, is registered from here on first use.
That keeps the upstream merge surface to a handful of lines (docs/MODDING.md).
*/
#ifndef TOUCH_LUA_H
#define TOUCH_LUA_H

#include "qtypes.h"

struct prvm_prog_s;

// Every client frame, in the 2D pass after the game view and HUD, under the
// menu and console (SCR_DrawScreen): registers everything on the first call,
// loads/unloads client mods, runs "frame", "hud" (in a match) and "draw".
void TouchLua_ClientFrame(void);

// A key in the game (Key_Event, key_dest game, not Escape): true when a mod
// took it, and the binding must not run.
qbool TouchLua_KeyEvent(int key, int ascii, qbool down);

// Points the server VM at a builtin table with the Touch Lua builtin
// (SV_VM_Setup, after prog->builtins is set); registers everything on the
// first call, so a dedicated server gets the plugin cvars and commands too.
void TouchLua_SV_InstallBuiltins(struct prvm_prog_s *prog);

// Text a server put in the client's command buffer (svc_stufftext, from
// CL_VM_Parse_StuffCmd). The engine runs it as if typed at the console, so
// the mod manager notes it: until it has run, manager commands and the Lua
// limit cvars refuse changes (TLua_FromServer).
void TouchLua_ServerText(void);

// libcurl.c, for each transfer it sets up: the protocols a Lua mod install
// may use (HTTPS only, and for redirects too); 0 for every other transfer.
// The values are CURLPROTO_HTTP (1) and CURLPROTO_HTTPS (2).
// csprogs.c, CL_VM_Init: the client VM's Lua builtins (touch_lua_clapi.c)
void TouchLua_CL_InstallBuiltins(struct prvm_prog_s *prog);

// csprogs.c: a line the server printed / a centered message, before the game
// code shows it; true when a client mod hid it (touch_lua_clapi.c)
qbool TouchLua_CL_Print(const char *msg, size_t len);
qbool TouchLua_CL_CenterPrint(const char *msg, size_t len);

// csprogs.c, CL_VM_Parse_StuffCmd: a mod channel frame from the server
// ("//xt..." lines, touch_lua_chan.c); true when it was one (never run)
qbool TouchLua_CL_StuffCmd(const char *msg, size_t len);

// The platform sets this when it can vibrate (Android: the native UI agent's
// JNI code); xt.vibrate(ms) calls it. NULL: no vibration (xt.game.has).
extern qbool (*TouchLua_VibrateHook)(int ms);

long TouchLua_CurlProtocols(void (*callback)(int, size_t, unsigned char *, void *), qbool redirect);

#endif
