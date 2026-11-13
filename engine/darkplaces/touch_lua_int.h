/*
Xonotic Touch: shared internals of the Lua sandbox (touch_lua.c) and its two
realms, client mods (touch_lua_cl.c) and server plugins (touch_lua_sv.c).
*/
#ifndef TOUCH_LUA_INT_H
#define TOUCH_LUA_INT_H

#include "touch_lua54/lua.h"
#include "touch_lua54/lauxlib.h"

#define TLUA_PERM_EVENTS    (1u << 0)
#define TLUA_PERM_COMMANDS  (1u << 1)
#define TLUA_PERM_CVAR_READ (1u << 2)
#define TLUA_PERM_CVAR_WRITE (1u << 3)
#define TLUA_PERM_EXEC      (1u << 4)
#define TLUA_PERM_STORAGE   (1u << 5)
#define TLUA_PERM_DISPLAY   (1u << 6)
#define TLUA_PERM_INPUT     (1u << 7)
#define TLUA_PERM_PLAYERS   (1u << 8)
#define TLUA_PERM_MESSAGES  (1u << 9)
#define TLUA_PERM_STATE     (1u << 10)
#define TLUA_PERM_ALL       ((1u << 11) - 1)

#define TLUA_MAX_MODS 32
#define TLUA_NAME_LEN 32
#define TLUA_MAX_FILE (1024 * 1024)      // init.lua, a required file, a load() chunk
#define TLUA_MAX_PATTERN_SUBJECT (1024 * 1024)

typedef enum tlua_state_e
{
	TLUA_OFF,       // disabled by the user (or never enabled)
	TLUA_RUNNING,
	TLUA_ERROR      // enabled, but stopped by an error or a limit
} tlua_state_t;

struct tlua_realm_s;

typedef struct tlua_mod_s
{
	char name[TLUA_NAME_LEN + 1];
	char dir[1024];                 // absolute, ends in '/'
	char title[64];
	char version[32];
	char author[64];
	char description[256];
	qbool enabled;                  // the user's choice (enabled.txt)
	unsigned requested, granted;
	qbool manifest_error, scoped;
	tlua_state_t state;
	char error[512];
	lua_State *L;
	size_t mem, memlimit;
	long long insns;                // instructions left in the current call
	qbool killed;
	char killreason[160];
	int prints;                     // print() lines left in the current call
	unsigned eventmask;             // bit per realm event with a handler
	int execs;                      // xt.exec calls left in the current call
	struct tlua_realm_s *realm;
} tlua_mod_t;

typedef struct tlua_realm_s
{
	const char *kind;               // "mod" / "plugin"
	const char *subdir;             // "mods" / "plugins"
	const char *cmdprefix;          // console commands mods register: <prefix><mod>_<name>
	unsigned cmdflags;              // CF_CLIENT / CF_SERVER
	const char *const *events;      // NULL terminated
	tlua_mod_t mods[TLUA_MAX_MODS];
	int nummods;
	qbool loaded;
	struct cvar_s *enable;          // master switch
	struct cvar_s *status;          // read-only summary for the menu (may be NULL)
	void (*openapi)(lua_State *L);  // adds realm functions to the xt table on the stack top
	qbool (*cvar_writable)(const char *name);
	qbool (*command_allowed)(const char *word, const char *line); // word: the command, lower case
} tlua_realm_t;

extern tlua_realm_t tlua_client, tlua_server;

// The mod whose code is running now (NULL between calls).
extern tlua_mod_t *tlua_current;

// touch_lua_vm.c asks this from the pattern matcher.
int TouchLua_PatternAbort(void);

// Realm management.
void TLua_Rescan(tlua_realm_t *r);
void TLua_LoadAll(tlua_realm_t *r);
void TLua_UnloadAll(tlua_realm_t *r, const char *why);
tlua_mod_t *TLua_Find(tlua_realm_t *r, const char *name);
qbool TLua_AnyHandler(tlua_realm_t *r, const char *event);

// Fires an event on every running mod of the realm. push() pushes the
// arguments and returns how many; result() sees each handler's single return
// value at the stack top (may be NULL). Both run inside a protected call.
typedef int (*tlua_push_f)(lua_State *L, void *ud);
typedef void (*tlua_result_f)(lua_State *L, tlua_mod_t *m, void *ud);
void TLua_Fire(tlua_realm_t *r, const char *event, tlua_push_f push, tlua_result_f result, void *ud);

// Helpers for realm API functions.
tlua_mod_t *TLua_Self(lua_State *L);
void TLua_Require(lua_State *L, unsigned permission);
void TLua_SetField(lua_State *L, const char *k, lua_CFunction f);
const char *TLua_CleanText(const char *in, char *out, size_t outsize, qbool allownewline);

// Cvars and commands, on the first hook call.
void TLua_InitOnce(void);

// Realm setup (touch_lua_cl.c / touch_lua_sv.c).
void TLua_CL_Setup(void);
void TLua_SV_Setup(void);
// The client frame stops the server's plugins once the local server is gone.
void TLua_SV_CheckStopped(void);

// Is the client drawing the 2D pass for mods now (xt.draw only works then).
extern qbool tlua_cl_drawing;

#endif
