/*
Godnotic: the vendored Lua 5.4 (touch_lua54/, MIT) as one translation unit.

Only the libraries the sandbox offers are compiled in (see touch_lua54/README.md);
io, os, debug and package do not exist in this binary. Lua's internal functions
are static here, so only the public lua_* / luaL_* / luaopen_* API is visible to
the rest of the engine (touch_lua.c).

lstrlib.c gets one addition without editing it: every l_unlikely() check in it
(the pattern matcher's recursion check runs once per match() call, so once per
backtrack) first asks TouchLua_PatternAbort(), which turns true when the running
mod is past its time limit or already killed. A catastrophic pattern then stops
with "pattern too complex" instead of hanging the game inside one C call, where
the instruction-count hook cannot reach it. ltablib.c gets the same kind of
addition through lua_geti (below).

TouchLua_ResetAborted is the one function here that reaches into Lua's
internals: after Host_AbortCurrentFrame longjmps through a call into a mod
(loading CANCEL, Host_Error), its state still points at C frames that are
gone; this makes it safe to close.
*/

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wc++-compat"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Winline"
#endif

#include "touch_lua54/lprefix.h"
#define LUA_CORE
#define LUA_LIB
#include "touch_lua54/luaconf.h"

/* do not export Lua's internal functions */
#undef LUAI_FUNC
#undef LUAI_DDEC
#undef LUAI_DDEF
#define LUAI_FUNC	static
#define LUAI_DDEC(dec)	static dec
#define LUAI_DDEF	static

/* core */
#include "touch_lua54/lzio.c"
#include "touch_lua54/lctype.c"
#include "touch_lua54/lopcodes.c"
#include "touch_lua54/lmem.c"
#include "touch_lua54/lundump.c"
#include "touch_lua54/ldump.c"
#include "touch_lua54/lstate.c"
#include "touch_lua54/lgc.c"
#include "touch_lua54/llex.c"
#include "touch_lua54/lcode.c"
#include "touch_lua54/lparser.c"
#include "touch_lua54/ldebug.c"
#include "touch_lua54/lfunc.c"
#include "touch_lua54/lobject.c"
#include "touch_lua54/ltm.c"
#include "touch_lua54/lstring.c"
#include "touch_lua54/ltable.c"
#include "touch_lua54/ldo.c"
#include "touch_lua54/lvm.c"
#include "touch_lua54/lapi.c"

/* auxiliary library and the sandbox's libraries */
#include "touch_lua54/lauxlib.c"
#include "touch_lua54/lbaselib.c"
#include "touch_lua54/lcorolib.c"
#include "touch_lua54/lmathlib.c"
/* Every loop in ltablib.c (insert, remove, move, concat, unpack, sort) reads
   one element per step with lua_geti, which may run no Lua code at all (a C
   __index, or none), so the instruction hook never fires inside them: a
   table with a lying __len made table.insert loop for 2^63 steps. Each read
   is charged here to the running mod's instruction budget, and the time
   limit is checked every few hundred, so these loops end like Lua loops do. */
int TouchLua_WorkStep(lua_State *L); /* touch_lua.c */
#define lua_geti(L,idx,n)	(TouchLua_WorkStep(L), lua_geti(L,idx,n))
#include "touch_lua54/ltablib.c"
#undef lua_geti
#include "touch_lua54/lutf8lib.c"

int TouchLua_PatternAbort(void); /* touch_lua.c */
#undef l_unlikely
#define l_unlikely(x)	(TouchLua_PatternAbort() || luai_unlikely(x))
#include "touch_lua54/lstrlib.c"
#undef l_unlikely
#define l_unlikely(x)	luai_unlikely(x)

/* After a longjmp out of a call into this state (Host_AbortCurrentFrame), its
   main thread still records the dead C frames: the error handler chain, the
   C call depth and the current CallInfo. Reset them so lua_close can run
   (close_state also resets the CallInfo, and runs pending __close handlers
   under a protected call). */
void TouchLua_ResetAborted(lua_State *L)
{
	L = G(L)->mainthread;
	L->errorJmp = NULL;
	L->nCcalls = 0;
	L->ci = &L->base_ci;
	L->allowhook = 1;
}
