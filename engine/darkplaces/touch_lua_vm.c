/*
Xonotic Touch: the vendored Lua 5.4 (touch_lua54/, MIT) as one translation unit.

Only the libraries the sandbox offers are compiled in (see touch_lua54/README.md);
io, os, debug and package do not exist in this binary. Lua's internal functions
are static here, so only the public lua_* / luaL_* / luaopen_* API is visible to
the rest of the engine (touch_lua.c).

lstrlib.c gets one addition without editing it: every l_unlikely() check in it
(the pattern matcher's recursion check runs once per match() call, so once per
backtrack) first asks TouchLua_PatternAbort(), which turns true when the running
mod is past its time limit or already killed. A catastrophic pattern then stops
with "pattern too complex" instead of hanging the game inside one C call, where
the instruction-count hook cannot reach it.
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
#include "touch_lua54/ltablib.c"
#include "touch_lua54/lutf8lib.c"

int TouchLua_PatternAbort(void); /* touch_lua.c */
#undef l_unlikely
#define l_unlikely(x)	(TouchLua_PatternAbort() || luai_unlikely(x))
#include "touch_lua54/lstrlib.c"
#undef l_unlikely
#define l_unlikely(x)	luai_unlikely(x)
