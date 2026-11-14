/*
Godnotic: API level 3 of the Lua mods (docs/LUA_API.md, generated from
scripts/lua_api_table.py).

Every function of the table is registered here as a closure over its table
entry: the closure checks the entry's tier on every call (Standard costs
nothing, the other tiers are a bit test), then calls the C function. So the
reference and the code cannot disagree about who may call what, and
revoking a tier takes effect on the next call.

This file also holds what both realms share:
  xt.json, xt.time, xt.game, xt.locale, xt.top, xt.engine (Full engine access),
  xt.http (Internet), xt.hook / xt.hooknames (dispatch to the realm's hooks)
and the accounting behind lua_mod_top and the per-mod frame budget
(lua_frame_ms): a mod past it misses its calls for the rest of that frame, so
one slow mod cannot starve the others or the game.
*/

#include "quakedef.h"
#include "libcurl.h"
#include "touch_webfetch.h"
#include "touch_lua.h"
#include "touch_lua_int.h"
#define TLUA_API_DEFINE_TABLE
#include "touch_lua_api_gen.h"
// the offline-only gate itself (TouchWep_PracticeGate); its other helpers are unused here
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "touch_weapons.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <time.h>

static cvar_t lua_frame_ms = {CF_CLIENT | CF_SERVER | CF_ARCHIVE, "lua_frame_ms", "50", "time one Lua mod may spend per frame in milliseconds (1-1000); past it its calls are skipped for the rest of that frame (lua_mod_top counts them)"};
static cvar_t lua_top = {CF_CLIENT | CF_READONLY, "_lua_top", "", "per-mod cost for UIs, JSON: [{\"realm\",\"name\",\"state\",\"cpu_ms\",\"calls\",\"skipped\",\"mem_kb\",\"peak_kb\"}] over the last second"};

void TLua_Guard(cvar_t *var);
void TLua_RegisterCvar(cvar_t *var);

// ---------------------------------------------------------------------------
// Registration

static int tlua_gate(lua_State *L)
{
	const tlua_apidef_t *d = (const tlua_apidef_t *)lua_touserdata(L, lua_upvalueindex(1));
	if (d->tier != TLUA_TIER_STANDARD)
		TLua_Require(L, d->tier);
	return d->fn(L);
}

// a module table that took the place of an API 1/2 function of the same name
// (xt.time): calling the table calls the old function
static int tlua_callold(lua_State *L)
{
	lua_remove(L, 1); // the table itself
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_insert(L, 1);
	lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
	return lua_gettop(L);
}

void TLua_API_Open(lua_State *L, tlua_mod_t *m)
{
	const tlua_apidef_t *d;
	int xt = lua_gettop(L);
	int realm = m->realm == &tlua_server ? 2 : 1;
	for (d = tlua_apidefs; d->name; d++)
	{
		if (!(d->realm & realm))
			continue;
		if (d->module)
		{
			int t = lua_getfield(L, xt, d->module);
			if (t != LUA_TTABLE)
			{
				lua_newtable(L);
				if (t == LUA_TFUNCTION)
				{
					// keep xt.time() working: the module is callable
					lua_createtable(L, 0, 1);
					lua_pushvalue(L, -3);
					lua_pushcclosure(L, tlua_callold, 1);
					lua_setfield(L, -2, "__call");
					lua_setmetatable(L, -2);
				}
				lua_remove(L, -2);
				lua_pushvalue(L, -1);
				lua_setfield(L, xt, d->module);
			}
		}
		else
			lua_pushvalue(L, xt);
		lua_pushlightuserdata(L, (void *)d);
		lua_pushcclosure(L, tlua_gate, 1);
		lua_setfield(L, -2, d->name);
		lua_pop(L, 1);
	}
	lua_settop(L, xt);
}

// ---------------------------------------------------------------------------
// Accounting, the frame budget, lua_mod_top

static void tlua_roll(tlua_mod_t *m)
{
	if (host.realtime - m->window_start < 1 && host.realtime >= m->window_start)
		return;
	m->cpu_last = m->cpu_window;
	m->calls_last = m->calls_window;
	m->throttled_last = m->throttled_window;
	m->cpu_window = 0;
	m->calls_window = m->throttled_window = 0;
	m->window_start = host.realtime;
}

qbool TLua_API_Throttled(tlua_mod_t *m)
{
	if (m->spentframe != host.framecount)
		return false;
	return m->framespent * 1000.0 > bound(1, lua_frame_ms.value, 1000);
}

void TLua_API_Account(tlua_mod_t *m, double seconds, qbool skipped)
{
	tlua_roll(m);
	if (m->spentframe != host.framecount)
	{
		m->spentframe = host.framecount;
		m->framespent = 0;
	}
	if (skipped)
	{
		m->throttled_window++;
		return;
	}
	m->framespent += seconds;
	m->cpu_window += seconds;
	m->calls_window++;
}

static const char *tlua_state(const tlua_mod_t *m)
{
	return m->state == TLUA_RUNNING ? "running" : m->state == TLUA_ERROR ? "error" : m->enabled ? "enabled" : "off";
}

static void tlua_json_str(char *out, size_t size, const char *s)
{
	size_t o = strlen(out);
	if (o + 2 >= size)
		return;
	out[o++] = '"';
	for (; *s && o + 8 < size; s++)
	{
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\')
		{
			out[o++] = '\\';
			out[o++] = (char)c;
		}
		else if (c < 32)
			o += (size_t)dpsnprintf(out + o, size - o, "\\u%04x", c);
		else
			out[o++] = (char)c;
	}
	out[o++] = '"';
	out[o] = 0;
}

void TLua_JsonAppendString(char *out, size_t size, const char *s);
void TLua_JsonAppendString(char *out, size_t size, const char *s)
{
	tlua_json_str(out, size, s);
}

static void tlua_top_json(char *buf, size_t size)
{
	tlua_realm_t *realms[2] = {&tlua_client, &tlua_server};
	int r, i;
	qbool first = true;
	dp_strlcpy(buf, "[", size);
	for (r = 0; r < 2; r++)
		for (i = 0; i < realms[r]->nummods; i++)
		{
			tlua_mod_t *m = &realms[r]->mods[i];
			char line[256];
			if (m->state == TLUA_OFF && !m->enabled)
				continue;
			tlua_roll(m);
			if (!first)
				dp_strlcat(buf, ",", size);
			first = false;
			dp_strlcat(buf, "{\"realm\":", size);
			tlua_json_str(buf, size, realms[r] == &tlua_server ? "server" : "client");
			dp_strlcat(buf, ",\"name\":", size);
			tlua_json_str(buf, size, m->name);
			dp_strlcat(buf, ",\"state\":", size);
			tlua_json_str(buf, size, tlua_state(m));
			dpsnprintf(line, sizeof(line), ",\"cpu_ms\":%.3f,\"calls\":%u,\"skipped\":%u,\"mem_kb\":%d,\"peak_kb\":%d}",
				m->cpu_last * 1000.0, m->calls_last, m->throttled_last, (int)(m->mem >> 10), (int)(m->mempeak >> 10));
			dp_strlcat(buf, line, size);
		}
	dp_strlcat(buf, "]", size);
}

static void tlua_top_f(cmd_state_t *cmd)
{
	tlua_realm_t *realms[2] = {&tlua_client, &tlua_server};
	int r, i, n = 0;
	if (Cmd_Argc(cmd) >= 2 && !strcmp(Cmd_Argv(cmd, 1), "json"))
	{
		static char buf[16384];
		tlua_top_json(buf, sizeof(buf));
		Con_Printf("%s\n", buf);
		return;
	}
	Con_Printf("Lua mods, the last second (lua_frame_ms %g per mod per frame):\n", lua_frame_ms.value);
	Con_Printf("  %-6s %-20s %-8s %9s %6s %7s %8s %8s\n", "realm", "name", "state", "cpu ms/s", "calls", "skipped", "mem KB", "peak KB");
	for (r = 0; r < 2; r++)
		for (i = 0; i < realms[r]->nummods; i++)
		{
			tlua_mod_t *m = &realms[r]->mods[i];
			if (m->state == TLUA_OFF && !m->enabled)
				continue;
			tlua_roll(m);
			n++;
			Con_Printf("  %-6s %-20s %-8s %9.3f %6u %7u %8d %8d\n", realms[r] == &tlua_server ? "server" : "client", m->name, tlua_state(m),
				m->cpu_last * 1000.0, m->calls_last, m->throttled_last, (int)(m->mem >> 10), (int)(m->mempeak >> 10));
		}
	if (!n)
		Con_Printf("  no mod is on\n");
}

static int tlua_xtop(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	tlua_roll(m);
	lua_createtable(L, 0, 4);
	lua_pushnumber(L, m->cpu_last * 1000.0);
	lua_setfield(L, -2, "cpu_ms");
	lua_pushinteger(L, m->calls_last);
	lua_setfield(L, -2, "calls");
	lua_pushinteger(L, (lua_Integer)(m->mem >> 10));
	lua_setfield(L, -2, "mem_kb");
	lua_pushinteger(L, m->throttled_last);
	lua_setfield(L, -2, "throttled");
	return 1;
}
int TLuaAPI_top(lua_State *L) { return tlua_xtop(L); }

// ---------------------------------------------------------------------------
// xt.json

#define TLUA_JSON_DEPTH 32
#define TLUA_JSON_MAX (256 * 1024)

// The encoder writes into one static buffer (no Lua callbacks run while it
// works, so it is never re-entered; an error longjmps out and leaves nothing
// to free) and pushes the result once.
static char tlua_jbuf[TLUA_JSON_MAX + 64];
static size_t tlua_jlen;

static void tlua_jput(lua_State *L, const char *s, size_t n)
{
	if (tlua_jlen + n > TLUA_JSON_MAX)
		luaL_error(L, "xt.json.encode: over %d KB", TLUA_JSON_MAX / 1024);
	memcpy(tlua_jbuf + tlua_jlen, s, n);
	tlua_jlen += n;
}
#define tlua_jputs(L, s) tlua_jput(L, s, strlen(s))

static void tlua_json_putstr(lua_State *L, const char *s, size_t len)
{
	size_t i;
	tlua_jput(L, "\"", 1);
	for (i = 0; i < len; i++)
	{
		unsigned char c = (unsigned char)s[i];
		if (c == '"' || c == '\\')
		{
			char e[2] = {'\\', (char)c};
			tlua_jput(L, e, 2);
		}
		else if (c == '\n')
			tlua_jputs(L, "\\n");
		else if (c == '\r')
			tlua_jputs(L, "\\r");
		else if (c == '\t')
			tlua_jputs(L, "\\t");
		else if (c < 32 || c == 127)
		{
			char u[8];
			dpsnprintf(u, sizeof(u), "\\u%04x", c);
			tlua_jputs(L, u);
		}
		else
			tlua_jput(L, (const char *)&s[i], 1);
	}
	tlua_jput(L, "\"", 1);
}

static void tlua_json_enc(lua_State *L, int idx, int depth)
{
	idx = lua_absindex(L, idx);
	TouchLua_WorkStep(L);
	switch (lua_type(L, idx))
	{
	case LUA_TNIL:
		tlua_jputs(L, "null");
		break;
	case LUA_TBOOLEAN:
		tlua_jputs(L, lua_toboolean(L, idx) ? "true" : "false");
		break;
	case LUA_TNUMBER:
	{
		char num[64];
		if (lua_isinteger(L, idx))
			dpsnprintf(num, sizeof(num), "%lld", (long long)lua_tointeger(L, idx));
		else
		{
			double d = lua_tonumber(L, idx);
			if (d != d || d > 1e308 || d < -1e308)
				dp_strlcpy(num, "null", sizeof(num));
			else
				dpsnprintf(num, sizeof(num), "%.14g", d);
		}
		tlua_jputs(L, num);
		break;
	}
	case LUA_TSTRING:
	{
		size_t len;
		const char *s = lua_tolstring(L, idx, &len);
		tlua_json_putstr(L, s, len);
		break;
	}
	case LUA_TTABLE:
	{
		lua_Integer n = (lua_Integer)lua_rawlen(L, idx), i;
		qbool first = true;
		if (depth >= TLUA_JSON_DEPTH)
			luaL_error(L, "xt.json.encode: nested deeper than %d (or a table inside itself)", TLUA_JSON_DEPTH);
		luaL_checkstack(L, 4, "json");
		if (n > 0)
		{
			tlua_jput(L, "[", 1);
			for (i = 1; i <= n; i++)
			{
				if (i > 1)
					tlua_jput(L, ",", 1);
				lua_rawgeti(L, idx, i);
				tlua_json_enc(L, -1, depth + 1);
				lua_pop(L, 1);
			}
			tlua_jput(L, "]", 1);
			break;
		}
		tlua_jput(L, "{", 1);
		lua_pushnil(L);
		while (lua_next(L, idx))
		{
			if (!first)
				tlua_jput(L, ",", 1);
			first = false;
			if (lua_type(L, -2) == LUA_TSTRING)
			{
				size_t klen;
				const char *k = lua_tolstring(L, -2, &klen);
				tlua_json_putstr(L, k, klen);
			}
			else if (lua_type(L, -2) == LUA_TNUMBER)
			{
				char num[64];
				if (lua_isinteger(L, -2))
					dpsnprintf(num, sizeof(num), "%lld", (long long)lua_tointeger(L, -2));
				else
					dpsnprintf(num, sizeof(num), "%.14g", lua_tonumber(L, -2));
				tlua_json_putstr(L, num, strlen(num));
			}
			else
				luaL_error(L, "xt.json.encode: table keys must be strings or numbers");
			tlua_jput(L, ":", 1);
			tlua_json_enc(L, -1, depth + 1);
			lua_pop(L, 1);
		}
		tlua_jput(L, "}", 1);
		break;
	}
	default:
		luaL_error(L, "xt.json.encode: cannot encode a %s", luaL_typename(L, idx));
	}
}

int TLuaAPI_json_encode(lua_State *L)
{
	luaL_checkany(L, 1);
	lua_settop(L, 1);
	tlua_jlen = 0;
	tlua_json_enc(L, 1, 0);
	lua_pushlstring(L, tlua_jbuf, tlua_jlen);
	return 1;
}

// C callers (pages, the channel): the value at idx as JSON into out; false
// when it does not fit or cannot be encoded (raises like the Lua function)
size_t TLua_JsonEncode(lua_State *L, int idx, char *out, size_t outsize);
size_t TLua_JsonEncode(lua_State *L, int idx, char *out, size_t outsize)
{
	idx = lua_absindex(L, idx);
	tlua_jlen = 0;
	tlua_json_enc(L, idx, 0);
	if (tlua_jlen + 1 > outsize)
		return 0;
	memcpy(out, tlua_jbuf, tlua_jlen);
	out[tlua_jlen] = 0;
	return tlua_jlen;
}

typedef struct tlua_jp_s
{
	lua_State *L;
	const char *s, *e;
	const char *err;
} tlua_jp_t;

static void tlua_jp_ws(tlua_jp_t *p)
{
	while (p->s < p->e && (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r'))
		p->s++;
}

static qbool tlua_jp_value(tlua_jp_t *p, int depth);

static qbool tlua_jp_string(tlua_jp_t *p)
{
	luaL_Buffer b;
	lua_State *L = p->L;
	p->s++; // the quote
	luaL_buffinit(L, &b);
	while (p->s < p->e && *p->s != '"')
	{
		unsigned char c = (unsigned char)*p->s++;
		if (c == '\\')
		{
			if (p->s >= p->e)
				break;
			c = (unsigned char)*p->s++;
			switch (c)
			{
			case 'n': luaL_addchar(&b, '\n'); break;
			case 't': luaL_addchar(&b, '\t'); break;
			case 'r': luaL_addchar(&b, '\r'); break;
			case 'b': luaL_addchar(&b, '\b'); break;
			case 'f': luaL_addchar(&b, '\f'); break;
			case 'u':
			{
				unsigned u = 0;
				int k;
				char utf[8];
				if (p->e - p->s < 4)
				{
					p->err = "bad \\u escape";
					luaL_pushresult(&b);
					return false;
				}
				for (k = 0; k < 4; k++)
				{
					char h = *p->s++;
					u <<= 4;
					if (h >= '0' && h <= '9') u |= (unsigned)(h - '0');
					else if (h >= 'a' && h <= 'f') u |= (unsigned)(h - 'a' + 10);
					else if (h >= 'A' && h <= 'F') u |= (unsigned)(h - 'A' + 10);
					else { p->err = "bad \\u escape"; luaL_pushresult(&b); return false; }
				}
				// UTF-8 of a BMP code point (surrogates come out as they are)
				if (u < 0x80) { utf[0] = (char)u; utf[1] = 0; }
				else if (u < 0x800) { utf[0] = (char)(0xC0 | (u >> 6)); utf[1] = (char)(0x80 | (u & 0x3F)); utf[2] = 0; }
				else { utf[0] = (char)(0xE0 | (u >> 12)); utf[1] = (char)(0x80 | ((u >> 6) & 0x3F)); utf[2] = (char)(0x80 | (u & 0x3F)); utf[3] = 0; }
				luaL_addstring(&b, utf);
				break;
			}
			default: luaL_addchar(&b, (char)c); break;
			}
		}
		else
			luaL_addchar(&b, (char)c);
	}
	luaL_pushresult(&b);
	if (p->s >= p->e)
	{
		p->err = "unterminated string";
		return false;
	}
	p->s++;
	return true;
}

static qbool tlua_jp_value(tlua_jp_t *p, int depth)
{
	lua_State *L = p->L;
	TouchLua_WorkStep(L);
	tlua_jp_ws(p);
	if (p->s >= p->e)
	{
		p->err = "unexpected end";
		return false;
	}
	if (depth > TLUA_JSON_DEPTH)
	{
		p->err = "nested too deep";
		return false;
	}
	luaL_checkstack(L, 4, "json");
	switch (*p->s)
	{
	case '"':
		return tlua_jp_string(p);
	case '{':
		p->s++;
		lua_newtable(L);
		tlua_jp_ws(p);
		if (p->s < p->e && *p->s == '}')
		{
			p->s++;
			return true;
		}
		for (;;)
		{
			tlua_jp_ws(p);
			if (p->s >= p->e || *p->s != '"')
			{
				p->err = "expected a key";
				return false;
			}
			if (!tlua_jp_string(p))
				return false;
			tlua_jp_ws(p);
			if (p->s >= p->e || *p->s != ':')
			{
				p->err = "expected ':'";
				return false;
			}
			p->s++;
			if (!tlua_jp_value(p, depth + 1))
				return false;
			lua_rawset(L, -3);
			tlua_jp_ws(p);
			if (p->s < p->e && *p->s == ',')
			{
				p->s++;
				continue;
			}
			if (p->s < p->e && *p->s == '}')
			{
				p->s++;
				return true;
			}
			p->err = "expected ',' or '}'";
			return false;
		}
	case '[':
	{
		lua_Integer i = 0;
		p->s++;
		lua_newtable(L);
		tlua_jp_ws(p);
		if (p->s < p->e && *p->s == ']')
		{
			p->s++;
			return true;
		}
		for (;;)
		{
			if (!tlua_jp_value(p, depth + 1))
				return false;
			// null in an array keeps its place as false-y nil would not
			if (lua_isnil(L, -1))
			{
				lua_pop(L, 1);
				lua_pushboolean(L, 0);
			}
			lua_rawseti(L, -2, ++i);
			tlua_jp_ws(p);
			if (p->s < p->e && *p->s == ',')
			{
				p->s++;
				continue;
			}
			if (p->s < p->e && *p->s == ']')
			{
				p->s++;
				return true;
			}
			p->err = "expected ',' or ']'";
			return false;
		}
	}
	case 't':
		if (p->e - p->s >= 4 && !strncmp(p->s, "true", 4)) { p->s += 4; lua_pushboolean(L, 1); return true; }
		break;
	case 'f':
		if (p->e - p->s >= 5 && !strncmp(p->s, "false", 5)) { p->s += 5; lua_pushboolean(L, 0); return true; }
		break;
	case 'n':
		if (p->e - p->s >= 4 && !strncmp(p->s, "null", 4)) { p->s += 4; lua_pushnil(L); return true; }
		break;
	default:
		if (*p->s == '-' || (*p->s >= '0' && *p->s <= '9'))
		{
			char num[64];
			size_t n = 0;
			char *end;
			double d;
			while (p->s < p->e && n + 1 < sizeof(num) && strchr("+-0123456789.eE", *p->s))
				num[n++] = *p->s++;
			num[n] = 0;
			d = strtod(num, &end);
			if (*end)
				break;
			if (!strpbrk(num, ".eE") && d >= -9007199254740992.0 && d <= 9007199254740992.0)
				lua_pushinteger(L, (lua_Integer)d);
			else
				lua_pushnumber(L, d);
			return true;
		}
	}
	p->err = "unexpected character";
	return false;
}

int TLuaAPI_json_decode(lua_State *L)
{
	size_t len;
	const char *s = luaL_checklstring(L, 1, &len);
	tlua_jp_t p;
	int base = lua_gettop(L);
	if (len > TLUA_JSON_MAX)
	{
		lua_pushnil(L);
		lua_pushfstring(L, "over %d KB", TLUA_JSON_MAX / 1024);
		return 2;
	}
	p.L = L;
	p.s = s;
	p.e = s + len;
	p.err = NULL;
	if (tlua_jp_value(&p, 0))
	{
		tlua_jp_ws(&p);
		if (p.s == p.e)
			return 1;
		p.err = "text after the value";
	}
	lua_settop(L, base);
	lua_pushnil(L);
	lua_pushfstring(L, "%s at byte %d", p.err ? p.err : "bad JSON", (int)(p.s - s) + 1);
	return 2;
}

// C callers (pages, the channel) decode with the same parser
qbool TLua_JsonDecode(lua_State *L, const char *s, size_t len);
qbool TLua_JsonDecode(lua_State *L, const char *s, size_t len)
{
	tlua_jp_t p;
	int base = lua_gettop(L);
	p.L = L;
	p.s = s;
	p.e = s + len;
	p.err = NULL;
	if (len <= TLUA_JSON_MAX && tlua_jp_value(&p, 0))
	{
		tlua_jp_ws(&p);
		if (p.s == p.e)
			return true;
	}
	lua_settop(L, base);
	return false;
}

// ---------------------------------------------------------------------------
// xt.time

int TLuaAPI_time_now(lua_State *L) { lua_pushnumber(L, host.realtime); return 1; }
int TLuaAPI_time_clock(lua_State *L) { lua_pushnumber(L, Sys_DirtyTime()); return 1; }
int TLuaAPI_time_unix(lua_State *L) { lua_pushinteger(L, (lua_Integer)time(NULL)); return 1; }

int TLuaAPI_time_date(lua_State *L)
{
	const char *fmt = luaL_optstring(L, 1, "%Y-%m-%d %H:%M:%S");
	time_t t = (time_t)luaL_optinteger(L, 2, (lua_Integer)time(NULL));
	const char *p;
	struct tm *tm;
	char out[256];
	if (strlen(fmt) > 64)
		return luaL_error(L, "xt.time.date: format over 64 characters");
	for (p = fmt; *p; p++)
		if (*p == '%')
		{
			p++;
			if (!*p || !strchr("YmdHMSabj%", *p))
				return luaL_error(L, "xt.time.date: only %%Y %%m %%d %%H %%M %%S %%a %%b %%j %%%%");
		}
	tm = localtime(&t);
	if (!tm || !strftime(out, sizeof(out), fmt, tm))
		out[0] = 0;
	lua_pushstring(L, out);
	return 1;
}

// ---------------------------------------------------------------------------
// xt.game: what this game is and can do, for feature tests

static const char *tlua_platform(void)
{
#if defined(__ANDROID__)
	return "android";
#elif defined(__EMSCRIPTEN__)
	return "web";
#elif defined(_WIN32)
	return "windows";
#elif defined(__APPLE__)
	return "mac";
#else
	return "linux";
#endif
}

qbool (*TouchLua_VibrateHook)(int ms); // set by the platform when it can vibrate (touch_lua.h)

static qbool tlua_has_http(void)
{
#if defined(__EMSCRIPTEN__)
	return TouchWeb_Available(); // the browser's fetch(): the host must allow web pages (CORS)
#else
	return Curl_Available();
#endif
}

static qbool tlua_cap(tlua_mod_t *m, const char *c)
{
	if (!strcmp(c, "http")) return tlua_has_http();
	if (!strcmp(c, "vibrate")) return m->realm == &tlua_client && TouchLua_VibrateHook != NULL;
#if defined(__EMSCRIPTEN__)
	if (!strcmp(c, "clipboard")) return false;
#else
	if (!strcmp(c, "clipboard")) return m->realm == &tlua_client;
#endif
	if (!strcmp(c, "channel") || !strcmp(c, "pages") || !strcmp(c, "hooks")) return true;
	if (!strcmp(c, "csqc_hooks")) return m->realm == &tlua_client && TLua_CL_CsqcKind() == 2;
	if (!strcmp(c, "engine_full")) return TLua_Has(m, TLUA_TIER_ENGINE);
	if (!strcmp(c, "offline")) return TLua_Offline(NULL);
	return false;
}

int TLuaAPI_game_has(lua_State *L)
{
	lua_pushboolean(L, tlua_cap(TLua_Self(L), luaL_checkstring(L, 1)));
	return 1;
}

int TLuaAPI_game_info(lua_State *L)
{
	static const char *const caps[] = {"http", "vibrate", "clipboard", "channel", "pages", "hooks", "csqc_hooks", "engine_full", "offline", NULL};
	tlua_mod_t *m = TLua_Self(L);
	int i, k = TLua_CL_CsqcKind();
	lua_createtable(L, 0, 10);
	lua_pushstring(L, gamename);
	lua_setfield(L, -2, "game");
	lua_pushstring(L, engineversion);
	lua_setfield(L, -2, "version");
	lua_pushinteger(L, m->api);
	lua_setfield(L, -2, "api");
	lua_pushinteger(L, TLUA_API_MAX);
	lua_setfield(L, -2, "apimax");
	lua_pushstring(L, tlua_platform());
	lua_setfield(L, -2, "platform");
	lua_pushboolean(L, vid_touchscreen.integer != 0);
	lua_setfield(L, -2, "touch");
	lua_pushstring(L, m->realm == &tlua_server ? "server" : "client");
	lua_setfield(L, -2, "realm");
	lua_pushstring(L, m->realm == &tlua_server ? "none" : k == 2 ? "godnotic" : k == 1 ? "foreign" : "none");
	lua_setfield(L, -2, "csqc");
	lua_pushboolean(L, TLua_Offline(NULL));
	lua_setfield(L, -2, "offline");
	lua_newtable(L);
	for (i = 0; caps[i]; i++)
	{
		lua_pushboolean(L, tlua_cap(m, caps[i]));
		lua_setfield(L, -2, caps[i]);
	}
	lua_setfield(L, -2, "caps");
	return 1;
}

// the offline-only rule of the see-through helpers (touch_weapons.h): the same
// check, so the Lua API can never be a way around it
qbool TLua_Offline(const char **why)
{
	touch_practice_env_t e;
	int i, g;
	memset(&e, 0, sizeof(e));
	e.demo = cls.demoplayback;
	e.server_active = sv.active;
	e.local_connection = cls.state == ca_connected && cls.netcon && LHNETADDRESS_GetAddressType(&cls.netcon->peeraddress) == LHNETADDRESSTYPE_LOOP;
	if (sv.active && svs.clients)
		for (i = 0; i < svs.maxclients; i++)
		{
			const client_t *c = &svs.clients[i];
			if (c->active && c->netconnection && LHNETADDRESS_GetAddressType(&c->netconnection->peeraddress) != LHNETADDRESSTYPE_LOOP)
				e.other_humans++;
		}
	g = TouchWep_PracticeGate(&e);
	if (why)
		*why = touch_practice_why[g];
	return g == TW_GATE_OPEN;
}

// ---------------------------------------------------------------------------
// xt.locale: the game's language and the mod's own string tables

extern cvar_t prvm_language;
static char tlua_key_locale;

int TLuaAPI_locale_lang(lua_State *L)
{
	const char *l = prvm_language.string;
	lua_pushstring(L, (*l && strcmp(l, "dump")) ? l : "en");
	return 1;
}

// locale/<lang>.txt: "key = text" lines, # comments; \n in the text is a line break
static void tlua_locale_load(lua_State *L, tlua_mod_t *m, const char *lang)
{
	char rel[64];
	const char *src;
	size_t len, i = 0;
	int keys = 0;
	dpsnprintf(rel, sizeof(rel), "locale/%s.txt", lang);
	lua_newtable(L);
	if (!TLua_ValidModName(lang) || !(src = TLua_ReadModFile(L, m, rel, &len)) || len > 64 * 1024)
		return;
	while (i < len && keys < 512)
	{
		size_t a = i, eq, b;
		while (i < len && src[i] != '\n')
			i++;
		b = i++;
		while (a < b && (src[a] == ' ' || src[a] == '\t'))
			a++;
		if (a >= b || src[a] == '#')
			continue;
		for (eq = a; eq < b && src[eq] != '='; eq++)
			;
		if (eq >= b)
			continue;
		{
			size_t ke = eq, vs = eq + 1, ve = b;
			luaL_Buffer buf;
			while (ke > a && (src[ke - 1] == ' ' || src[ke - 1] == '\t'))
				ke--;
			while (vs < ve && (src[vs] == ' ' || src[vs] == '\t'))
				vs++;
			while (ve > vs && (src[ve - 1] == '\r' || src[ve - 1] == ' '))
				ve--;
			lua_pushlstring(L, src + a, ke - a);
			luaL_buffinit(L, &buf);
			for (; vs < ve; vs++)
			{
				if (src[vs] == '\\' && vs + 1 < ve && src[vs + 1] == 'n')
				{
					luaL_addchar(&buf, '\n');
					vs++;
				}
				else
					luaL_addchar(&buf, src[vs]);
			}
			luaL_pushresult(&buf);
			lua_rawset(L, -3);
			keys++;
		}
	}
}

int TLuaAPI_locale_t(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	const char *key = luaL_checkstring(L, 1);
	const char *lang, *langs[2];
	int n = lua_gettop(L), i;
	TLuaAPI_locale_lang(L);
	lang = lua_tostring(L, -1);
	langs[0] = lang;
	langs[1] = "en";
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_locale) != LUA_TTABLE)
	{
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_rawsetp(L, LUA_REGISTRYINDEX, &tlua_key_locale);
	}
	for (i = 0; i < 2; i++)
	{
		if (lua_getfield(L, -1, langs[i]) != LUA_TTABLE)
		{
			lua_pop(L, 1);
			tlua_locale_load(L, m, langs[i]);
			lua_pushvalue(L, -1);
			lua_setfield(L, -3, langs[i]);
		}
		if (lua_getfield(L, -1, key) == LUA_TSTRING)
			break;
		lua_pop(L, 2);
	}
	if (i == 2)
		lua_pushvalue(L, 1);
	if (n > 1)
	{
		// string.format with the extra arguments, %s only
		const char *fmt = lua_tostring(L, -1);
		luaL_Buffer b;
		int arg = 2;
		luaL_buffinit(L, &b);
		for (; *fmt; fmt++)
		{
			if (fmt[0] == '%' && fmt[1] == 's' && arg <= n)
			{
				luaL_tolstring(L, arg++, NULL);
				luaL_addvalue(&b);
				fmt++;
			}
			else
				luaL_addchar(&b, *fmt);
		}
		luaL_pushresult(&b);
	}
	return 1;
}

// ---------------------------------------------------------------------------
// xt.engine: Full engine access

static qbool tlua_engine_cmd_ok(const char *line, const char **why)
{
	static const char *const manager[] = {"lua_mod", "sv_lua_plugin", "sv_lua_mod", "lua_install", "lua_page", "lua_channel", NULL};
	const char *p;
	int i;
	for (p = line; *p; p++)
		if ((unsigned char)*p < 32 || *p == '$')
		{
			*why = "control characters and $ (cvar expansion) are refused";
			return false;
		}
	for (p = line; *p == ' '; p++)
		;
	for (i = 0; manager[i]; i++)
	{
		char semi[32];
		dpsnprintf(semi, sizeof(semi), ";%s", manager[i]);
		if (!strncasecmp(p, manager[i], strlen(manager[i])) || strstr(line, semi))
		{
			*why = "the mod manager's commands are the player's own";
			return false;
		}
	}
	return true;
}

int TLuaAPI_engine_cmd(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	const char *line = luaL_checkstring(L, 1), *why = NULL;
	char text[1100];
	if (strlen(line) > 1024)
		return luaL_error(L, "xt.engine.cmd: over 1024 characters");
	if (!tlua_engine_cmd_ok(line, &why))
		return luaL_error(L, "xt.engine.cmd: %s", why);
	if (m->engcmds-- <= 0)
		return luaL_error(L, "xt.engine.cmd: too many commands in one call (16)");
	dpsnprintf(text, sizeof(text), "%s\n", line);
	Cbuf_AddText(cmd_local, text);
	return 0;
}

int TLuaAPI_engine_cvar(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	cvar_t *v = Cvar_FindVar(&cvars_all, name, ~0);
	if (!v || (v->flags & CF_PRIVATE) || TLua_CvarSecret(name))
		lua_pushnil(L);
	else
		lua_pushstring(L, v->string);
	return 1;
}

int TLuaAPI_engine_setcvar(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	const char *value = lua_type(L, 2) == LUA_TBOOLEAN ? (lua_toboolean(L, 2) ? "1" : "0") : luaL_checkstring(L, 2);
	cvar_t *v = Cvar_FindVar(&cvars_all, name, ~0);
	if (!v || (v->flags & (CF_PRIVATE | CF_READONLY)) || TLua_CvarSecret(name) || strlen(value) > 1024)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	Cvar_SetQuick(v, value);
	lua_pushboolean(L, 1);
	return 1;
}

// ---------------------------------------------------------------------------
// xt.hook / xt.hooknames: the realm's hook set (touch_lua_hooks.c)

int TLuaAPI_hook(lua_State *L) { return TLua_Hooks_Subscribe(L); }
int TLuaAPI_hooknames(lua_State *L) { return TLua_Hooks_Names(L); }

// ---------------------------------------------------------------------------
// xt.http: HTTPS to the hosts a mod's manifest lists (Internet tier), through
// the engine's libcurl: one transfer runs between frames like any download;
// the answer is handed to the mod's callback at the start of a later frame,
// under the usual limits.

#define TLUA_HTTP_MAX 32
#define TLUA_HTTP_BODY (256 * 1024)
#define TLUA_HTTP_POST (64 * 1024)
typedef struct tlua_http_s
{
	qbool used, done, delivered;
	tlua_realm_t *realm;
	char mod[TLUA_NAME_LEN + 1];
	int ref;
	int status;
	size_t length;
	unsigned char *buf;
	unsigned char *post;
	double started;
} tlua_http_t;
static tlua_http_t tlua_http[TLUA_HTTP_MAX];

// libcurl.c asks touch_lua_pkg.c, which asks this, for each transfer: ours
// stay on HTTP(S) for the request itself and HTTPS for any redirect; 0
// leaves a transfer alone. Which URL a mod may start with was checked when
// it asked (https://, or http:// to this machine only).
static void tlua_http_cb(int status, size_t length, unsigned char *buffer, void *cbdata);
static qbool tlua_http_local;
long TLua_Http_Protocols(void (*callback)(int, size_t, unsigned char *, void *), qbool redirect);
long TLua_Http_Protocols(void (*callback)(int, size_t, unsigned char *, void *), qbool redirect)
{
	if (callback != tlua_http_cb)
		return 0;
	return redirect ? 2 : (1 | 2);
}

static void tlua_http_cb(int status, size_t length, unsigned char *buffer, void *cbdata)
{
	tlua_http_t *h = (tlua_http_t *)cbdata;
	(void)buffer;
	if (h < tlua_http || h >= tlua_http + TLUA_HTTP_MAX || !h->used)
		return;
	h->done = true;
	h->status = status == CURLCBSTATUS_OK ? 200 : status > 0 ? status : status == CURLCBSTATUS_FAILED && length > TLUA_HTTP_BODY ? -2 : -1;
	h->length = status == CURLCBSTATUS_OK ? min(length, (size_t)TLUA_HTTP_BODY) : 0;
}

static qbool tlua_http_host_ok(tlua_mod_t *m, const char *url, qbool *local, char *hn, size_t hostsize)
{
	const char *p, *h;
	size_t n = 0;
	char list[256], *tok;
	*local = false;
	if (!strncmp(url, "https://", 8))
		h = url + 8;
	else if (!strncmp(url, "http://", 7))
		h = url + 7, *local = true;
	else
		return false;
	for (p = h; *p && *p != '/' && *p != ':' && *p != '?' && n + 1 < hostsize; p++)
		hn[n++] = (char)tolower((unsigned char)*p);
	hn[n] = 0;
	if (!n || strchr(h, '@') || strpbrk(url, " \"\\\r\n"))
		return false;
	if (*local && strcmp(hn, "localhost") && strcmp(hn, "127.0.0.1"))
		return false; // plain http only to this machine (tests, a local leaderboard)
	dp_strlcpy(list, m->httphosts, sizeof(list));
	for (tok = strtok(list, ", "); tok; tok = strtok(NULL, ", "))
	{
		char *colon = strchr(tok, ':');
		if (colon)
			*colon = 0;
		if (!strcasecmp(tok, hn))
			return true;
	}
	return false;
}

static int tlua_http_start(lua_State *L, const char *url, const char *body, size_t bodylen, const char *type, int fnidx)
{
	tlua_mod_t *m = TLua_Self(L);
	static double window[2];
	static int count;
	char hn[128];
	qbool local;
	int i, mine = 0;
	tlua_http_t *h = NULL;
	if (!tlua_has_http())
		return luaL_error(L, "xt.http: not available in this build (xt.game.has(\"http\"))");
	if (strlen(url) > 1024 || !tlua_http_host_ok(m, url, &local, hn, sizeof(hn)))
		return luaL_error(L, "xt.http: %s is not a host this mod's manifest lists (http: ...), or not https", url);
	for (i = 0; i < TLUA_HTTP_MAX; i++)
	{
		if (tlua_http[i].used && tlua_http[i].realm == m->realm && !strcmp(tlua_http[i].mod, m->name))
			mine++;
		else if (!tlua_http[i].used && !h)
			h = &tlua_http[i];
	}
	if (mine >= 4 || !h)
		return luaL_error(L, "xt.http: too many requests at once (4 per mod)");
	// 30 a minute per mod (a simple window shared by all: the cap is per call site)
	if (host.realtime - window[0] > 60)
	{
		window[0] = host.realtime;
		count = 0;
	}
	if (++count > 30 * 4)
		return luaL_error(L, "xt.http: too many requests this minute");
	memset(h, 0, sizeof(*h));
	h->used = true;
	h->realm = m->realm;
	dp_strlcpy(h->mod, m->name, sizeof(h->mod));
	h->buf = (unsigned char *)Z_Malloc(TLUA_HTTP_BODY + 1);
	h->started = host.realtime;
	if (body)
	{
		h->post = (unsigned char *)Z_Malloc(bodylen + 1);
		memcpy(h->post, body, bodylen);
	}
	h->ref = TLua_Ref(L, fnidx);
	tlua_http_local = local;
#ifdef __EMSCRIPTEN__
	if (!TouchWeb_Begin_ToMemory_POST(url, body ? type : NULL, body ? h->post : NULL, body ? bodylen : 0, h->buf, TLUA_HTTP_BODY, local, tlua_http_cb, h))
#else
	if (!Curl_Begin_ToMemory_POST(url, NULL, 0, body ? type : NULL, h->post, body ? bodylen : 0, h->buf, TLUA_HTTP_BODY, tlua_http_cb, h))
#endif
	{
		tlua_http_local = false;
		TLua_Unref(m, h->ref);
		Z_Free(h->buf);
		if (h->post)
			Z_Free(h->post);
		h->used = false;
		return luaL_error(L, "xt.http: the request could not start");
	}
	tlua_http_local = false;
	lua_pushboolean(L, 1);
	return 1;
}

int TLuaAPI_http_get(lua_State *L)
{
	const char *url = luaL_checkstring(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	return tlua_http_start(L, url, NULL, 0, NULL, 2);
}

int TLuaAPI_http_post(lua_State *L)
{
	size_t len;
	const char *url = luaL_checkstring(L, 1);
	const char *body = luaL_checklstring(L, 2, &len);
	const char *type = "application/json";
	luaL_checktype(L, 3, LUA_TFUNCTION);
	if (len > TLUA_HTTP_POST)
		return luaL_error(L, "xt.http.post: body over 64 KB");
	if (lua_type(L, 4) == LUA_TTABLE)
	{
		lua_getfield(L, 4, "type");
		if (lua_type(L, -1) == LUA_TSTRING && strlen(lua_tostring(L, -1)) < 64 && !strpbrk(lua_tostring(L, -1), "\r\n"))
			type = lua_tostring(L, -1);
	}
	return tlua_http_start(L, url, body, len, type, 3);
}

typedef struct tlua_httpans_s { int status; const char *body; size_t len; } tlua_httpans_t;
static int tlua_http_push(lua_State *L, void *ud)
{
	tlua_httpans_t *a = (tlua_httpans_t *)ud;
	lua_pushinteger(L, a->status);
	lua_pushlstring(L, a->body, a->len);
	return 2;
}

static void tlua_http_free(tlua_http_t *h)
{
	if (h->buf)
		Z_Free(h->buf);
	if (h->post)
		Z_Free(h->post);
	h->buf = h->post = NULL;
	h->used = false;
}

static void tlua_http_frame(void)
{
	int i;
	for (i = 0; i < TLUA_HTTP_MAX; i++)
	{
		tlua_http_t *h = &tlua_http[i];
		tlua_mod_t *m;
		tlua_httpans_t a;
		if (!h->used)
			continue;
		if (!h->done)
		{
			if (host.realtime - h->started > 15)
			{
				// libcurl owns the buffer until it is done: stop waiting, keep it
#ifdef __EMSCRIPTEN__
				TouchWeb_Cancel_ToMemory(tlua_http_cb, h);
#else
				Curl_Cancel_ToMemory(tlua_http_cb, h);
#endif
				h->done = true;
				h->status = -3;
				h->length = 0;
			}
			else
				continue;
		}
		m = TLua_Find(h->realm, h->mod);
		if (m && m->state == TLUA_RUNNING && m->L)
		{
			a.status = h->status;
			a.body = (const char *)h->buf;
			a.len = h->length;
			TLua_CallRef(m, h->ref, tlua_http_push, &a, NULL);
			TLua_Unref(m, h->ref);
		}
		tlua_http_free(h);
	}
}

static void tlua_http_modstopped(tlua_mod_t *m)
{
	int i;
	for (i = 0; i < TLUA_HTTP_MAX; i++)
	{
		tlua_http_t *h = &tlua_http[i];
		if (!h->used || h->realm != m->realm || strcmp(h->mod, m->name))
			continue;
		if (!h->done)
#ifdef __EMSCRIPTEN__
			TouchWeb_Cancel_ToMemory(tlua_http_cb, h);
#else
			Curl_Cancel_ToMemory(tlua_http_cb, h);
#endif
		tlua_http_free(h);
	}
}

// ---------------------------------------------------------------------------
// Life cycle

void TLua_API_ModStopped(tlua_mod_t *m)
{
	tlua_http_modstopped(m);
	TLua_Chan_ModStopped(m);
	TLua_UI_ModStopped(m);
	if (m->realm == &tlua_server)
		TLua_SVAPI_ModStopped(m);
	else
		TLua_CLAPI_ModStopped(m);
}

void TLua_API_Frame(void)
{
	static double lasttop;
	if (TLua_Busy())
		return;
	tlua_http_frame();
	TLua_Chan_Frame();
	if (cls.state != ca_dedicated && host.realtime - lasttop >= 1)
	{
		static char buf[8192];
		lasttop = host.realtime;
		tlua_top_json(buf, sizeof(buf));
		if (strcmp(buf, lua_top.string))
			Cvar_SetQuick(&lua_top, buf);
	}
}

void TLua_API_Setup(void)
{
	TLua_RegisterCvar(&lua_frame_ms);
	TLua_Guard(&lua_frame_ms);
	if (cls.state != ca_dedicated)
		TLua_RegisterCvar(&lua_top);
	Cmd_AddCommand(CF_CLIENT | CF_SERVER, "lua_mod_top", tlua_top_f, "per-mod CPU, calls, skipped calls and memory over the last second (json: the same as JSON)");
	TLua_Chan_Setup();
	TLua_UI_Setup();
	TLua_SVAPI_Setup();
	TLua_CLAPI_Setup();
}
