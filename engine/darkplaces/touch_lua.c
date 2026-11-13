/*
Xonotic Touch: the Lua sandbox and the mod manager (see touch_lua.h).

What a mod can reach is exactly what this file and the two realm files put in
its lua_State; everything else in Lua's libraries is either not compiled in
(touch_lua_vm.c) or removed here before the mod's first line runs:

- libraries: base, coroutine, table, string, math, utf8. No io, os, debug,
  package; no dofile, loadfile, string.dump, warn.
- load() takes text only (mode "t", never binary chunks), strings only.
- require(name) loads <mod folder>/<name with . as />.lua and nothing else.
- setmetatable() refuses __gc: finalizers run with hooks off (during GC and
  lua_close), where no limit could stop an endless loop.
- limits per call into the mod: memory (allocator), instructions (count hook),
  wall time (count hook, and the pattern matcher through touch_lua_vm.c);
  string pattern functions refuse subjects over 1 MB and patterns over 512 bytes.
- a mod that hits a limit is "killed": its hook then fires on every
  instruction and raises again, and pcall, xpcall, coroutine.resume/close and
  load re-raise instead of returning, so the mod cannot catch its own death.
  xpcall's message handler is skipped once killed (it would run inside the
  hook, where hooks are off).
- cvar writes go through an allowlist per realm plus the mod's own lua_<mod>_*
  namespace; private cvars and secrets cannot be read; console commands go
  through an allowlist and may not contain ; " $ \ or line breaks.
*/

#include "quakedef.h"
#include "cl_screen.h"
#include "touch_lua.h"
#include "touch_lua_int.h"
#include "touch_lua54/lualib.h"
#include "touch_lua_lib.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static cvar_t lua_memory_mb = {CF_CLIENT | CF_SERVER | CF_ARCHIVE, "lua_memory_mb", "16", "memory limit of each Lua mod or plugin in MB (1-64)"};
static cvar_t lua_instructions = {CF_CLIENT | CF_SERVER | CF_ARCHIVE, "lua_instructions", "2000000", "instruction limit of each call into a Lua mod (an event, a command); loading gets 10 times as many"};
static cvar_t lua_time_ms = {CF_CLIENT | CF_SERVER | CF_ARCHIVE, "lua_time_ms", "40", "time limit in milliseconds of each call into a Lua mod (1-250); loading gets 1 second"};
static cvar_t cl_lua_enable = {CF_CLIENT | CF_ARCHIVE, "cl_lua_enable", "1", "run the enabled Lua client mods from <userdir>/mods (lua_mods lists them)"};
static cvar_t sv_lua_enable = {CF_SERVER | CF_ARCHIVE, "sv_lua_enable", "1", "run the enabled Lua server plugins from <userdir>/plugins on servers this engine hosts (sv_lua_plugins lists them)"};
static cvar_t cl_lua_status = {CF_CLIENT | CF_READONLY, "_cl_lua_mods", "", "client mods for the Mods dialog: one line per mod, name<TAB>state<TAB>title<TAB>error<TAB>description"};
static cvar_t cl_lua_permissions = {CF_CLIENT | CF_READONLY, "_cl_lua_permissions", "", "client permissions: name<TAB>mode<TAB>requested<TAB>granted, one mod per line"};

tlua_mod_t *tlua_current;
static double tlua_deadline;
static unsigned tlua_patsteps;

static char tlua_key_events, tlua_key_cmds, tlua_key_loaded; // registry keys (addresses)

#define TLUA_HOOK_STEP 1000
#define TLUA_PRINTS_PER_CALL 64
#define TLUA_MAX_PATTERN 512

// ---------------------------------------------------------------------------
// Limits

static void tlua_kill(tlua_mod_t *m, const char *why)
{
	if (m->killed)
		return;
	m->killed = true;
	dp_strlcpy(m->killreason, why, sizeof(m->killreason));
}

tlua_mod_t *TLua_Self(lua_State *L)
{
	return *(tlua_mod_t **)lua_getextraspace(L);
}

static void *tlua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	tlua_mod_t *m = (tlua_mod_t *)ud;
	size_t old = ptr ? osize : 0;
	void *p;
	if (nsize == 0)
	{
		if (ptr)
		{
			m->mem -= old;
			free(ptr);
		}
		return NULL;
	}
	if (nsize > old)
	{
		if (m->killed)
			return NULL;
		if (m->mem - old + nsize > m->memlimit)
		{
			char why[64];
			dpsnprintf(why, sizeof(why), "memory limit (%i MB)", (int)(m->memlimit >> 20));
			tlua_kill(m, why);
			return NULL;
		}
	}
	p = realloc(ptr, nsize);
	if (!p)
		return nsize <= old ? ptr : NULL;
	m->mem = m->mem - old + nsize;
	return p;
}

static void tlua_hook(lua_State *L, lua_Debug *ar)
{
	tlua_mod_t *m = TLua_Self(L);
	(void)ar;
	if (!m->killed)
	{
		m->insns -= TLUA_HOOK_STEP;
		if (m->insns <= 0)
			tlua_kill(m, "instruction limit");
		else if (Sys_DirtyTime() > tlua_deadline)
			tlua_kill(m, "time limit");
	}
	if (m->killed)
	{
		// from now on every instruction of this thread raises again
		lua_sethook(L, tlua_hook, LUA_MASKCOUNT, 1);
		luaL_error(L, "%s", m->killreason);
	}
}

// touch_lua_vm.c: asked on every l_unlikely() in lstrlib.c
int TouchLua_PatternAbort(void)
{
	tlua_mod_t *m = tlua_current;
	if (!m)
		return 0;
	if (m->killed)
		return 1;
	if ((++tlua_patsteps & 15) != 0)
		return 0;
	if (Sys_DirtyTime() > tlua_deadline)
	{
		tlua_kill(m, "time limit");
		return 1;
	}
	return 0;
}

static int tlua_rethrow(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	return luaL_error(L, "%s", m->killreason);
}

static int tlua_panic(lua_State *L)
{
	const char *msg = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "?";
	Sys_Error("Lua: unprotected error (%s)", msg);
	return 0;
}

// ---------------------------------------------------------------------------
// The sandboxed base functions

static int tlua_print(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	char line[1024], clean[1024];
	int i, n = lua_gettop(L);
	line[0] = 0;
	if (m->prints <= 0)
		return 0;
	if (--m->prints == 0)
	{
		Con_Printf("^3[%s]^7 (more prints this call dropped)\n", m->name);
		return 0;
	}
	for (i = 1; i <= n; i++)
	{
		const char *s;
		switch (lua_type(L, i))
		{
		case LUA_TSTRING:
		case LUA_TNUMBER:
			s = lua_tostring(L, i); // a number turns into a string in place; harmless here
			break;
		case LUA_TBOOLEAN:
			s = lua_toboolean(L, i) ? "true" : "false";
			break;
		case LUA_TNIL:
			s = "nil";
			break;
		default:
			s = lua_typename(L, lua_type(L, i));
			break;
		}
		if (i > 1)
			dp_strlcat(line, " ", sizeof(line));
		dp_strlcat(line, s, sizeof(line));
	}
	Con_Printf("^3[%s]^7 %s\n", m->name, TLua_CleanText(line, clean, sizeof(clean), false));
	return 0;
}

static int tlua_finishpcall(lua_State *L, int status, lua_KContext extra)
{
	if (TLua_Self(L)->killed)
		return tlua_rethrow(L);
	if (status != LUA_OK && status != LUA_YIELD)
	{
		lua_pushboolean(L, 0);
		lua_pushvalue(L, -2);
		return 2;
	}
	return lua_gettop(L) - (int)extra;
}

static int tlua_pcall(lua_State *L)
{
	int status;
	luaL_checkany(L, 1);
	lua_pushboolean(L, 1);
	lua_insert(L, 1);
	status = lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0, tlua_finishpcall);
	return tlua_finishpcall(L, status, 0);
}

// xpcall's handler, wrapped: skipped once the mod is killed
static int tlua_msgh(lua_State *L)
{
	if (TLua_Self(L)->killed)
		return 1;
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_insert(L, 1);
	lua_call(L, lua_gettop(L) - 1, 1);
	return 1;
}

static int tlua_xpcall(lua_State *L)
{
	int status;
	int n = lua_gettop(L);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	lua_pushvalue(L, 2);
	lua_pushcclosure(L, tlua_msgh, 1);
	lua_replace(L, 2);
	lua_pushboolean(L, 1);
	lua_pushvalue(L, 1);
	lua_rotate(L, 3, 2);
	status = lua_pcallk(L, n - 2, LUA_MULTRET, 2, 2, tlua_finishpcall);
	return tlua_finishpcall(L, status, 2);
}

// calls upvalue 1 with all arguments, then re-raises if the mod got killed
static int tlua_passthrough(lua_State *L)
{
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_insert(L, 1);
	lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
	if (TLua_Self(L)->killed)
		return tlua_rethrow(L);
	return lua_gettop(L);
}

static int tlua_strguard(lua_State *L)
{
	size_t len;
	if (lua_type(L, 1) == LUA_TSTRING)
	{
		lua_tolstring(L, 1, &len);
		if (len > TLUA_MAX_PATTERN_SUBJECT)
			return luaL_error(L, "string too long for pattern functions (limit 1 MB)");
	}
	if (lua_type(L, 2) == LUA_TSTRING)
	{
		lua_tolstring(L, 2, &len);
		if (len > TLUA_MAX_PATTERN)
			return luaL_error(L, "pattern too long (limit %d bytes)", TLUA_MAX_PATTERN);
	}
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_insert(L, 1);
	lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
	if (!TLua_Self(L)->killed && Sys_DirtyTime() > tlua_deadline)
		tlua_kill(TLua_Self(L), "time limit");
	if (TLua_Self(L)->killed)
		return tlua_rethrow(L);
	return lua_gettop(L);
}

static int tlua_load(lua_State *L)
{
	size_t len;
	const char *s, *chunkname;
	int status;
	if (lua_type(L, 1) != LUA_TSTRING)
		return luaL_error(L, "load: only a string chunk is allowed here");
	s = lua_tolstring(L, 1, &len);
	if (len > TLUA_MAX_FILE)
		return luaL_error(L, "load: chunk too long (limit 1 MB)");
	chunkname = luaL_optstring(L, 2, "=(load)");
	// argument 3 (mode) is ignored: always text, never precompiled bytecode
	status = luaL_loadbufferx(L, s, len, chunkname, "t");
	if (TLua_Self(L)->killed)
		return tlua_rethrow(L);
	if (status != LUA_OK)
	{
		lua_pushnil(L);
		lua_insert(L, -2);
		return 2;
	}
	if (!lua_isnone(L, 4))
	{
		lua_pushvalue(L, 4);
		if (!lua_setupvalue(L, -2, 1))
			lua_pop(L, 1);
	}
	return 1;
}

static int tlua_setmetatable(lua_State *L)
{
	int t = lua_type(L, 2);
	luaL_checktype(L, 1, LUA_TTABLE);
	luaL_argexpected(L, t == LUA_TNIL || t == LUA_TTABLE, 2, "nil or table");
	if (t == LUA_TTABLE)
	{
		lua_pushliteral(L, "__gc");
		if (lua_rawget(L, 2) != LUA_TNIL)
			return luaL_error(L, "setmetatable: __gc metamethods are not allowed");
		lua_pop(L, 1);
	}
	if (luaL_getmetafield(L, 1, "__metatable") != LUA_TNIL)
		return luaL_error(L, "cannot change a protected metatable");
	lua_settop(L, 2);
	lua_setmetatable(L, 1);
	return 1;
}

static int tlua_collectgarbage(lua_State *L)
{
	const char *opt = luaL_optstring(L, 1, "collect");
	if (!strcmp(opt, "count"))
	{
		lua_pushnumber(L, (lua_Number)lua_gc(L, LUA_GCCOUNT) + (lua_Number)lua_gc(L, LUA_GCCOUNTB) / 1024.0);
		return 1;
	}
	if (!strcmp(opt, "collect"))
	{
		lua_gc(L, LUA_GCCOLLECT);
		lua_pushinteger(L, 0);
		return 1;
	}
	if (!strcmp(opt, "step"))
	{
		lua_pushboolean(L, lua_gc(L, LUA_GCSTEP, 0));
		return 1;
	}
	return luaL_error(L, "collectgarbage: only \"count\", \"collect\" and \"step\" are allowed");
}

static qbool tlua_validname(const char *s, qbool dots)
{
	const char *p;
	size_t n = strlen(s);
	if (n == 0 || n > 64)
		return false;
	for (p = s; *p; p++)
	{
		if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')
			continue;
		if (dots && *p == '.' && p != s && p[1] && p[1] != '.')
			continue;
		if (dots && *p >= 'A' && *p <= 'Z')
			continue;
		return false;
	}
	return true;
}

// Reads <dir><rel> into a userdata on the stack; returns its data or NULL.
static const char *tlua_readfile(lua_State *L, const char *dir, const char *rel, size_t *outlen)
{
	char path[2048];
	FILE *f;
	long size;
	char *buf;
	dpsnprintf(path, sizeof(path), "%s%s", dir, rel);
	f = fopen(path, "rb");
	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0)
		size = -1;
	fclose(f);
	if (size < 0 || size > TLUA_MAX_FILE)
	{
		luaL_error(L, "%s: unreadable or over 1 MB", rel);
		return NULL;
	}
	buf = (char *)lua_newuserdatauv(L, (size_t)size + 1, 0); // may raise (memory limit), no file is open then
	f = fopen(path, "rb");
	if (!f || (long)fread(buf, 1, (size_t)size, f) != size)
	{
		if (f)
			fclose(f);
		luaL_error(L, "%s: read error", rel);
		return NULL;
	}
	fclose(f);
	buf[size] = 0;
	*outlen = (size_t)size;
	return buf;
}

static int tlua_require(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	const char *name = luaL_checkstring(L, 1);
	char rel[256], chunk[320];
	const char *src;
	size_t len, i;
	if (!tlua_validname(name, true) || strlen(name) > 120)
		return luaL_error(L, "require: bad module name '%s' (letters, digits, _ and dots only)", name);
	lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_loaded);
	lua_pushvalue(L, 1);
	if (lua_rawget(L, -2) != LUA_TNIL)
		return 1;
	lua_pop(L, 1);
	if (!strcmp(name, "xt.ui") && m->realm == &tlua_client)
	{
		if (luaL_loadbufferx(L, tlua_lib_ui, sizeof(tlua_lib_ui) - 1, "=xt.ui", "t") != LUA_OK)
			return lua_error(L);
		lua_pushvalue(L, 1);
		lua_call(L, 1, 1);
		lua_pushvalue(L, 1);
		lua_pushvalue(L, -2);
		lua_rawset(L, 2);
		return 1;
	}
	dp_strlcpy(rel, name, sizeof(rel) - 4);
	for (i = 0; rel[i]; i++)
		if (rel[i] == '.')
			rel[i] = '/';
	dp_strlcat(rel, ".lua", sizeof(rel));
	src = tlua_readfile(L, m->dir, rel, &len);
	if (!src)
		return luaL_error(L, "require: module '%s' not found in this mod's folder (%s)", name, rel);
	dpsnprintf(chunk, sizeof(chunk), "@%s/%s", m->name, rel);
	if (luaL_loadbufferx(L, src, len, chunk, "t") != LUA_OK)
		return lua_error(L);
	lua_pushvalue(L, 1);
	lua_call(L, 1, 1);
	if (lua_isnil(L, -1))
	{
		lua_pop(L, 1);
		lua_pushboolean(L, 1);
	}
	lua_pushvalue(L, 1);
	lua_pushvalue(L, -2);
	lua_rawset(L, 2); // loaded[name] = result
	return 1;
}

// ---------------------------------------------------------------------------
// The common xt API

void TLua_SetField(lua_State *L, const char *k, lua_CFunction f)
{
	lua_pushcfunction(L, f);
	lua_setfield(L, -2, k);
}

const char *TLua_CleanText(const char *in, char *out, size_t outsize, qbool allownewline)
{
	size_t o = 0;
	for (; *in && o + 1 < outsize; in++)
	{
		unsigned char c = (unsigned char)*in;
		if (c == '\n' && allownewline)
			out[o++] = '\n';
		else if (c < 32 || c == 127)
			out[o++] = ' ';
		else
			out[o++] = (char)c;
	}
	out[o] = 0;
	return out;
}

static int tlua_eventindex(tlua_realm_t *r, const char *ev)
{
	int i;
	for (i = 0; r->events[i]; i++)
		if (!strcmp(r->events[i], ev))
			return i;
	return -1;
}

static unsigned *tlua_maskof(tlua_mod_t *m)
{
	return &m->eventmask;
}

typedef struct tlua_permission_s { const char *name; unsigned bit; } tlua_permission_t;
static const tlua_permission_t tlua_permissions[] = {
	{"events", TLUA_PERM_EVENTS}, {"commands", TLUA_PERM_COMMANDS},
	{"cvar_read", TLUA_PERM_CVAR_READ}, {"cvar_write", TLUA_PERM_CVAR_WRITE},
	{"exec", TLUA_PERM_EXEC}, {"storage", TLUA_PERM_STORAGE},
	{"display", TLUA_PERM_DISPLAY}, {"input", TLUA_PERM_INPUT},
	{"players", TLUA_PERM_PLAYERS}, {"messages", TLUA_PERM_MESSAGES},
	{"state", TLUA_PERM_STATE}, {NULL, 0}
};

void TLua_Require(lua_State *L, unsigned permission)
{
	tlua_mod_t *m = TLua_Self(L);
	int i;
	if ((m->requested & m->granted & permission) == permission)
		return;
	for (i = 0; tlua_permissions[i].name; i++)
		if (tlua_permissions[i].bit == permission)
			luaL_error(L, "permission denied: %s", tlua_permissions[i].name);
	luaL_error(L, "permission denied");
}

static int tlua_on(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	const char *ev = luaL_checkstring(L, 1);
	int idx = tlua_eventindex(m->realm, ev);
	TLua_Require(L, TLUA_PERM_EVENTS);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	if (idx < 0)
		return luaL_error(L, "xt.on: unknown event '%s'", ev);
	lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_events);
	lua_pushvalue(L, 1);
	if (lua_rawget(L, -2) != LUA_TTABLE)
	{
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, 1);
		lua_pushvalue(L, -2);
		lua_rawset(L, -4);
	}
	lua_pushvalue(L, 2);
	lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
	*tlua_maskof(m) |= 1u << idx;
	return 0;
}

// console commands mods register; never removed from the engine, so the
// dispatcher looks the mod up on each call
typedef struct tlua_cmd_s
{
	char full[96];
	tlua_realm_t *realm;
	char mod[TLUA_NAME_LEN + 1];
	char cmd[TLUA_NAME_LEN + 1];
} tlua_cmd_t;
static tlua_cmd_t tlua_cmds[256];
static int tlua_numcmds;

static void tlua_modcmd_f(cmd_state_t *cmd);

static int tlua_command(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	const char *name = luaL_checkstring(L, 1);
	const char *help = luaL_optstring(L, 3, "Lua mod command");
	char full[96], desc[160];
	int i, mine = 0;
	TLua_Require(L, TLUA_PERM_COMMANDS);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	if (!tlua_validname(name, false) || strlen(name) > TLUA_NAME_LEN)
		return luaL_error(L, "xt.command: bad name '%s' (a-z, 0-9, _)", name);
	dpsnprintf(full, sizeof(full), "%s%s_%s", m->realm->cmdprefix, m->name, name);
	for (i = 0; i < tlua_numcmds; i++)
	{
		if (!strcmp(tlua_cmds[i].full, full))
			break;
		if (!strcmp(tlua_cmds[i].mod, m->name) && tlua_cmds[i].realm == m->realm)
			mine++;
	}
	if (i == tlua_numcmds)
	{
		if (mine >= 32 || tlua_numcmds >= (int)(sizeof(tlua_cmds) / sizeof(tlua_cmds[0])))
			return luaL_error(L, "xt.command: too many commands");
		if (Cmd_Exists(cmd_local, full) || Cvar_FindVar(&cvars_all, full, ~0))
			return luaL_error(L, "xt.command: '%s' already exists", full);
		dp_strlcpy(tlua_cmds[i].full, full, sizeof(tlua_cmds[i].full));
		tlua_cmds[i].realm = m->realm;
		dp_strlcpy(tlua_cmds[i].mod, m->name, sizeof(tlua_cmds[i].mod));
		dp_strlcpy(tlua_cmds[i].cmd, name, sizeof(tlua_cmds[i].cmd));
		tlua_numcmds++;
		TLua_CleanText(help, desc, sizeof(desc), false);
		// the engine keeps the name and description pointers
		Cmd_AddCommand(m->realm->cmdflags, tlua_cmds[i].full, tlua_modcmd_f, Z_strdup(desc));
	}
	lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_cmds);
	lua_pushvalue(L, 1);
	lua_pushvalue(L, 2);
	lua_rawset(L, -3);
	lua_pushstring(L, full);
	return 1;
}

static qbool tlua_cvar_secret(const char *name)
{
	static const char *const bad[] = {"password", "rcon", "secret", "token", "crypto", "_key", "log_dest", "cookie", "auth", NULL};
	int i;
	char lower[256];
	size_t k;
	for (k = 0; name[k] && k + 1 < sizeof(lower); k++)
		lower[k] = (char)tolower((unsigned char)name[k]);
	lower[k] = 0;
	for (i = 0; bad[i]; i++)
		if (strstr(lower, bad[i]))
			return true;
	return false;
}

static int tlua_cvar(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	cvar_t *v = Cvar_FindVar(&cvars_all, name, ~0);
	TLua_Require(L, TLUA_PERM_CVAR_READ);
	if (!v || (v->flags & CF_PRIVATE) || tlua_cvar_secret(name))
		return 0;
	lua_pushstring(L, v->string);
	return 1;
}

static qbool tlua_owncvar(tlua_mod_t *m, const char *name)
{
	char prefix[64];
	dpsnprintf(prefix, sizeof(prefix), "lua_%s_", m->name);
	return !strncmp(name, prefix, strlen(prefix)) && tlua_validname(name + strlen(prefix), false);
}

static qbool tlua_cleanvalue(const char *s)
{
	if (strlen(s) > 256)
		return false;
	for (; *s; s++)
		if ((unsigned char)*s < 32 || *s == '"' || *s == ';' || *s == '$' || *s == '\\')
			return false;
	return true;
}

static int tlua_setcvar(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	const char *name = luaL_checkstring(L, 1);
	const char *value;
	cvar_t *v;
	TLua_Require(L, TLUA_PERM_CVAR_WRITE);
	if (lua_type(L, 2) == LUA_TBOOLEAN)
		value = lua_toboolean(L, 2) ? "1" : "0";
	else
		value = luaL_checkstring(L, 2);
	if (!tlua_cleanvalue(value))
		return luaL_error(L, "xt.setcvar: value has a forbidden character or is too long");
	v = Cvar_FindVar(&cvars_all, name, ~0);
	if (tlua_owncvar(m, name))
	{
		if (!v)
		{
			Cvar_Get(&cvars_all, name, value, m->realm->cmdflags | CF_ARCHIVE, "Lua mod setting");
			lua_pushboolean(L, 1);
			return 1;
		}
	}
	else if (!m->realm->cvar_writable(name) || tlua_cvar_secret(name))
		return luaL_error(L, "xt.setcvar: '%s' is not on the allowlist (see docs/MODDING.md)", name);
	if (!v || (v->flags & (CF_PRIVATE | CF_READONLY)))
		return luaL_error(L, "xt.setcvar: no writable cvar '%s'", name);
	Cvar_SetQuick(v, value);
	lua_pushboolean(L, 1);
	return 1;
}

static int tlua_exec(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	const char *line = luaL_checkstring(L, 1);
	char word[64];
	const char *p;
	size_t n = 0;
	TLua_Require(L, TLUA_PERM_EXEC);
	if (strlen(line) > 256)
		return luaL_error(L, "xt.exec: command too long");
	for (p = line; *p; p++)
		if ((unsigned char)*p < 32 || *p == ';' || *p == '"' || *p == '$' || *p == '\\')
			return luaL_error(L, "xt.exec: ; \" $ \\ and control characters are not allowed");
	for (p = line; *p == ' '; p++)
		;
	while (*p && *p != ' ' && n + 1 < sizeof(word))
		word[n++] = (char)tolower((unsigned char)*p++);
	word[n] = 0;
	if (!n || !m->realm->command_allowed(word, line))
		return luaL_error(L, "xt.exec: '%s' is not on the allowlist (see docs/MODDING.md)", word);
	if (m->execs <= 0)
		return luaL_error(L, "xt.exec: too many commands in one call");
	m->execs--;
	{
		char text[300];
		dpsnprintf(text, sizeof(text), "%s\n", line);
		Cbuf_AddText(cmd_local, text);
	}
	return 0;
}

// xt.save(key, text) / xt.load(key): small text files in <mod folder>/data/
#define TLUA_MAX_SAVE (64 * 1024)
#define TLUA_MAX_SAVEFILES 64

static const char *tlua_savekey(lua_State *L, char *rel, size_t relsize)
{
	const char *key = luaL_checkstring(L, 1);
	if (!tlua_validname(key, false) || strlen(key) > TLUA_NAME_LEN)
	{
		luaL_error(L, "bad key '%s' (a-z, 0-9, _; up to 32)", key);
		return NULL;
	}
	dpsnprintf(rel, relsize, "data/%s.txt", key);
	return key;
}

static int tlua_save(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	char rel[64], path[2048];
	size_t len;
	const char *text;
	FILE *f;
	TLua_Require(L, TLUA_PERM_STORAGE);
	tlua_savekey(L, rel, sizeof(rel));
	text = luaL_checklstring(L, 2, &len);
	if (len > TLUA_MAX_SAVE)
		return luaL_error(L, "xt.save: over 64 KB");
	dpsnprintf(path, sizeof(path), "%s%s", m->dir, rel);
	f = fopen(path, "rb");
	if (f)
		fclose(f);
	else
	{
		// a new key: keep the number of files bounded
		char dir[1100];
		stringlist_t list;
		int n;
		dpsnprintf(dir, sizeof(dir), "%sdata/", m->dir);
		stringlistinit(&list);
		listdirectory(&list, dir, "");
		n = list.numstrings;
		stringlistfreecontents(&list);
		if (n >= TLUA_MAX_SAVEFILES)
			return luaL_error(L, "xt.save: too many keys (%d)", TLUA_MAX_SAVEFILES);
		FS_CreatePath(path);
	}
	f = fopen(path, "wb");
	if (!f)
		return luaL_error(L, "xt.save: cannot write %s", rel);
	if (len && fwrite(text, 1, len, f) != len)
	{
		fclose(f);
		return luaL_error(L, "xt.save: write error");
	}
	fclose(f);
	lua_pushboolean(L, 1);
	return 1;
}

static int tlua_load_saved(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	char rel[64];
	size_t len;
	const char *buf;
	TLua_Require(L, TLUA_PERM_STORAGE);
	tlua_savekey(L, rel, sizeof(rel));
	buf = tlua_readfile(L, m->dir, rel, &len);
	if (!buf)
		return 0;
	lua_pushlstring(L, buf, len);
	return 1;
}

static int tlua_realtime(lua_State *L)
{
	TLua_Require(L, TLUA_PERM_STATE);
	lua_pushnumber(L, host.realtime);
	return 1;
}

static void tlua_openxt(lua_State *L, tlua_mod_t *m)
{
	lua_newtable(L);
	TLua_SetField(L, "on", tlua_on);
	TLua_SetField(L, "command", tlua_command);
	TLua_SetField(L, "cvar", tlua_cvar);
	TLua_SetField(L, "setcvar", tlua_setcvar);
	TLua_SetField(L, "exec", tlua_exec);
	TLua_SetField(L, "realtime", tlua_realtime);
	TLua_SetField(L, "save", tlua_save);
	TLua_SetField(L, "load", tlua_load_saved);
	lua_pushstring(L, m->name);
	lua_setfield(L, -2, "modname");
	lua_pushstring(L, m->realm == &tlua_server ? "server" : "client");
	lua_setfield(L, -2, "realm");
	lua_pushstring(L, "1");
	lua_setfield(L, -2, "apiversion");
	m->realm->openapi(L);
	lua_setglobal(L, "xt");
}

// ---------------------------------------------------------------------------
// States and calls

static void tlua_nil(lua_State *L, int table, const char *k)
{
	lua_pushnil(L);
	lua_setfield(L, table, k);
}

static void tlua_wrap(lua_State *L, int table, const char *k, lua_CFunction f)
{
	lua_getfield(L, table, k);
	lua_pushcclosure(L, f, 1);
	lua_setfield(L, table, k);
}

static int tlua_setup(lua_State *L)
{
	tlua_mod_t *m = (tlua_mod_t *)lua_touserdata(L, 1);
	const char *src;
	size_t len;
	char chunk[64];
	int g;

	luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
	luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
	luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
	luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
	luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
	luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
	lua_settop(L, 1);

	lua_pushglobaltable(L);
	g = lua_gettop(L);
	tlua_nil(L, g, "dofile");
	tlua_nil(L, g, "loadfile");
	tlua_nil(L, g, "warn");
	TLua_SetField(L, "print", tlua_print);
	TLua_SetField(L, "pcall", tlua_pcall);
	TLua_SetField(L, "xpcall", tlua_xpcall);
	TLua_SetField(L, "load", tlua_load);
	TLua_SetField(L, "require", tlua_require);
	TLua_SetField(L, "setmetatable", tlua_setmetatable);
	TLua_SetField(L, "collectgarbage", tlua_collectgarbage);

	lua_getfield(L, g, "coroutine");
	tlua_wrap(L, lua_gettop(L), "resume", tlua_passthrough);
	tlua_wrap(L, lua_gettop(L), "close", tlua_passthrough);
	lua_pop(L, 1);

	lua_getfield(L, g, "string");
	tlua_nil(L, lua_gettop(L), "dump");
	tlua_wrap(L, lua_gettop(L), "find", tlua_strguard);
	tlua_wrap(L, lua_gettop(L), "match", tlua_strguard);
	tlua_wrap(L, lua_gettop(L), "gmatch", tlua_strguard);
	tlua_wrap(L, lua_gettop(L), "gsub", tlua_strguard);
	lua_pop(L, 1);

	// the loaded-libraries table luaL_requiref keeps in the registry holds the
	// originals; replace it with the module cache require() uses
	lua_pushnil(L);
	lua_setfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &tlua_key_loaded);
	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &tlua_key_events);
	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &tlua_key_cmds);

	tlua_openxt(L, m);
	lua_settop(L, 1);

	src = tlua_readfile(L, m->dir, "init.lua", &len);
	if (!src)
		return luaL_error(L, "init.lua not found");
	dpsnprintf(chunk, sizeof(chunk), "@%s/init.lua", m->name);
	if (luaL_loadbufferx(L, src, len, chunk, "t") != LUA_OK)
		return lua_error(L);
	lua_call(L, 0, 0);
	return 0;
}

static void tlua_close(tlua_mod_t *m)
{
	if (!m->L)
		return;
	// keep the killed state's hook: a __close handler run by lua_close then
	// raises at once instead of looping
	m->killed = true;
	if (!m->killreason[0])
		dp_strlcpy(m->killreason, "mod stopped", sizeof(m->killreason));
	lua_sethook(m->L, tlua_hook, LUA_MASKCOUNT, 1);
	tlua_current = m;
	lua_close(m->L);
	tlua_current = NULL;
	m->L = NULL;
	m->mem = 0;
	*tlua_maskof(m) = 0;
}

static void tlua_fail(tlua_mod_t *m, const char *msg)
{
	char clean[512];
	TLua_CleanText(msg, clean, sizeof(clean), false);
	dp_strlcpy(m->error, clean, sizeof(m->error));
	Con_Printf(CON_ERROR "Lua %s %s stopped: %s\n", m->realm->kind, m->name, m->error);
	m->state = TLUA_ERROR;
	tlua_close(m);
}

static void tlua_updatestatus(tlua_realm_t *r);

// Runs f(L) with ud as its argument, under the mod's limits. Returns true when
// it finished; on any error the mod is stopped and marked.
static qbool tlua_run(tlua_mod_t *m, lua_CFunction f, void *ud, qbool loading)
{
	int status;
	double ms;
	if (!m->L || tlua_current)
		return false;
	ms = bound(1, lua_time_ms.value, 250);
	m->insns = (long long)bound(10000, lua_instructions.value, 50000000) * (loading ? 10 : 1);
	m->prints = TLUA_PRINTS_PER_CALL * (loading ? 16 : 1);
	m->execs = 8;
	tlua_deadline = Sys_DirtyTime() + (loading ? 1.0 : ms / 1000.0);
	tlua_current = m;
	lua_settop(m->L, 0);
	lua_pushcfunction(m->L, f);
	lua_pushlightuserdata(m->L, ud);
	status = lua_pcall(m->L, 1, 0, 0);
	tlua_current = NULL;
	if (status != LUA_OK || m->killed)
	{
		char msg[512];
		if (m->killed)
			dpsnprintf(msg, sizeof(msg), "%s", m->killreason);
		else if (lua_type(m->L, -1) == LUA_TSTRING)
			dp_strlcpy(msg, lua_tostring(m->L, -1), sizeof(msg));
		else
			dpsnprintf(msg, sizeof(msg), "error object is a %s", luaL_typename(m->L, -1));
		tlua_fail(m, msg);
		tlua_updatestatus(m->realm);
		return false;
	}
	lua_settop(m->L, 0);
	return true;
}

static int tlua_setup_tramp(lua_State *L)
{
	return tlua_setup(L);
}

static void tlua_start(tlua_mod_t *m)
{
	lua_State *L;
	if (m->L)
		return;
	if (m->manifest_error)
	{
		tlua_fail(m, "invalid or unsupported Lua API manifest");
		return;
	}
	m->killed = false;
	m->killreason[0] = 0;
	m->error[0] = 0;
	m->mem = 0;
	m->memlimit = (size_t)bound(1, lua_memory_mb.integer, 64) << 20;
	*tlua_maskof(m) = 0;
	L = lua_newstate(tlua_alloc, m);
	if (!L)
	{
		tlua_fail(m, "could not create a Lua state");
		return;
	}
	*(tlua_mod_t **)lua_getextraspace(L) = m;
	lua_atpanic(L, tlua_panic);
	lua_sethook(L, tlua_hook, LUA_MASKCOUNT, TLUA_HOOK_STEP);
	m->L = L;
	m->state = TLUA_RUNNING;
	if (tlua_run(m, tlua_setup_tramp, m, true))
		Con_Printf("Lua %s %s loaded (%i KB)\n", m->realm->kind, m->name, (int)(m->mem >> 10));
}

typedef struct tlua_fire_s
{
	const char *event;
	tlua_push_f push;
	tlua_result_f result;
	void *ud;
	tlua_mod_t *m;
} tlua_fire_t;

static int tlua_fire_tramp(lua_State *L)
{
	tlua_fire_t *f = (tlua_fire_t *)lua_touserdata(L, 1);
	lua_Integer i, n;
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_events) != LUA_TTABLE)
		return 0;
	lua_pushstring(L, f->event);
	if (lua_rawget(L, -2) != LUA_TTABLE)
		return 0;
	n = (lua_Integer)lua_rawlen(L, -1);
	for (i = 1; i <= n; i++)
	{
		int nargs;
		if (lua_rawgeti(L, -1, i) != LUA_TFUNCTION)
		{
			lua_pop(L, 1);
			continue;
		}
		nargs = f->push ? f->push(L, f->ud) : 0;
		lua_call(L, nargs, 1);
		if (f->result)
			f->result(L, f->m, f->ud);
		lua_pop(L, 1);
	}
	return 0;
}

static void tlua_fire_one(tlua_mod_t *m, const char *event, tlua_push_f push, tlua_result_f result, void *ud)
{
	tlua_fire_t f;
	int idx = tlua_eventindex(m->realm, event);
	if (m->state != TLUA_RUNNING || !m->L || idx < 0 || !(m->requested & m->granted & TLUA_PERM_EVENTS) || !(*tlua_maskof(m) & (1u << idx)))
		return;
	f.event = event;
	f.push = push;
	f.result = result;
	f.ud = ud;
	f.m = m;
	tlua_run(m, tlua_fire_tramp, &f, false);
}

void TLua_Fire(tlua_realm_t *r, const char *event, tlua_push_f push, tlua_result_f result, void *ud)
{
	int i;
	for (i = 0; i < r->nummods; i++)
		tlua_fire_one(&r->mods[i], event, push, result, ud);
}

qbool TLua_AnyHandler(tlua_realm_t *r, const char *event)
{
	int i, idx = tlua_eventindex(r, event);
	if (idx < 0)
		return false;
	for (i = 0; i < r->nummods; i++)
		if (r->mods[i].state == TLUA_RUNNING && (r->mods[i].requested & r->mods[i].granted & TLUA_PERM_EVENTS) && (*tlua_maskof(&r->mods[i]) & (1u << idx)))
			return true;
	return false;
}

typedef struct tlua_cmdcall_s
{
	const char *cmd;
	cmd_state_t *c;
} tlua_cmdcall_t;

static int tlua_cmd_tramp(lua_State *L)
{
	tlua_cmdcall_t *cc = (tlua_cmdcall_t *)lua_touserdata(L, 1);
	int i, argc = Cmd_Argc(cc->c);
	lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_cmds);
	lua_pushstring(L, cc->cmd);
	if (lua_rawget(L, -2) != LUA_TFUNCTION)
		return 0;
	for (i = 1; i < argc && i <= 16; i++)
		lua_pushstring(L, Cmd_Argv(cc->c, i));
	lua_call(L, i - 1, 0);
	return 0;
}

static void tlua_modcmd_f(cmd_state_t *cmd)
{
	const char *full = Cmd_Argv(cmd, 0);
	int i;
	for (i = 0; i < tlua_numcmds; i++)
	{
		tlua_cmd_t *c = &tlua_cmds[i];
		tlua_mod_t *m;
		tlua_cmdcall_t cc;
		if (strcmp(c->full, full))
			continue;
		m = TLua_Find(c->realm, c->mod);
		if (!m || m->state != TLUA_RUNNING || !(m->requested & m->granted & TLUA_PERM_COMMANDS))
		{
			Con_Printf("%s: Lua %s %s is not running\n", full, c->realm->kind, c->mod);
			return;
		}
		cc.cmd = c->cmd;
		cc.c = cmd;
		tlua_run(m, tlua_cmd_tramp, &cc, false);
		return;
	}
}

// ---------------------------------------------------------------------------
// The mod manager

static void tlua_dir(tlua_realm_t *r, char *out, size_t outsize)
{
	dpsnprintf(out, outsize, "%s%s/", fs_userdir, r->subdir);
}

static void tlua_readinfo(tlua_mod_t *m)
{
	char path[2048], line[512];
	FILE *f;
	m->title[0] = m->version[0] = m->author[0] = m->description[0] = 0;
	m->scoped = m->manifest_error = false;
	m->requested = 0;
	dpsnprintf(path, sizeof(path), "%smod.txt", m->dir);
	f = fopen(path, "r");
	if (!f)
	{
		if (errno == ENOENT) m->requested = TLUA_PERM_ALL;
		else m->manifest_error = true;
		return;
	}
	while (fgets(line, sizeof(line), f))
	{
		char *colon = strchr(line, ':'), *v, *e;
		if (!colon)
			continue;
		*colon = 0;
		for (v = colon + 1; *v == ' ' || *v == '\t'; v++)
			;
		for (e = v + strlen(v); e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); e--)
			*(e - 1) = 0;
		if (!strcmp(line, "title"))
			TLua_CleanText(v, m->title, sizeof(m->title), false);
		else if (!strcmp(line, "version"))
			TLua_CleanText(v, m->version, sizeof(m->version), false);
		else if (!strcmp(line, "author"))
			TLua_CleanText(v, m->author, sizeof(m->author), false);
		else if (!strcmp(line, "description"))
			TLua_CleanText(v, m->description, sizeof(m->description), false);
		else if (!strcmp(line, "api"))
		{
			if (m->scoped || strcmp(v, "1")) m->manifest_error = true;
			m->scoped = true;
		}
		else if (!strcmp(line, "permissions"))
		{
			char *token = strtok(v, ", \t");
			while (token)
			{
				int i;
				for (i = 0; tlua_permissions[i].name; i++)
					if (!strcmp(token, tlua_permissions[i].name)) break;
				if (!tlua_permissions[i].name) m->manifest_error = true;
				else m->requested |= tlua_permissions[i].bit;
				token = strtok(NULL, ", \t");
			}
		}
	}
	if (ferror(f)) m->manifest_error = true;
	fclose(f);
	if (!m->scoped && m->requested) m->manifest_error = true;
	if (!m->scoped) m->requested = TLUA_PERM_ALL;
	if (m->manifest_error) m->requested = 0;
}

static void tlua_readgrants(tlua_mod_t *m)
{
	char path[2048], line[128], extra, kind[16];
	unsigned mask;
	FILE *f;
	m->granted = m->scoped ? 0 : TLUA_PERM_ALL;
	dpsnprintf(path, sizeof(path), "%s%s/.permissions/%s.txt", fs_userdir, m->realm->subdir, m->name);
	f = fopen(path, "r");
	if (!f)
	{
		if (errno != ENOENT) m->granted = 0;
		return;
	}
	m->granted = 0; // malformed grant files fail closed
	if (fgets(line, sizeof(line), f) && sscanf(line, "%15s %x %c", kind, &mask, &extra) == 2 && !strcmp(kind, m->scoped ? "v1" : "legacy") && !(mask & ~TLUA_PERM_ALL))
		m->granted = mask;
	fclose(f);
}

static void tlua_readenabled(tlua_realm_t *r)
{
	char path[2048], line[128];
	FILE *f;
	int i;
	for (i = 0; i < r->nummods; i++)
		r->mods[i].enabled = false;
	tlua_dir(r, path, sizeof(path));
	dp_strlcat(path, "enabled.txt", sizeof(path));
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f))
	{
		tlua_mod_t *m;
		line[strcspn(line, "\r\n \t")] = 0;
		if ((m = TLua_Find(r, line)))
			m->enabled = true;
	}
	fclose(f);
}

static void tlua_writeenabled(tlua_realm_t *r)
{
	char path[2048];
	FILE *f;
	int i;
	tlua_dir(r, path, sizeof(path));
	FS_CreatePath(path);
	dp_strlcat(path, "enabled.txt", sizeof(path));
	f = fopen(path, "w");
	if (!f)
	{
		Con_Printf(CON_WARN "Lua: could not write %s\n", path);
		return;
	}
	for (i = 0; i < r->nummods; i++)
		if (r->mods[i].enabled)
			fprintf(f, "%s\n", r->mods[i].name);
	fclose(f);
}

static const char *tlua_statename(const tlua_mod_t *m)
{
	if (m->state == TLUA_RUNNING)
		return "running";
	if (m->state == TLUA_ERROR)
		return "error";
	return m->enabled ? "enabled" : "off";
}

static void tlua_updatestatus(tlua_realm_t *r)
{
	char buf[8192];
	int i;
	if (!r->status)
		return;
	buf[0] = 0;
	for (i = 0; i < r->nummods; i++)
	{
		tlua_mod_t *m = &r->mods[i];
		char line[1024];
		dpsnprintf(line, sizeof(line), "%s\t%s\t%s\t%s\t%s\n", m->name, tlua_statename(m), m->title[0] ? m->title : m->name, m->error, m->description);
		dp_strlcat(buf, line, sizeof(buf));
	}
	Cvar_SetQuick(r->status, buf);
	if (r == &tlua_client)
	{
		char perms[8192];
		perms[0] = 0;
		for (i = 0; i < r->nummods; i++)
		{
			tlua_mod_t *m = &r->mods[i];
			char requested[256] = "", granted[256] = "", line[640];
			int j;
			for (j = 0; tlua_permissions[j].name; j++)
			{
				unsigned bit = tlua_permissions[j].bit;
				if (m->requested & bit)
				{
					if (requested[0]) dp_strlcat(requested, ",", sizeof(requested));
					dp_strlcat(requested, tlua_permissions[j].name, sizeof(requested));
					if (m->granted & bit)
					{
						if (granted[0]) dp_strlcat(granted, ",", sizeof(granted));
						dp_strlcat(granted, tlua_permissions[j].name, sizeof(granted));
					}
				}
			}
			dpsnprintf(line, sizeof(line), "%s\t%s\t%s\t%s\n", m->name,
				m->manifest_error ? "invalid" : m->scoped ? "v1" : "legacy", requested, granted);
			dp_strlcat(perms, line, sizeof(perms));
		}
		Cvar_SetQuick(&cl_lua_permissions, perms);
	}
}

tlua_mod_t *TLua_Find(tlua_realm_t *r, const char *name)
{
	int i;
	for (i = 0; i < r->nummods; i++)
		if (!strcmp(r->mods[i].name, name))
			return &r->mods[i];
	return NULL;
}

static void tlua_stop(tlua_mod_t *m)
{
	if (m->L && m->state == TLUA_RUNNING)
		tlua_fire_one(m, "unload", NULL, NULL, NULL);
	m->killreason[0] = 0;
	tlua_close(m);
	m->state = TLUA_OFF;
}

static int tlua_cmpmod(const void *a, const void *b)
{
	return strcmp(((const tlua_mod_t *)a)->name, ((const tlua_mod_t *)b)->name);
}

void TLua_Rescan(tlua_realm_t *r)
{
	char dir[1024], path[2048];
	stringlist_t list;
	int i, j;
	qbool running;
	FILE *f;
	tlua_dir(r, dir, sizeof(dir));
	stringlistinit(&list);
	listdirectory(&list, dir, "");
	// mods keep pointers to their entry (lua_State extraspace), so entries
	// only move or go away while none of the realm's mods runs
	for (i = 0; i < r->nummods; i++)
		if (r->mods[i].L)
			break;
	running = i < r->nummods;
	// drop folders that went away
	for (i = 0; !running && i < r->nummods; i++)
	{
		tlua_mod_t *m = &r->mods[i];
		qbool found = false;
		for (j = 0; j < list.numstrings; j++)
			if (!strcmp(list.strings[j], m->name))
				found = true;
		dpsnprintf(path, sizeof(path), "%sinit.lua", m->dir);
		if (found && (f = fopen(path, "rb")))
		{
			fclose(f);
			continue;
		}
		memmove(&r->mods[i], &r->mods[i + 1], sizeof(r->mods[0]) * (size_t)(r->nummods - i - 1));
		r->nummods--;
		i--;
	}
	for (j = 0; j < list.numstrings; j++)
	{
		const char *name = list.strings[j];
		tlua_mod_t *m;
		if (strlen(name) > TLUA_NAME_LEN || !tlua_validname(name, false) || TLua_Find(r, name))
			continue;
		dpsnprintf(path, sizeof(path), "%s%s/init.lua", dir, name);
		if (!(f = fopen(path, "rb")))
			continue;
		fclose(f);
		if (r->nummods >= TLUA_MAX_MODS)
		{
			Con_Printf(CON_WARN "Lua: more than %d %ss, %s ignored\n", TLUA_MAX_MODS, r->kind, name);
			continue;
		}
		m = &r->mods[r->nummods++];
		memset(m, 0, sizeof(*m));
		dp_strlcpy(m->name, name, sizeof(m->name));
		dpsnprintf(m->dir, sizeof(m->dir), "%s%s/", dir, name);
		m->realm = r;
	}
	stringlistfreecontents(&list);
	// alphabetical order is the load order
	if (!running)
		qsort(r->mods, (size_t)r->nummods, sizeof(r->mods[0]), tlua_cmpmod);
	for (i = 0; i < r->nummods; i++)
	{
		tlua_readinfo(&r->mods[i]);
		tlua_readgrants(&r->mods[i]);
	}
	tlua_readenabled(r);
	tlua_updatestatus(r);
}

void TLua_LoadAll(tlua_realm_t *r)
{
	int i;
	r->loaded = true;
	TLua_Rescan(r);
	if (!r->enable->integer)
		return;
	for (i = 0; i < r->nummods; i++)
		if (r->mods[i].enabled && !r->mods[i].L)
			tlua_start(&r->mods[i]);
	tlua_updatestatus(r);
}

void TLua_UnloadAll(tlua_realm_t *r, const char *why)
{
	int i, n = 0;
	for (i = 0; i < r->nummods; i++)
	{
		if (r->mods[i].L)
			n++;
		tlua_stop(&r->mods[i]);
	}
	if (n && why)
		Con_DPrintf("Lua: %d %s(s) stopped (%s)\n", n, r->kind, why);
	r->loaded = false;
	tlua_updatestatus(r);
}

static tlua_realm_t *tlua_realmofcmd(cmd_state_t *cmd)
{
	return !strncmp(Cmd_Argv(cmd, 0), "sv_", 3) ? &tlua_server : &tlua_client;
}

// a server can stuff any command into a client's console, so a client mod is
// only switched on or reloaded while not connected to someone else's server
static qbool tlua_remote_refused(tlua_realm_t *r, cmd_state_t *cmd)
{
	if (r == &tlua_client && cls.state == ca_connected && !sv.active)
	{
		Con_Printf("%s: not while connected to a server; disconnect first\n", Cmd_Argv(cmd, 0));
		return true;
	}
	return false;
}

static void tlua_list_f(cmd_state_t *cmd)
{
	tlua_realm_t *r = tlua_realmofcmd(cmd);
	char dir[1024];
	int i;
	TLua_Rescan(r);
	tlua_dir(r, dir, sizeof(dir));
	Con_Printf("Lua %ss in %s (master switch %s %s):\n", r->kind, dir, r->enable->name, r->enable->integer ? "on" : "off");
	for (i = 0; i < r->nummods; i++)
	{
		tlua_mod_t *m = &r->mods[i];
		char kb[32] = "";
		if (m->state == TLUA_RUNNING)
			dpsnprintf(kb, sizeof(kb), " (%i KB)", (int)(m->mem >> 10));
		Con_Printf("  %-20s %-8s %s%s\n", m->name, tlua_statename(m), m->title[0] ? m->title : "", kb);
		if (m->state == TLUA_ERROR)
			Con_Printf("      ^1%s^7\n", m->error);
	}
	if (!r->nummods)
		Con_Printf("  none: put a folder with an init.lua there (docs/MODDING.md)\n");
}

static tlua_mod_t *tlua_argmod(cmd_state_t *cmd, tlua_realm_t *r)
{
	tlua_mod_t *m;
	if (Cmd_Argc(cmd) < 2)
	{
		Con_Printf("usage: %s <name>\n", Cmd_Argv(cmd, 0));
		return NULL;
	}
	TLua_Rescan(r);
	m = TLua_Find(r, Cmd_Argv(cmd, 1));
	if (!m)
		Con_Printf("%s: no %s named %s\n", Cmd_Argv(cmd, 0), r->kind, Cmd_Argv(cmd, 1));
	return m;
}

static void tlua_enable_f(cmd_state_t *cmd)
{
	tlua_realm_t *r = tlua_realmofcmd(cmd);
	tlua_mod_t *m;
	if (tlua_remote_refused(r, cmd) || !(m = tlua_argmod(cmd, r)))
		return;
	m->enabled = true;
	tlua_writeenabled(r);
	if (r->loaded && r->enable->integer && !m->L)
		tlua_start(m);
	tlua_updatestatus(r);
	Con_Printf("Lua %s %s: %s\n", r->kind, m->name, tlua_statename(m));
}

static void tlua_disable_f(cmd_state_t *cmd)
{
	tlua_realm_t *r = tlua_realmofcmd(cmd);
	tlua_mod_t *m;
	if (!(m = tlua_argmod(cmd, r)))
		return;
	m->enabled = false;
	tlua_writeenabled(r);
	tlua_stop(m);
	m->error[0] = 0;
	tlua_updatestatus(r);
	Con_Printf("Lua %s %s: off\n", r->kind, m->name);
}

static void tlua_reload_f(cmd_state_t *cmd)
{
	tlua_realm_t *r = tlua_realmofcmd(cmd);
	int i;
	if (tlua_remote_refused(r, cmd))
		return;
	if (Cmd_Argc(cmd) >= 2)
	{
		tlua_mod_t *m = tlua_argmod(cmd, r);
		if (!m)
			return;
		tlua_stop(m);
		m->error[0] = 0;
		if (m->enabled && r->loaded && r->enable->integer)
			tlua_start(m);
		tlua_updatestatus(r);
		return;
	}
	for (i = 0; i < r->nummods; i++)
	{
		tlua_stop(&r->mods[i]);
		r->mods[i].error[0] = 0;
	}
	if (r->loaded)
		TLua_LoadAll(r);
	else
		TLua_Rescan(r);
}

static void tlua_info_f(cmd_state_t *cmd)
{
	tlua_realm_t *r = tlua_realmofcmd(cmd);
	tlua_mod_t *m = tlua_argmod(cmd, r);
	int i;
	if (!m)
		return;
	Con_Printf("%s %s: %s\n  folder: %s\n", r->kind, m->name, tlua_statename(m), m->dir);
	if (m->title[0])
		Con_Printf("  title: %s\n", m->title);
	if (m->version[0])
		Con_Printf("  version: %s\n", m->version);
	if (m->author[0])
		Con_Printf("  author: %s\n", m->author);
	if (m->description[0])
		Con_Printf("  %s\n", m->description);
	if (m->L)
		Con_Printf("  memory: %i KB of %i MB\n", (int)(m->mem >> 10), (int)(m->memlimit >> 20));
	if (m->error[0])
		Con_Printf("  ^1error: %s^7\n", m->error);
	for (i = 0; m->realm->events[i]; i++)
		if (*tlua_maskof(m) & (1u << i))
			Con_Printf("  listens to: %s\n", m->realm->events[i]);
	for (i = 0; i < tlua_numcmds; i++)
		if (tlua_cmds[i].realm == r && !strcmp(tlua_cmds[i].mod, m->name))
			Con_Printf("  command: %s\n", tlua_cmds[i].full);
}

// Grants are a local user choice, never supplied by a Lua mod. Store each
// realm separately, and publish only a fully written file.
static void tlua_permission_f(cmd_state_t *cmd)
{
	tlua_realm_t *r = tlua_realmofcmd(cmd);
	tlua_mod_t *m;
	const char *action, *name;
	char path[2048], tmp[2056];
	unsigned mask;
	int i, failed;
	FILE *f;
	if (tlua_remote_refused(r, cmd) || !(m = tlua_argmod(cmd, r))) return;
	if (Cmd_Argc(cmd) == 2)
	{
		Con_Printf("Lua %s %s API: %s%s\n", r->kind, m->name, m->scoped ? "1" : "legacy", m->manifest_error ? " (invalid manifest)" : "");
		for (i = 0; tlua_permissions[i].name; i++)
			Con_Printf("  %s: %s%s\n", tlua_permissions[i].name, (m->granted & tlua_permissions[i].bit) ? "granted" : "denied", (m->requested & tlua_permissions[i].bit) ? " (requested)" : " (not requested)");
		return;
	}
	if (Cmd_Argc(cmd) != 4)
	{
		Con_Printf("usage: %s <name> [grant|revoke <permission>]\n", Cmd_Argv(cmd, 0));
		return;
	}
	action = Cmd_Argv(cmd, 2); name = Cmd_Argv(cmd, 3);
	for (i = 0; tlua_permissions[i].name; i++) if (!strcmp(name, tlua_permissions[i].name)) break;
	if (!tlua_permissions[i].name || (strcmp(action, "grant") && strcmp(action, "revoke")))
	{
		Con_Printf("unknown action or permission\n"); return;
	}
	if (!strcmp(action, "grant") && (!(m->requested & tlua_permissions[i].bit) || m->manifest_error))
	{
		Con_Printf("permission %s is not requested or manifest is invalid\n", name); return;
	}
	mask = !strcmp(action, "grant") ? m->granted | tlua_permissions[i].bit : m->granted & ~tlua_permissions[i].bit;
	dpsnprintf(path, sizeof(path), "%s%s/.permissions/%s.txt", fs_userdir, m->realm->subdir, m->name);
	dpsnprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FS_CreatePath(path);
	f = fopen(tmp, "w");
	if (!f) { Con_Printf("could not save permissions\n"); return; }
	failed = fprintf(f, "%s %x\n", m->scoped ? "v1" : "legacy", mask) < 0;
	if (fclose(f)) failed = 1;
	if (failed || rename(tmp, path)) { remove(tmp); Con_Printf("could not save permissions\n"); return; }
	m->granted = mask;
	tlua_updatestatus(r);
	Con_Printf("Lua %s %s: %s %s\n", r->kind, m->name, action, name);
}

// ---------------------------------------------------------------------------
// Engine entry points

void TLua_InitOnce(void)
{
	static qbool done;
	if (done)
		return;
	done = true;
	Cvar_RegisterVariable(&lua_memory_mb);
	Cvar_RegisterVariable(&lua_instructions);
	Cvar_RegisterVariable(&lua_time_ms);
	Cvar_RegisterVariable(&cl_lua_enable);
	Cvar_RegisterVariable(&sv_lua_enable);
	Cvar_RegisterVariable(&cl_lua_status);
	Cvar_RegisterVariable(&cl_lua_permissions);
	tlua_client.enable = &cl_lua_enable;
	tlua_client.status = &cl_lua_status;
	tlua_server.enable = &sv_lua_enable;
	tlua_server.status = NULL;
	TLua_CL_Setup();
	TLua_SV_Setup();

	Cmd_AddCommand(CF_CLIENT, "lua_mods", tlua_list_f, "list the Lua client mods in <userdir>/mods and their state");
	Cmd_AddCommand(CF_CLIENT, "lua_mod_enable", tlua_enable_f, "turn a Lua client mod on (kept in mods/enabled.txt)");
	Cmd_AddCommand(CF_CLIENT, "lua_mod_disable", tlua_disable_f, "turn a Lua client mod off");
	Cmd_AddCommand(CF_CLIENT, "lua_mod_reload", tlua_reload_f, "restart one Lua client mod, or all of them (clears errors)");
	Cmd_AddCommand(CF_CLIENT, "lua_mod_permission", tlua_permission_f, "inspect or change a Lua client mod's permissions");
	Cmd_AddCommand(CF_CLIENT, "lua_mod_info", tlua_info_f, "show a Lua client mod's details, error, events and commands");
	Cmd_AddCommand(CF_SERVER, "sv_lua_plugins", tlua_list_f, "list the Lua server plugins in <userdir>/plugins and their state");
	Cmd_AddCommand(CF_SERVER, "sv_lua_plugin_enable", tlua_enable_f, "turn a Lua server plugin on (kept in plugins/enabled.txt)");
	Cmd_AddCommand(CF_SERVER, "sv_lua_plugin_disable", tlua_disable_f, "turn a Lua server plugin off");
	Cmd_AddCommand(CF_SERVER, "sv_lua_plugin_reload", tlua_reload_f, "restart one Lua server plugin, or all of them (clears errors)");
	Cmd_AddCommand(CF_SERVER, "sv_lua_plugin_permission", tlua_permission_f, "inspect or change a Lua server plugin's permissions");
	Cmd_AddCommand(CF_SERVER, "sv_lua_plugin_info", tlua_info_f, "show a Lua server plugin's details, error, events and commands");
}
