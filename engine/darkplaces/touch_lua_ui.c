/*
Godnotic: pages Lua mods describe (docs/LUA_API.md, "UI descriptions").

A mod describes a screen as rows (section, card, label, switch, slider,
choice, text, color, keybind, button, list, image, progress) with
xt.settings.page{...} (shown under Settings > Mods) or xt.menu.page{...} (its
own page in the Mods menu); a server mod shows one on a player's screen with
xt.page.show(id, desc). The mod's manifest settings (setting.<name>) become a
settings page of their own without any code.

The engine checks and bounds the description (64 rows, depth 4, strings 256
bytes, 64 list items, 8 pages per mod, 16 KB of JSON), turns it into one
renderer-neutral JSON document and hands it to the UI store through four
calls; the renderers (the QuakeC menu, Android, the Linux app) draw it with
their own widgets and send events back through the fifth:

  tlua_ui_register(id, origin, kind, json)   a page appears
  tlua_ui_update(id, json)                   its content changed
  tlua_ui_remove(id)                         it went away
  tlua_ui_list(...)                          (the store's index, for the stub)
  tlua_ui_event(id, row, event, value)       a renderer: "change", "press", "select"

Until the UI bridge's store (docs/UI_BRIDGE.md, work/uibridge) exists, the
backend here is a stub that keeps the JSON in memory and publishes it for
tests and UIs: the cvar _lua_pages (an index), _lua_page (the page that
_lua_page_select names), and the commands lua_pages, lua_page_json <id>,
lua_page_event <id> <row> <event> [value].

An event reaches the mod under the usual limits: a row bound to a setting
(setting = "<name>") first sets that cvar, checked against the manifest's type
and range (so the value persists in the config), then the row's on_change /
on_press / on_select and the page's on_change run, and the page is
published again with whatever they changed.
*/

#include "quakedef.h"
#include "touch_lua.h"
#include "touch_lua_int.h"
#include "touch_lua_api_gen.h"

static cvar_t lua_pages = {CF_CLIENT | CF_READONLY, "_lua_pages", "", "pages Lua mods describe, JSON index: [{\"id\",\"origin\",\"kind\",\"title\",\"from_server\"}] (docs/LUA_API.md, UI descriptions)"};
static cvar_t lua_page_select = {CF_CLIENT, "_lua_page_select", "", "the page _lua_page shows (an id from _lua_pages)"};
static cvar_t lua_page = {CF_CLIENT | CF_READONLY, "_lua_page", "", "the JSON of the page _lua_page_select names"};

void TLua_RegisterCvar(cvar_t *var);
size_t TLua_JsonEncode(lua_State *L, int idx, char *out, size_t outsize);
void TLua_JsonAppendString(char *out, size_t size, const char *s);

#define TLUA_UI_MAXPAGES 96
#define TLUA_UI_PERMOD 8
#define TLUA_UI_MAXROWS 64
#define TLUA_UI_MAXDEPTH 4
#define TLUA_UI_MAXSTR 256
#define TLUA_UI_MAXITEMS 64
#define TLUA_UI_MAXJSON (16 * 1024)

typedef struct tlua_uipage_s
{
	qbool used;
	char id[96];
	char origin[64];               // the mod's name; "server:<mod>" for a page a server sent
	tlua_realm_t *realm;           // the owner's realm (NULL: a server's page on this client)
	char mod[TLUA_NAME_LEN + 1];
	char kind[12];                 // settings, menu, manifest, server
	char title[64];
	char *json;
	int pref, dref;                // the page object and the dispatcher in the mod's state
} tlua_uipage_t;

static tlua_uipage_t tlua_pages[TLUA_UI_MAXPAGES];
static qbool tlua_pages_dirty;
static char tlua_key_pagemt, tlua_key_dispatch;

qbool tlua_ui_event(const char *id, const char *row, const char *event, const char *value);

// ---------------------------------------------------------------------------
// The store interface (stub backend). The UI bridge replaces the bodies of
// these five functions; nothing else here knows where pages go.

static tlua_uipage_t *tlua_ui_find(const char *id)
{
	int i;
	for (i = 0; i < TLUA_UI_MAXPAGES; i++)
		if (tlua_pages[i].used && !strcmp(tlua_pages[i].id, id))
			return &tlua_pages[i];
	return NULL;
}

#if defined(__has_include)
#if __has_include("touch_uib.h")
#define TLUA_HAVE_UIB 1
#endif
#endif

#ifdef TLUA_HAVE_UIB
// ---- the UI bridge's store (docs/UI_BRIDGE.md, "Mods"): our JSON (schema 1,
// docs/LUA_API.md) turned into the bridge's page shape: a flat list of nodes,
// the first the page's root section, ids relative to the page
#include "touch_uib.h"

typedef struct tlua_jn_s
{
	char type;             // o object, a array, s string, n number, t true, f false, z null
	const char *s; int len; // string (raw, escapes kept) or number text
	int first, next;       // first child, next sibling (indices, -1: none); object members: key node then value node
} tlua_jn_t;
#define TLUA_JN_MAX 4096
static tlua_jn_t tlua_jn[TLUA_JN_MAX];
static int tlua_jn_count;

static void tlua_jn_ws(const char **p) { while (**p == ' ' || **p == '\n' || **p == '\t' || **p == '\r') (*p)++; }

static int tlua_jn_parse(const char **p, int depth)
{
	int me, last = -1;
	tlua_jn_ws(p);
	if (tlua_jn_count >= TLUA_JN_MAX || depth > 32 || !**p)
		return -1;
	me = tlua_jn_count++;
	tlua_jn[me].first = tlua_jn[me].next = -1;
	tlua_jn[me].s = *p;
	tlua_jn[me].len = 0;
	if (**p == '{' || **p == '[')
	{
		char close = **p == '{' ? '}' : ']';
		tlua_jn[me].type = **p == '{' ? 'o' : 'a';
		(*p)++;
		for (;;)
		{
			int c;
			tlua_jn_ws(p);
			if (**p == close) { (*p)++; return me; }
			if (**p == ',') { (*p)++; continue; }
			c = tlua_jn_parse(p, depth + 1);
			if (c < 0)
				return -1;
			if (last < 0) tlua_jn[me].first = c; else tlua_jn[last].next = c;
			last = c;
			if (tlua_jn[me].type == 'o')
			{
				int v;
				tlua_jn_ws(p);
				if (**p != ':')
					return -1;
				(*p)++;
				if ((v = tlua_jn_parse(p, depth + 1)) < 0)
					return -1;
				tlua_jn[last].next = v;
				last = v;
			}
		}
	}
	if (**p == '"')
	{
		const char *q = ++(*p);
		while (**p && **p != '"') { if (**p == '\\' && (*p)[1]) (*p)++; (*p)++; }
		if (!**p)
			return -1;
		tlua_jn[me].type = 's';
		tlua_jn[me].s = q;
		tlua_jn[me].len = (int)(*p - q);
		(*p)++;
		return me;
	}
	if (!strncmp(*p, "true", 4)) { tlua_jn[me].type = 't'; *p += 4; return me; }
	if (!strncmp(*p, "false", 5)) { tlua_jn[me].type = 'f'; *p += 5; return me; }
	if (!strncmp(*p, "null", 4)) { tlua_jn[me].type = 'z'; *p += 4; return me; }
	tlua_jn[me].type = 'n';
	while (**p && strchr("+-0123456789.eE", **p)) (*p)++;
	tlua_jn[me].len = (int)(*p - tlua_jn[me].s);
	return tlua_jn[me].len ? me : -1;
}

static int tlua_jn_get(int obj, const char *key)
{
	int k;
	if (obj < 0 || tlua_jn[obj].type != 'o')
		return -1;
	for (k = tlua_jn[obj].first; k >= 0 && tlua_jn[k].next >= 0; k = tlua_jn[tlua_jn[k].next].next)
		if (tlua_jn[k].len == (int)strlen(key) && !strncmp(tlua_jn[k].s, key, (size_t)tlua_jn[k].len))
			return tlua_jn[k].next;
	return -1;
}

// appends the raw JSON of node n (strings re-quoted as they were)
static void tlua_jn_put(char *out, size_t size, int n)
{
	size_t o = strlen(out);
	if (n < 0) { dp_strlcat(out, "null", size); return; }
	switch (tlua_jn[n].type)
	{
	case 's':
		if (o + (size_t)tlua_jn[n].len + 3 < size)
		{
			out[o++] = '"';
			memcpy(out + o, tlua_jn[n].s, (size_t)tlua_jn[n].len);
			o += (size_t)tlua_jn[n].len;
			out[o++] = '"';
			out[o] = 0;
		}
		break;
	case 'n':
		if (o + (size_t)tlua_jn[n].len + 1 < size)
		{
			memcpy(out + o, tlua_jn[n].s, (size_t)tlua_jn[n].len);
			out[o + tlua_jn[n].len] = 0;
		}
		break;
	case 't': dp_strlcat(out, "true", size); break;
	case 'f': dp_strlcat(out, "false", size); break;
	default: dp_strlcat(out, "null", size); break;
	}
}

static void tlua_jn_prop(char *out, size_t size, const char *name, int n, qbool *first)
{
	if (n < 0)
		return;
	dp_strlcat(out, *first ? "\"" : ",\"", size);
	*first = false;
	dp_strlcat(out, name, size);
	dp_strlcat(out, "\":", size);
	tlua_jn_put(out, size, n);
}

static qbool tlua_jn_is(int n, const char *s)
{
	return n >= 0 && tlua_jn[n].type == 's' && tlua_jn[n].len == (int)strlen(s) && !strncmp(tlua_jn[n].s, s, (size_t)tlua_jn[n].len);
}

static int tlua_uib_seq;
static char tlua_uib_ids[TLUA_JN_MAX][40]; // a row's node id, by its tree index

// node ids first (rows without an id get n1, n2, ...), so parents can be
// written before their children
static void tlua_uib_assign(int row)
{
	int id = tlua_jn_get(row, "id"), kids = tlua_jn_get(row, "children"), c;
	if (id >= 0 && tlua_jn[id].type == 's' && tlua_jn[id].len < (int)sizeof(tlua_uib_ids[0]))
	{
		memcpy(tlua_uib_ids[row], tlua_jn[id].s, (size_t)tlua_jn[id].len);
		tlua_uib_ids[row][tlua_jn[id].len] = 0;
	}
	else
		dpsnprintf(tlua_uib_ids[row], sizeof(tlua_uib_ids[row]), "n%d", ++tlua_uib_seq);
	if (kids >= 0)
		for (c = tlua_jn[kids].first; c >= 0; c = tlua_jn[c].next)
			tlua_uib_assign(c);
}

// one row, then its children, as bridge nodes appended to out
static void tlua_uib_node(char *out, size_t size, const char *mod, int row)
{
	char child_ids[2048] = "", buf[96];
	const char *idout = tlua_uib_ids[row];
	int type = tlua_jn_get(row, "type"), kids = tlua_jn_get(row, "children"), c, opts, items;
	qbool first = true, list = tlua_jn_is(type, "list");
	if (kids >= 0)
		for (c = tlua_jn[kids].first; c >= 0; c = tlua_jn[c].next)
		{
			if (child_ids[0]) dp_strlcat(child_ids, ",", sizeof(child_ids));
			dp_strlcat(child_ids, "\"", sizeof(child_ids));
			dp_strlcat(child_ids, tlua_uib_ids[c], sizeof(child_ids));
			dp_strlcat(child_ids, "\"", sizeof(child_ids));
		}
	if (out[strlen(out) - 1] != '[')
		dp_strlcat(out, ",", size);
	dp_strlcat(out, "{\"id\":\"", size);
	dp_strlcat(out, idout, size);
	dp_strlcat(out, "\",\"type\":", size);
	// a list of items is a choice to the bridge (one of them selected)
	if (list)
		dp_strlcat(out, "\"choice\"", size);
	else
		tlua_jn_put(out, size, type);
	dp_strlcat(out, ",\"origin\":\"mod:", size);
	dp_strlcat(out, mod, size);
	dp_strlcat(out, "\",\"props\":{", size);
	if (tlua_jn_is(type, "section") || tlua_jn_is(type, "card"))
		tlua_jn_prop(out, size, "title", tlua_jn_get(row, tlua_jn_is(type, "card") ? "title" : "text"), &first);
	else if (tlua_jn_is(type, "text"))
		tlua_jn_prop(out, size, "body", tlua_jn_get(row, "text"), &first);
	else if (tlua_jn_is(type, "image"))
	{
		int f = tlua_jn_get(row, "file");
		if (f >= 0)
		{
			char ref[160];
			dpsnprintf(ref, sizeof(ref), "%s\"ref\":\"mod:%s/%.*s\"", first ? "" : ",", mod, tlua_jn[f].len, tlua_jn[f].s);
			dp_strlcat(out, ref, size);
			first = false;
		}
	}
	else
	{
		int v = tlua_jn_get(row, "value");
		tlua_jn_prop(out, size, "label", tlua_jn_get(row, "label"), &first);
		tlua_jn_prop(out, size, "help", tlua_jn_get(row, "desc"), &first);
		if (!tlua_jn_is(type, "button") && !tlua_jn_is(type, "progress"))
		{
			dpsnprintf(buf, sizeof(buf), "%s\"bind\":\"mod:%s.%s\"", first ? "" : ",", mod, idout);
			dp_strlcat(out, buf, size);
			first = false;
		}
		if (tlua_jn_is(type, "colour") && v >= 0 && tlua_jn[v].type == 's')
		{
			dpsnprintf(buf, sizeof(buf), ",\"value\":\"#%.*s\"", tlua_jn[v].len, tlua_jn[v].s);
			dp_strlcat(out, buf, size);
		}
		else if (tlua_jn_is(type, "keybind") && v >= 0)
		{
			dp_strlcat(out, ",\"value\":[", size);
			tlua_jn_put(out, size, v);
			dp_strlcat(out, "]", size);
		}
		else
			tlua_jn_prop(out, size, "value", v, &first);
		tlua_jn_prop(out, size, "min", tlua_jn_get(row, "min"), &first);
		tlua_jn_prop(out, size, "max", tlua_jn_get(row, "max"), &first);
		tlua_jn_prop(out, size, "step", tlua_jn_get(row, "step"), &first);
		tlua_jn_prop(out, size, "placeholder", tlua_jn_get(row, "placeholder"), &first);
		tlua_jn_prop(out, size, "maxLength", tlua_jn_get(row, "maxlen"), &first);
		if (tlua_jn_is(type, "button"))
		{
			dpsnprintf(buf, sizeof(buf), ",\"action\":{\"id\":\"%s\"}", idout);
			dp_strlcat(out, buf, size);
			tlua_jn_prop(out, size, "style", tlua_jn_get(row, "style"), &first);
		}
		if ((opts = tlua_jn_get(row, "options")) >= 0)
		{
			dp_strlcat(out, ",\"options\":[", size);
			for (c = tlua_jn[opts].first; c >= 0; c = tlua_jn[c].next)
			{
				dp_strlcat(out, c == tlua_jn[opts].first ? "{\"value\":" : ",{\"value\":", size);
				tlua_jn_put(out, size, c);
				dp_strlcat(out, ",\"label\":", size);
				tlua_jn_put(out, size, c);
				dp_strlcat(out, "}", size);
			}
			dp_strlcat(out, "]", size);
		}
		if (list && (items = tlua_jn_get(row, "items")) >= 0)
		{
			dp_strlcat(out, ",\"style\":\"radio\",\"options\":[", size);
			for (c = tlua_jn[items].first; c >= 0; c = tlua_jn[c].next)
			{
				dp_strlcat(out, c == tlua_jn[items].first ? "{\"value\":" : ",{\"value\":", size);
				tlua_jn_put(out, size, tlua_jn_get(c, "id"));
				dp_strlcat(out, ",\"label\":", size);
				tlua_jn_put(out, size, tlua_jn_get(c, "label"));
				if (tlua_jn_get(c, "detail") >= 0)
				{
					dp_strlcat(out, ",\"help\":", size);
					tlua_jn_put(out, size, tlua_jn_get(c, "detail"));
				}
				dp_strlcat(out, "}", size);
			}
			dp_strlcat(out, "]", size);
		}
		if (tlua_jn_is(tlua_jn_get(row, "disabled"), "x") || (tlua_jn_get(row, "disabled") >= 0 && tlua_jn[tlua_jn_get(row, "disabled")].type == 't'))
			dp_strlcat(out, ",\"enabled\":false", size);
	}
	dp_strlcat(out, "},\"children\":[", size);
	dp_strlcat(out, child_ids, size);
	dp_strlcat(out, "]}", size);
	if (kids >= 0)
		for (c = tlua_jn[kids].first; c >= 0; c = tlua_jn[c].next)
			tlua_uib_node(out, size, mod, c);
}

// the bridge's (mod id, page key) for one of our page ids
static void tlua_uib_key(const char *id, char *mod, size_t modsize, char *key, size_t keysize)
{
	const char *p = id, *dot;
	qbool server = !strncmp(p, "server.", 7);
	if (server)
		p += 7;
	dot = strchr(p, '.');
	dp_strlcpy(mod, p, min(modsize, dot ? (size_t)(dot - p) + 1 : modsize));
	dpsnprintf(key, keysize, "%s%s", server ? "server-" : "", dot ? dot + 1 : "page");
}

static void tlua_uib_set(const char *id, const char *json)
{
	static char out[96 * 1024];
	char mod[64], key[96], err[256], rootids[4096] = "";
	const char *p = json;
	int root, rows, title, c;
	tlua_jn_count = 0;
	tlua_uib_seq = 0;
	if ((root = tlua_jn_parse(&p, 0)) < 0 || (rows = tlua_jn_get(root, "rows")) < 0)
		return;
	tlua_uib_key(id, mod, sizeof(mod), key, sizeof(key));
	title = tlua_jn_get(root, "title");
	// the rows first (children before the root here; the root goes first below)
	dp_strlcpy(out, "[", sizeof(out));
	for (c = tlua_jn[rows].first; c >= 0; c = tlua_jn[c].next)
		tlua_uib_assign(c);
	for (c = tlua_jn[rows].first; c >= 0; c = tlua_jn[c].next)
	{
		tlua_uib_node(out, sizeof(out), mod, c);
		if (rootids[0]) dp_strlcat(rootids, ",", sizeof(rootids));
		dp_strlcat(rootids, "\"", sizeof(rootids));
		dp_strlcat(rootids, tlua_uib_ids[c], sizeof(rootids));
		dp_strlcat(rootids, "\"", sizeof(rootids));
	}
	{
		static char page[100 * 1024];
		dp_strlcpy(page, "{\"title\":", sizeof(page));
		tlua_jn_put(page, sizeof(page), title);
		dp_strlcat(page, ",\"nodes\":[{\"id\":\"root\",\"type\":\"section\",\"props\":{\"title\":", sizeof(page));
		tlua_jn_put(page, sizeof(page), title);
		dp_strlcat(page, "},\"children\":[", sizeof(page));
		dp_strlcat(page, rootids, sizeof(page));
		dp_strlcat(page, "]}", sizeof(page));
		if (strlen(out) > 1)
		{
			dp_strlcat(page, ",", sizeof(page));
			dp_strlcat(page, out + 1, sizeof(page));
		}
		else
			dp_strlcat(page, "]", sizeof(page));
		dp_strlcat(page, out[1] ? "]}" : "}", sizeof(page)); // out has no closing bracket
		if (TouchUIB_ModSetPage(mod, key, page, strlen(page), err, sizeof(err)) < 0)
			Con_Printf(CON_WARN "Lua: the UI bridge refused page %s: %s\n", id, err);
	}
}

// the bridge's events on mod nodes: {"ev":"value"|"action", "node":"...", "value":...}
static void tlua_uib_event(const char *mod_id, const char *ev_json, size_t len, void *ctx)
{
	char buf[2048], ev[16] = "", node[160] = "", value[512] = "", page[160], row[64];
	const char *p, *slash;
	int root, n;
	(void)ctx;
	if (len >= sizeof(buf))
		return;
	memcpy(buf, ev_json, len);
	buf[len] = 0;
	p = buf;
	tlua_jn_count = 0;
	if ((root = tlua_jn_parse(&p, 0)) < 0)
		return;
	if ((n = tlua_jn_get(root, "ev")) >= 0 && tlua_jn[n].type == 's')
		dp_strlcpy(ev, tlua_jn[n].s, min(sizeof(ev), (size_t)tlua_jn[n].len + 1));
	if ((n = tlua_jn_get(root, "node")) >= 0 && tlua_jn[n].type == 's')
		dp_strlcpy(node, tlua_jn[n].s, min(sizeof(node), (size_t)tlua_jn[n].len + 1));
	if ((n = tlua_jn_get(root, "value")) >= 0)
	{
		if (tlua_jn[n].type == 'a')
			n = tlua_jn[n].first; // a keybind's names: the first
		if (n >= 0 && tlua_jn[n].type == 't') dp_strlcpy(value, "1", sizeof(value));
		else if (n >= 0 && tlua_jn[n].type == 'f') dp_strlcpy(value, "0", sizeof(value));
		else if (n >= 0 && (tlua_jn[n].type == 's' || tlua_jn[n].type == 'n'))
			dp_strlcpy(value, tlua_jn[n].s + (tlua_jn[n].type == 's' && tlua_jn[n].s[0] == '#' ? 1 : 0), min(sizeof(value), (size_t)tlua_jn[n].len + 1 - (tlua_jn[n].s[0] == '#' ? 1 : 0)));
	}
	// node: "mods.page.<mod>.<page key>/<row>[/<row>...]": the page key and the last row
	slash = strrchr(node, '/');
	if (!slash)
		return;
	dp_strlcpy(row, slash + 1, sizeof(row));
	{
		char prefix[96], k[96];
		const char *q;
		size_t kl;
		dpsnprintf(prefix, sizeof(prefix), "mods.page.%s.", mod_id);
		if (strncmp(node, prefix, strlen(prefix)))
			return;
		q = node + strlen(prefix);
		kl = strcspn(q, "/");
		if (kl >= sizeof(k))
			return;
		memcpy(k, q, kl);
		k[kl] = 0;
		if (!strncmp(k, "server-", 7))
			dpsnprintf(page, sizeof(page), "server.%s.%s", mod_id, k + 7);
		else
			dpsnprintf(page, sizeof(page), "%s.%s", mod_id, k);
	}
	if (!strcmp(ev, "action"))
		tlua_ui_event(page, row, "press", "");
	else if (!strcmp(ev, "value") || !strcmp(ev, "submit"))
	{
		tlua_uipage_t *pg = tlua_ui_find(page);
		char key[64];
		const char *t;
		// a list (sent as a choice) answers with select
		dpsnprintf(key, sizeof(key), "\"id\":\"%s\",\"type\":\"list\"", row);
		t = pg && pg->json ? strstr(pg->json, key) : NULL;
		if (!t)
		{
			dpsnprintf(key, sizeof(key), "\"type\":\"list\",\"id\":\"%s\"", row);
			t = pg && pg->json ? strstr(pg->json, key) : NULL;
		}
		tlua_ui_event(page, row, t ? "select" : "change", value);
	}
}
#endif

static qbool tlua_ui_register(const char *id, const char *origin, const char *kind, const char *json)
{
	(void)origin; (void)kind;
#ifdef TLUA_HAVE_UIB
	tlua_uib_set(id, json);
#else
	(void)id; (void)json;
#endif
	tlua_pages_dirty = true;
	return true;
}

static void tlua_ui_update(const char *id, const char *json)
{
#ifdef TLUA_HAVE_UIB
	tlua_uib_set(id, json);
#else
	(void)id; (void)json;
#endif
	tlua_pages_dirty = true;
}

static void tlua_ui_remove(const char *id)
{
#ifdef TLUA_HAVE_UIB
	char mod[64], key[96];
	tlua_uib_key(id, mod, sizeof(mod), key, sizeof(key));
	TouchUIB_ModRemove(mod, key);
#else
	(void)id;
#endif
	tlua_pages_dirty = true;
}

static void tlua_ui_publish(void)
{
	static char buf[16384];
	int i;
	qbool first = true;
	tlua_uipage_t *sel;
	if (cls.state == ca_dedicated)
		return;
	tlua_pages_dirty = false;
	dp_strlcpy(buf, "[", sizeof(buf));
	for (i = 0; i < TLUA_UI_MAXPAGES; i++)
	{
		tlua_uipage_t *p = &tlua_pages[i];
		if (!p->used)
			continue;
		if (!first)
			dp_strlcat(buf, ",", sizeof(buf));
		first = false;
		dp_strlcat(buf, "{\"id\":", sizeof(buf));
		TLua_JsonAppendString(buf, sizeof(buf), p->id);
		dp_strlcat(buf, ",\"origin\":", sizeof(buf));
		TLua_JsonAppendString(buf, sizeof(buf), p->origin);
		dp_strlcat(buf, ",\"kind\":", sizeof(buf));
		TLua_JsonAppendString(buf, sizeof(buf), p->kind);
		dp_strlcat(buf, ",\"title\":", sizeof(buf));
		TLua_JsonAppendString(buf, sizeof(buf), p->title);
		dp_strlcat(buf, p->realm ? ",\"from_server\":false}" : ",\"from_server\":true}", sizeof(buf));
	}
	dp_strlcat(buf, "]", sizeof(buf));
	if (strcmp(buf, lua_pages.string))
		Cvar_SetQuick(&lua_pages, buf);
	sel = tlua_ui_find(lua_page_select.string);
	if (strcmp(sel && sel->json ? sel->json : "", lua_page.string))
		Cvar_SetQuick(&lua_page, sel && sel->json ? sel->json : "");
}

// ---------------------------------------------------------------------------
// Pages in the engine's table

static void tlua_page_setjson(tlua_uipage_t *p, const char *json)
{
	qbool fresh = p->json == NULL;
	if (p->json && !strcmp(p->json, json))
		return;
	if (p->json)
		Z_Free(p->json);
	p->json = (char *)Z_Malloc(strlen(json) + 1);
	memcpy(p->json, json, strlen(json) + 1);
	if (fresh)
		tlua_ui_register(p->id, p->origin, p->kind, p->json);
	else
		tlua_ui_update(p->id, p->json);
}

static void tlua_page_free(tlua_uipage_t *p)
{
	tlua_ui_remove(p->id);
	if (p->json)
		Z_Free(p->json);
	memset(p, 0, sizeof(*p));
	tlua_pages_dirty = true;
}

static tlua_uipage_t *tlua_page_new(const char *id)
{
	int i;
	tlua_uipage_t *p = tlua_ui_find(id);
	if (p)
		return p;
	for (i = 0; i < TLUA_UI_MAXPAGES; i++)
		if (!tlua_pages[i].used)
		{
			p = &tlua_pages[i];
			memset(p, 0, sizeof(*p));
			p->used = true;
			dp_strlcpy(p->id, id, sizeof(p->id));
			return p;
		}
	return NULL;
}

// ---------------------------------------------------------------------------
// The description -> JSON

typedef struct tlua_jw_s
{
	char *buf;
	size_t size;
	int rows;
	tlua_mod_t *m;
} tlua_jw_t;

static void tlua_jw(lua_State *L, tlua_jw_t *w, const char *s)
{
	if (strlen(w->buf) + strlen(s) + 1 >= w->size)
		luaL_error(L, "page: the description is over %d KB as JSON", TLUA_UI_MAXJSON / 1024);
	dp_strlcat(w->buf, s, w->size);
}

static void tlua_jwstr(lua_State *L, tlua_jw_t *w, const char *s)
{
	char clean[TLUA_UI_MAXSTR * 2];
	char tmp[TLUA_UI_MAXSTR * 6 + 8];
	if (strlen(s) > TLUA_UI_MAXSTR)
		luaL_error(L, "page: a string is over %d bytes", TLUA_UI_MAXSTR);
	TLua_CleanText(s, clean, sizeof(clean), true);
	tmp[0] = 0;
	TLua_JsonAppendString(tmp, sizeof(tmp), clean);
	tlua_jw(L, w, tmp);
}

// the UI bridge's node types (docs/UI_BRIDGE.md, "Mods"); "label" and "color"
// are accepted as the older names of "text" and "colour"; "list" is ours
static const char *const tlua_rowtypes[] = {"section", "card", "text", "divider", "switch", "slider", "stepper", "choice", "field", "colour", "keybind", "button", "list", "image", "progress", "label", "color", NULL};
static const char *tlua_rowtype_name(const char *t)
{
	if (!strcmp(t, "label")) return "text";
	if (!strcmp(t, "color")) return "colour";
	return t;
}

static qbool tlua_rowid_ok(const char *s)
{
	size_t n = strlen(s);
	if (!n || n > 32)
		return false;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_'))
			return false;
	return true;
}

static tlua_setting_t *tlua_setting_of(tlua_mod_t *m, const char *name)
{
	int i;
	for (i = 0; m && i < m->numsettings; i++)
		if (!strcmp(m->settings[i].name, name))
			return &m->settings[i];
	return NULL;
}

static void tlua_setting_cvar(tlua_mod_t *m, const tlua_setting_t *st, char *out, size_t size)
{
	dpsnprintf(out, size, "%s%s_%s", m->realm->cmdprefix, m->name, st->name);
}

// a string field (key) of the table at t, written as "key":"..." when present
static void tlua_jfield_str(lua_State *L, tlua_jw_t *w, int t, const char *key)
{
	if (lua_getfield(L, t, key) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER)
	{
		char k[48];
		dpsnprintf(k, sizeof(k), ",\"%s\":", key);
		tlua_jw(L, w, k);
		tlua_jwstr(L, w, lua_tostring(L, -1));
	}
	lua_pop(L, 1);
}

static void tlua_jfield_num(lua_State *L, tlua_jw_t *w, int t, const char *key)
{
	if (lua_getfield(L, t, key) == LUA_TNUMBER)
	{
		char k[96];
		double d = lua_tonumber(L, -1);
		if (d == d && d < 1e15 && d > -1e15)
		{
			dpsnprintf(k, sizeof(k), ",\"%s\":%.10g", key, d);
			tlua_jw(L, w, k);
		}
	}
	lua_pop(L, 1);
}

static void tlua_jrows(lua_State *L, tlua_jw_t *w, int rows, int depth);

static void tlua_jrow(lua_State *L, tlua_jw_t *w, int r, int depth)
{
	const char *type;
	char setting[TLUA_SUFFIX_LEN + 1] = "";
	tlua_setting_t *st = NULL;
	int i;
	luaL_checkstack(L, 8, "page");
	if (++w->rows > TLUA_UI_MAXROWS)
		luaL_error(L, "page: over %d rows", TLUA_UI_MAXROWS);
	if (lua_getfield(L, r, "type") != LUA_TSTRING)
		luaL_error(L, "page: every row needs a type (section, switch, slider, ...)");
	type = lua_tostring(L, -1);
	for (i = 0; tlua_rowtypes[i]; i++)
		if (!strcmp(type, tlua_rowtypes[i]))
			break;
	if (!tlua_rowtypes[i])
		luaL_error(L, "page: no row type '%s'", type);
	tlua_jw(L, w, "{\"type\":");
	tlua_jwstr(L, w, tlua_rowtype_name(type));
	type = tlua_rowtype_name(type); // a constant now: the stack slot can go
	lua_pop(L, 1);
	if (lua_getfield(L, r, "id") != LUA_TNIL)
	{
		if (lua_type(L, -1) != LUA_TSTRING || !tlua_rowid_ok(lua_tostring(L, -1)))
			luaL_error(L, "page: a row id is a-z 0-9 _, up to 32");
		tlua_jw(L, w, ",\"id\":");
		tlua_jwstr(L, w, lua_tostring(L, -1));
	}
	else if (strcmp(type, "section") && strcmp(type, "text") && strcmp(type, "image") && strcmp(type, "card") && strcmp(type, "divider"))
		luaL_error(L, "page: a %s row needs an id", type);
	lua_pop(L, 1);
	tlua_jfield_str(L, w, r, "label");
	tlua_jfield_str(L, w, r, "text");
	tlua_jfield_str(L, w, r, "title");
	tlua_jfield_str(L, w, r, "desc");
	tlua_jfield_str(L, w, r, "placeholder");
	tlua_jfield_str(L, w, r, "file");
	tlua_jfield_str(L, w, r, "style");
	tlua_jfield_num(L, w, r, "min");
	tlua_jfield_num(L, w, r, "max");
	tlua_jfield_num(L, w, r, "step");
	tlua_jfield_num(L, w, r, "maxlen");
	tlua_jfield_num(L, w, r, "w");
	tlua_jfield_num(L, w, r, "h");
	if (lua_getfield(L, r, "disabled") == LUA_TBOOLEAN && lua_toboolean(L, -1))
		tlua_jw(L, w, ",\"disabled\":true");
	lua_pop(L, 1);
	// a row bound to one of the manifest's settings: its value is the cvar's
	if (lua_getfield(L, r, "setting") == LUA_TSTRING)
	{
		dp_strlcpy(setting, lua_tostring(L, -1), sizeof(setting));
		if (!(st = tlua_setting_of(w->m, setting)))
			luaL_error(L, "page: setting '%s' is not declared in mod.txt", setting);
	}
	lua_pop(L, 1);
	if (st)
	{
		char cv[96];
		const char *val;
		tlua_setting_cvar(w->m, st, cv, sizeof(cv));
		val = Cvar_VariableString(&cvars_all, cv, ~0);
		tlua_jw(L, w, ",\"setting\":");
		tlua_jwstr(L, w, cv);
		tlua_jw(L, w, ",\"value\":");
		if (!strcmp(type, "switch"))
			tlua_jw(L, w, atof(val) != 0 ? "true" : "false");
		else if (!strcmp(type, "slider") || !strcmp(type, "stepper") || !strcmp(type, "progress"))
		{
			char num[64];
			dpsnprintf(num, sizeof(num), "%.10g", atof(val));
			tlua_jw(L, w, num);
		}
		else
			tlua_jwstr(L, w, val);
		if (!strcmp(st->type, "choice") && !strcmp(type, "choice"))
		{
			// the manifest's options, unless the row lists its own
			lua_getfield(L, r, "options");
			if (lua_isnil(L, -1))
			{
				char opts[128], *o;
				qbool first = true;
				dp_strlcpy(opts, st->args, sizeof(opts));
				tlua_jw(L, w, ",\"options\":[");
				for (o = strtok(opts, " \t"); o; o = strtok(NULL, " \t"))
				{
					if (!first)
						tlua_jw(L, w, ",");
					first = false;
					tlua_jwstr(L, w, o);
				}
				tlua_jw(L, w, "]");
			}
			lua_pop(L, 1);
		}
		if ((!strcmp(st->type, "number") || !strcmp(st->type, "int") || !strcmp(st->type, "float")) && !strcmp(type, "slider"))
		{
			float a = 0, b = 1, c = 0;
			char num[128];
			sscanf(st->args, "%f %f %f", &a, &b, &c);
			if (lua_getfield(L, r, "min") == LUA_TNIL)
			{
				dpsnprintf(num, sizeof(num), ",\"min\":%g,\"max\":%g", a, b);
				tlua_jw(L, w, num);
				if (c > 0 || !strcmp(st->type, "int"))
				{
					dpsnprintf(num, sizeof(num), ",\"step\":%g", c > 0 ? c : 1);
					tlua_jw(L, w, num);
				}
			}
			lua_pop(L, 1);
		}
	}
	else if (lua_getfield(L, r, "value") != LUA_TNIL)
	{
		tlua_jw(L, w, ",\"value\":");
		if (lua_type(L, -1) == LUA_TBOOLEAN)
			tlua_jw(L, w, lua_toboolean(L, -1) ? "true" : "false");
		else if (lua_type(L, -1) == LUA_TNUMBER)
		{
			char num[64];
			dpsnprintf(num, sizeof(num), "%.10g", lua_tonumber(L, -1));
			tlua_jw(L, w, num);
		}
		else if (lua_type(L, -1) == LUA_TSTRING)
			tlua_jwstr(L, w, lua_tostring(L, -1));
		else
			luaL_error(L, "page: a value is a boolean, number or string");
		lua_pop(L, 1);
	}
	else
		lua_pop(L, 1);
	// choice options and list items: strings, or {id, label, detail} for a list
	if (lua_getfield(L, r, "options") == LUA_TTABLE)
	{
		lua_Integer n = (lua_Integer)lua_rawlen(L, -1), k;
		if (n > 16)
			luaL_error(L, "page: over 16 options");
		tlua_jw(L, w, ",\"options\":[");
		for (k = 1; k <= n; k++)
		{
			if (k > 1)
				tlua_jw(L, w, ",");
			lua_rawgeti(L, -1, k);
			tlua_jwstr(L, w, luaL_tolstring(L, -1, NULL));
			lua_pop(L, 2);
		}
		tlua_jw(L, w, "]");
	}
	lua_pop(L, 1);
	if (lua_getfield(L, r, "items") == LUA_TTABLE)
	{
		lua_Integer n = (lua_Integer)lua_rawlen(L, -1), k;
		if (n > TLUA_UI_MAXITEMS)
			luaL_error(L, "page: over %d list items", TLUA_UI_MAXITEMS);
		tlua_jw(L, w, ",\"items\":[");
		for (k = 1; k <= n; k++)
		{
			if (k > 1)
				tlua_jw(L, w, ",");
			lua_rawgeti(L, -1, k);
			if (lua_type(L, -1) == LUA_TTABLE)
			{
				int it = lua_gettop(L);
				char kid[16];
				dpsnprintf(kid, sizeof(kid), "%d", (int)k);
				tlua_jw(L, w, "{\"id\":");
				lua_getfield(L, it, "id");
				tlua_jwstr(L, w, lua_isnil(L, -1) ? kid : luaL_tolstring(L, -1, NULL));
				lua_settop(L, it);
				tlua_jfield_str(L, w, it, "label");
				tlua_jfield_str(L, w, it, "detail");
				tlua_jw(L, w, "}");
			}
			else
			{
				char id[16];
				dpsnprintf(id, sizeof(id), "%d", (int)k);
				tlua_jw(L, w, "{\"id\":");
				tlua_jwstr(L, w, id);
				tlua_jw(L, w, ",\"label\":");
				tlua_jwstr(L, w, luaL_tolstring(L, -1, NULL));
				lua_pop(L, 1);
				tlua_jw(L, w, "}");
			}
			lua_pop(L, 1);
		}
		tlua_jw(L, w, "]");
	}
	lua_pop(L, 1);
	if (lua_getfield(L, r, "children") == LUA_TTABLE)
	{
		if (depth + 1 >= TLUA_UI_MAXDEPTH)
			luaL_error(L, "page: nested deeper than %d", TLUA_UI_MAXDEPTH);
		tlua_jw(L, w, ",\"children\":");
		tlua_jrows(L, w, lua_gettop(L), depth + 1);
	}
	lua_pop(L, 1);
	tlua_jw(L, w, "}");
}

static void tlua_jrows(lua_State *L, tlua_jw_t *w, int rows, int depth)
{
	lua_Integer n = (lua_Integer)lua_rawlen(L, rows), k;
	tlua_jw(L, w, "[");
	for (k = 1; k <= n; k++)
	{
		if (k > 1)
			tlua_jw(L, w, ",");
		if (lua_rawgeti(L, rows, k) != LUA_TTABLE)
			luaL_error(L, "page: row %d is not a table", (int)k);
		tlua_jrow(L, w, lua_gettop(L), depth);
		lua_pop(L, 1);
	}
	tlua_jw(L, w, "]");
}

// the page object at idx (its __desc) as JSON into out
static void tlua_page_json(lua_State *L, int obj, tlua_mod_t *m, const char *id, const char *kind, char *out, size_t size)
{
	tlua_jw_t w;
	int d;
	w.buf = out;
	w.size = size;
	w.rows = 0;
	w.m = m;
	out[0] = 0;
	obj = lua_absindex(L, obj);
	lua_getfield(L, obj, "__desc");
	d = lua_gettop(L);
	tlua_jw(L, &w, "{\"schema\":1,\"id\":");
	tlua_jwstr(L, &w, id);
	tlua_jw(L, &w, ",\"origin\":");
	tlua_jwstr(L, &w, m->name);
	tlua_jw(L, &w, ",\"realm\":");
	tlua_jwstr(L, &w, m->realm == &tlua_server ? "server" : "client");
	tlua_jw(L, &w, ",\"kind\":");
	tlua_jwstr(L, &w, kind);
	tlua_jfield_str(L, &w, d, "title");
	tlua_jfield_str(L, &w, d, "desc");
	tlua_jfield_str(L, &w, d, "group");
	tlua_jw(L, &w, ",\"from_server\":");
	tlua_jw(L, &w, !strcmp(kind, "server") ? "true" : "false");
	if (lua_getfield(L, d, "rows") != LUA_TTABLE)
		luaL_error(L, "page: rows = { ... } is required");
	tlua_jw(L, &w, ",\"rows\":");
	tlua_jrows(L, &w, lua_gettop(L), 0);
	lua_pop(L, 2);
	tlua_jw(L, &w, "}");
}

// ---------------------------------------------------------------------------
// Lua API

static int tlua_page_method_publish(lua_State *L);
static int tlua_page_method_set(lua_State *L);
static int tlua_page_method_rows(lua_State *L);
static int tlua_page_method_remove(lua_State *L);
static int tlua_page_method_value(lua_State *L);

static void tlua_page_pushmt(lua_State *L)
{
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_pagemt) == LUA_TTABLE)
		return;
	lua_pop(L, 1);
	lua_newtable(L);
	lua_newtable(L);
	TLua_SetField(L, "publish", tlua_page_method_publish);
	TLua_SetField(L, "set", tlua_page_method_set);
	TLua_SetField(L, "rows", tlua_page_method_rows);
	TLua_SetField(L, "remove", tlua_page_method_remove);
	TLua_SetField(L, "value", tlua_page_method_value);
	lua_setfield(L, -2, "__index");
	lua_pushliteral(L, "page");
	lua_setfield(L, -2, "__metatable");
	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &tlua_key_pagemt);
}

// the dispatcher (Lua): row callbacks, then the page's on_change, then publish
static const char tlua_page_dispatch_src[] =
	"local page, rowid, event, value, player = ...\n"
	"local d = rawget(page, '__desc')\n"
	"local function find(rows)\n"
	"  for _, r in ipairs(rows or {}) do\n"
	"    if r.id == rowid then return r end\n"
	"    local c = find(r.children) if c then return c end\n"
	"  end\n"
	"end\n"
	"local row = find(d.rows)\n"
	"if not row then return end\n"
	"if event == 'change' or event == 'select' then\n"
	"  if row.type == 'switch' then value = (value == '1' or value == 'true')\n"
	"  elseif row.type == 'slider' or row.type == 'stepper' or row.type == 'progress' then value = tonumber(value) or 0 end\n"
	"  if not row.setting then row.value = value end\n"
	"  local f = event == 'select' and (row.on_select or row.on_change) or row.on_change\n"
	"  if f then f(value, player) end\n"
	"elseif event == 'press' then\n"
	"  if row.on_press then row.on_press(player) end\n"
	"end\n"
	"if d.on_change then d.on_change(rowid, value, event, player) end\n"
	"if rawget(page, '__shown') == nil then page:publish() end\n";

static const char *tlua_page_kind(lua_State *L, int obj)
{
	const char *k;
	lua_getfield(L, obj, "__kind");
	k = lua_tostring(L, -1);
	lua_pop(L, 1);
	return k ? k : "menu";
}

static int tlua_page_method_publish(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	static char json[TLUA_UI_MAXJSON];
	const char *id, *kind;
	tlua_uipage_t *p;
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_getfield(L, 1, "id");
	id = luaL_checkstring(L, -1);
	kind = tlua_page_kind(L, 1);
	if (!strcmp(kind, "server"))
		return 0; // xt.page.show sends it again
	tlua_page_json(L, 1, m, id, kind, json, sizeof(json));
	p = tlua_ui_find(id);
	if (p)
		tlua_page_setjson(p, json);
	return 0;
}

// a row by id (recursive) in the table at rows; pushes it or nil
static qbool tlua_findrow(lua_State *L, int rows, const char *id, int depth)
{
	lua_Integer n, k;
	if (lua_type(L, rows) != LUA_TTABLE || depth > TLUA_UI_MAXDEPTH)
		return false;
	n = (lua_Integer)lua_rawlen(L, rows);
	for (k = 1; k <= n; k++)
	{
		int r;
		lua_rawgeti(L, rows, k);
		r = lua_gettop(L);
		if (lua_type(L, r) == LUA_TTABLE)
		{
			lua_getfield(L, r, "id");
			if (lua_type(L, -1) == LUA_TSTRING && !strcmp(lua_tostring(L, -1), id))
			{
				lua_pop(L, 1);
				return true;
			}
			lua_pop(L, 1);
			lua_getfield(L, r, "children");
			if (tlua_findrow(L, lua_gettop(L), id, depth + 1))
			{
				lua_replace(L, r);
				lua_settop(L, r);
				return true;
			}
			lua_settop(L, r);
		}
		lua_pop(L, 1);
	}
	return false;
}

static int tlua_page_method_set(lua_State *L)
{
	const char *rowid = luaL_checkstring(L, 2), *field = luaL_checkstring(L, 3);
	luaL_checktype(L, 1, LUA_TTABLE);
	luaL_checkany(L, 4);
	if (!strcmp(field, "type") || !strcmp(field, "id"))
		return luaL_error(L, "page:set: a row's type and id stay as they are");
	lua_getfield(L, 1, "__desc");
	lua_getfield(L, -1, "rows");
	if (!tlua_findrow(L, lua_gettop(L), rowid, 0))
		return luaL_error(L, "page:set: no row '%s'", rowid);
	lua_pushvalue(L, 4);
	lua_setfield(L, -2, field);
	lua_settop(L, 1);
	return tlua_page_method_publish(L);
}

static int tlua_page_method_rows(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	luaL_checktype(L, 2, LUA_TTABLE);
	lua_getfield(L, 1, "__desc");
	lua_pushvalue(L, 2);
	lua_setfield(L, -2, "rows");
	lua_settop(L, 1);
	return tlua_page_method_publish(L);
}

static int tlua_page_method_value(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	const char *rowid = luaL_checkstring(L, 2);
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_getfield(L, 1, "__desc");
	lua_getfield(L, -1, "rows");
	if (!tlua_findrow(L, lua_gettop(L), rowid, 0))
		return 0;
	if (lua_getfield(L, -1, "setting") == LUA_TSTRING)
	{
		tlua_setting_t *st = tlua_setting_of(m, lua_tostring(L, -1));
		char cv[96];
		if (st)
		{
			tlua_setting_cvar(m, st, cv, sizeof(cv));
			lua_pushstring(L, Cvar_VariableString(&cvars_all, cv, ~0));
			return 1;
		}
	}
	lua_pop(L, 1);
	lua_getfield(L, -1, "value");
	return 1;
}

static void tlua_page_unregister(tlua_mod_t *m, tlua_uipage_t *p)
{
	TLua_Unref(m, p->pref);
	TLua_Unref(m, p->dref);
	tlua_page_free(p);
}

static int tlua_page_method_remove(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	tlua_uipage_t *p;
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_getfield(L, 1, "id");
	p = tlua_ui_find(luaL_checkstring(L, -1));
	if (p && p->realm == m->realm && !strcmp(p->mod, m->name))
		tlua_page_unregister(m, p);
	return 0;
}

// builds the page object { id, __desc, __kind } for a description at 1;
// the engine id is "<mod>.<desc.id>"
static void tlua_page_object(lua_State *L, tlua_mod_t *m, const char *kind, char *id, size_t idsize)
{
	const char *pid;
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_getfield(L, 1, "id");
	pid = lua_tostring(L, -1);
	if (!pid || !tlua_rowid_ok(pid))
		luaL_error(L, "page: id = \"...\" (a-z 0-9 _, up to 32) is required");
	dpsnprintf(id, idsize, "%s.%s", m->name, pid);
	lua_pop(L, 1);
	lua_createtable(L, 0, 4);
	lua_pushstring(L, id);
	lua_setfield(L, -2, "id");
	lua_pushvalue(L, 1);
	lua_setfield(L, -2, "__desc");
	lua_pushstring(L, kind);
	lua_setfield(L, -2, "__kind");
	tlua_page_pushmt(L);
	lua_setmetatable(L, -2);
}

static void tlua_push_dispatcher(lua_State *L)
{
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_dispatch) == LUA_TFUNCTION)
		return;
	lua_pop(L, 1);
	if (luaL_loadbufferx(L, tlua_page_dispatch_src, sizeof(tlua_page_dispatch_src) - 1, "=xt.page", "t") != LUA_OK)
		lua_error(L);
	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &tlua_key_dispatch);
}

static int tlua_page_create(lua_State *L, const char *kind)
{
	tlua_mod_t *m = TLua_Self(L);
	static char json[TLUA_UI_MAXJSON];
	char id[96];
	int i, mine = 0, obj;
	tlua_uipage_t *p;
	tlua_page_object(L, m, kind, id, sizeof(id));
	obj = lua_gettop(L);
	tlua_page_json(L, obj, m, id, kind, json, sizeof(json));
	p = tlua_ui_find(id);
	if (p && (p->realm != m->realm || strcmp(p->mod, m->name)))
		return luaL_error(L, "page: '%s' belongs to someone else", id);
	if (!p)
	{
		for (i = 0; i < TLUA_UI_MAXPAGES; i++)
			if (tlua_pages[i].used && tlua_pages[i].realm == m->realm && !strcmp(tlua_pages[i].mod, m->name) && strcmp(tlua_pages[i].kind, "manifest"))
				mine++;
		if (mine >= TLUA_UI_PERMOD)
			return luaL_error(L, "page: %d pages per mod", TLUA_UI_PERMOD);
		if (!(p = tlua_page_new(id)))
			return luaL_error(L, "page: too many pages");
		p->realm = m->realm;
		dp_strlcpy(p->mod, m->name, sizeof(p->mod));
		dp_strlcpy(p->origin, m->name, sizeof(p->origin));
		dp_strlcpy(p->kind, kind, sizeof(p->kind));
	}
	else
	{
		TLua_Unref(m, p->pref);
		TLua_Unref(m, p->dref);
	}
	lua_getfield(L, 1, "title");
	TLua_CleanText(lua_isstring(L, -1) ? lua_tostring(L, -1) : m->title[0] ? m->title : m->name, p->title, sizeof(p->title), false);
	lua_pop(L, 1);
	p->pref = TLua_Ref(L, obj);
	tlua_push_dispatcher(L);
	p->dref = TLua_Ref(L, -1);
	lua_pop(L, 1);
	tlua_page_setjson(p, json);
	lua_settop(L, obj);
	return 1;
}

int TLuaAPI_settings_page(lua_State *L) { return tlua_page_create(L, "settings"); }
int TLuaAPI_menu_page(lua_State *L) { return tlua_page_create(L, "menu"); }

int TLuaAPI_page_get(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	char id[96];
	tlua_uipage_t *p;
	dpsnprintf(id, sizeof(id), "%s.%s", m->name, luaL_checkstring(L, 1));
	p = tlua_ui_find(id);
	if (!p || p->realm != m->realm || strcmp(p->mod, m->name) || !p->pref)
		return 0;
	lua_rawgeti(L, LUA_REGISTRYINDEX, p->pref);
	return 1;
}

// ---------------------------------------------------------------------------
// Server pages (a server mod's xt.page.show; the transport is touch_lua_chan.c)

#define TLUA_SVPAGES 32
typedef struct tlua_svpage_s { qbool used; tlua_realm_t *realm; char mod[TLUA_NAME_LEN + 1]; char id[96]; int pref, dref; } tlua_svpage_t;
static tlua_svpage_t tlua_svpages[TLUA_SVPAGES];

qbool TLua_Chan_SendPage(int player, const char *pageid, const char *json);
qbool TLua_Chan_ClosePage(int player, const char *pageid);

int TLuaAPI_sv_page_show(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	static char json[TLUA_UI_MAXJSON];
	lua_Integer player = luaL_checkinteger(L, 1);
	char id[96];
	int i, obj;
	tlua_svpage_t *sp = NULL;
	lua_remove(L, 1); // the description is now at 1
	tlua_page_object(L, m, "server", id, sizeof(id));
	obj = lua_gettop(L);
	lua_pushboolean(L, 1);
	lua_setfield(L, obj, "__shown");
	tlua_page_json(L, obj, m, id, "server", json, sizeof(json));
	for (i = 0; i < TLUA_SVPAGES; i++)
		if (tlua_svpages[i].used && !strcmp(tlua_svpages[i].id, id))
			sp = &tlua_svpages[i];
	if (!sp)
		for (i = 0; i < TLUA_SVPAGES; i++)
			if (!tlua_svpages[i].used)
			{
				sp = &tlua_svpages[i];
				break;
			}
	if (!sp)
		return luaL_error(L, "xt.page.show: too many server pages");
	if (sp->used)
	{
		TLua_Unref(m, sp->pref);
		TLua_Unref(m, sp->dref);
	}
	sp->used = true;
	sp->realm = m->realm;
	dp_strlcpy(sp->mod, m->name, sizeof(sp->mod));
	dp_strlcpy(sp->id, id, sizeof(sp->id));
	sp->pref = TLua_Ref(L, obj);
	tlua_push_dispatcher(L);
	sp->dref = TLua_Ref(L, -1);
	lua_pop(L, 1);
	lua_pushboolean(L, TLua_Chan_SendPage((int)player, id, json));
	return 1;
}

int TLuaAPI_sv_page_close(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	char id[96];
	lua_Integer player = luaL_checkinteger(L, 1);
	if (lua_type(L, 2) == LUA_TSTRING)
		dpsnprintf(id, sizeof(id), "%s.%s", m->name, lua_tostring(L, 2));
	else
		dpsnprintf(id, sizeof(id), "%s.", m->name);
	TLua_Chan_ClosePage((int)player, id);
	return 0;
}

// ---------------------------------------------------------------------------
// Events from a renderer

typedef struct tlua_uiev_s { int pref; const char *row, *event, *value; int player; } tlua_uiev_t;
static int tlua_uiev_push(lua_State *L, void *ud)
{
	tlua_uiev_t *e = (tlua_uiev_t *)ud;
	lua_rawgeti(L, LUA_REGISTRYINDEX, e->pref);
	lua_pushstring(L, e->row);
	lua_pushstring(L, e->event);
	lua_pushstring(L, e->value);
	if (e->player)
		lua_pushinteger(L, e->player);
	else
		lua_pushnil(L);
	return 5;
}

// a value for a manifest setting, checked against its declaration
static qbool tlua_setting_value(const tlua_setting_t *st, const char *in, char *out, size_t size)
{
	if (!TLua_CleanValue(in))
		return false;
	if (!strcmp(st->type, "bool"))
		dp_strlcpy(out, (!strcmp(in, "1") || !strcmp(in, "true")) ? "1" : "0", size);
	else if (!strcmp(st->type, "number") || !strcmp(st->type, "int") || !strcmp(st->type, "float"))
	{
		float a = 0, b = 0, c = 0, v = (float)atof(in);
		int n = sscanf(st->args, "%f %f %f", &a, &b, &c);
		if (n >= 2)
			v = bound(a, v, b);
		if (n == 3 && c > 0)
			v = a + (float)floor((v - a) / c + 0.5f) * c;
		if (!strcmp(st->type, "int"))
			v = (float)floor(v + 0.5f);
		dpsnprintf(out, size, "%g", v);
	}
	else if (!strcmp(st->type, "choice"))
	{
		char opts[128], *o;
		dp_strlcpy(opts, st->args, sizeof(opts));
		for (o = strtok(opts, " \t"); o; o = strtok(NULL, " \t"))
			if (!strcmp(o, in))
				break;
		if (!o)
			return false;
		dp_strlcpy(out, in, size);
	}
	else if (!strcmp(st->type, "color"))
	{
		if (strlen(in) != 6 || strspn(in, "0123456789abcdefABCDEF") != 6)
			return false;
		dp_strlcpy(out, in, size);
	}
	else if (!strcmp(st->type, "key"))
	{
		if (Key_StringToKeynum(in) < 0)
			return false;
		dp_strlcpy(out, in, size);
	}
	else
		dp_strlcpy(out, in, size);
	return true;
}

// The one entry point for renderers (and lua_page_event): a value changed
// ("change"), a button was pressed ("press"), a list item chosen ("select").
qbool tlua_ui_event(const char *id, const char *row, const char *event, const char *value);
qbool tlua_ui_event(const char *id, const char *row, const char *event, const char *value)
{
	tlua_uipage_t *p = tlua_ui_find(id);
	tlua_mod_t *m;
	tlua_uiev_t e;
	if (!p || !tlua_rowid_ok(row) || (strcmp(event, "change") && strcmp(event, "press") && strcmp(event, "select")))
		return false;
	if (!p->realm)
	{
		// a page a server sent: the answer goes back to it (rate limited there)
		qbool TLua_Chan_PageReply(const char *pageid, const char *row, const char *event, const char *value);
		return TLua_Chan_PageReply(id + 7, row, event, value); // "server." prefix off
	}
	m = TLua_Find(p->realm, p->mod);
	if (!m)
		return false;
	if (!strcmp(p->kind, "manifest") || (p->pref && m->L))
	{
		// a row bound to a setting: the cvar first, checked against its declaration
		const char *json = p->json ? p->json : "";
		char key[64];
		const char *hit;
		dpsnprintf(key, sizeof(key), "\"id\":\"%s\"", row);
		hit = strstr(json, key);
		if (hit && !strcmp(event, "change"))
		{
			const char *sp = strstr(hit, "\"setting\":\"");
			const char *end = strchr(hit, '}');
			if (sp && (!end || sp < end))
			{
				char cv[96], val[256];
				size_t n = 0;
				const tlua_setting_t *st = NULL;
				int i;
				sp += 11;
				while (*sp && *sp != '"' && n + 1 < sizeof(cv))
					cv[n++] = *sp++;
				cv[n] = 0;
				for (i = 0; i < m->numsettings; i++)
				{
					char want[96];
					tlua_setting_cvar(m, &m->settings[i], want, sizeof(want));
					if (!strcmp(want, cv))
						st = &m->settings[i];
				}
				if (!st || !tlua_setting_value(st, value, val, sizeof(val)))
					return false;
				Cvar_Set(&cvars_all, cv, val);
			}
		}
	}
	if (!strcmp(p->kind, "manifest"))
	{
		TLua_UI_SyncManifest(p->realm);
		return true;
	}
	if (!p->pref || !m->L || m->state != TLUA_RUNNING)
		return false;
	e.pref = p->pref;
	e.row = row;
	e.event = event;
	e.value = value;
	e.player = 0;
	return TLua_CallRef(m, p->dref, tlua_uiev_push, &e, NULL);
}

// the server: a player's answer to a page a server mod showed (touch_lua_chan.c)
void TLua_UI_ServerReply(int player, const char *id, const char *row, const char *event, const char *value);
void TLua_UI_ServerReply(int player, const char *id, const char *row, const char *event, const char *value)
{
	int i;
	for (i = 0; i < TLUA_SVPAGES; i++)
	{
		tlua_svpage_t *sp = &tlua_svpages[i];
		tlua_mod_t *m;
		tlua_uiev_t e;
		if (!sp->used || strcmp(sp->id, id))
			continue;
		m = TLua_Find(sp->realm, sp->mod);
		if (!m || m->state != TLUA_RUNNING || !tlua_rowid_ok(row))
			return;
		e.pref = sp->pref;
		e.row = row;
		e.event = event;
		e.value = value;
		e.player = player;
		TLua_CallRef(m, sp->dref, tlua_uiev_push, &e, NULL);
		return;
	}
}

// the client: a page a server sent (touch_lua_chan.c)
void TLua_UI_ServerPage(const char *pageid, const char *json, const char *servermod);
void TLua_UI_ServerPage(const char *pageid, const char *json, const char *servermod)
{
	char id[110];
	tlua_uipage_t *p;
	dpsnprintf(id, sizeof(id), "server.%s", pageid);
	p = tlua_page_new(id);
	if (!p)
		return;
	p->realm = NULL;
	dpsnprintf(p->origin, sizeof(p->origin), "server:%s", servermod);
	dp_strlcpy(p->kind, "server", sizeof(p->kind));
	dp_strlcpy(p->title, "Page from the server", sizeof(p->title));
	tlua_page_setjson(p, json);
}

void TLua_UI_ServerPageClose(const char *prefix);
void TLua_UI_ServerPageClose(const char *prefix)
{
	int i;
	char id[110];
	dpsnprintf(id, sizeof(id), "server.%s", prefix);
	for (i = 0; i < TLUA_UI_MAXPAGES; i++)
		if (tlua_pages[i].used && !tlua_pages[i].realm && !strncmp(tlua_pages[i].id, id, strlen(id)))
			tlua_page_free(&tlua_pages[i]);
}

// ---------------------------------------------------------------------------
// Settings pages from the manifest (setting.<name>), with no code

void TLua_UI_SyncManifest(tlua_realm_t *r)
{
	static char json[TLUA_UI_MAXJSON];
	int i, j, k;
	if (cls.state == ca_dedicated && r == &tlua_client)
		return;
	// pages of mods that went away or lost their settings
	for (k = 0; k < TLUA_UI_MAXPAGES; k++)
	{
		tlua_uipage_t *p = &tlua_pages[k];
		tlua_mod_t *m;
		if (!p->used || p->realm != r || strcmp(p->kind, "manifest"))
			continue;
		m = TLua_Find(r, p->mod);
		if (!m || !m->numsettings || m->manifest_error)
			tlua_page_free(p);
	}
	for (i = 0; i < r->nummods; i++)
	{
		tlua_mod_t *m = &r->mods[i];
		char id[96], line[512], group[32] = "";
		tlua_uipage_t *p;
		if (!m->numsettings || m->manifest_error || m->reserved)
			continue;
		dpsnprintf(id, sizeof(id), "%s.settings", m->name);
		p = tlua_ui_find(id);
		if (p && strcmp(p->kind, "manifest"))
			continue; // the mod registered its own page with that id
		if (!p && !(p = tlua_page_new(id)))
			continue;
		p->realm = r;
		dp_strlcpy(p->mod, m->name, sizeof(p->mod));
		dp_strlcpy(p->origin, m->name, sizeof(p->origin));
		dp_strlcpy(p->kind, "manifest", sizeof(p->kind));
		dp_strlcpy(p->title, m->title[0] ? m->title : m->name, sizeof(p->title));
		json[0] = 0;
		dp_strlcat(json, "{\"schema\":1,\"id\":", sizeof(json));
		TLua_JsonAppendString(json, sizeof(json), id);
		dp_strlcat(json, ",\"origin\":", sizeof(json));
		TLua_JsonAppendString(json, sizeof(json), m->name);
		dp_strlcat(json, r == &tlua_server ? ",\"realm\":\"server\"" : ",\"realm\":\"client\"", sizeof(json));
		dp_strlcat(json, ",\"kind\":\"settings\",\"title\":", sizeof(json));
		TLua_JsonAppendString(json, sizeof(json), p->title);
		dp_strlcat(json, ",\"from_server\":false,\"rows\":[", sizeof(json));
		for (j = 0; j < m->numsettings; j++)
		{
			tlua_setting_t *st = &m->settings[j];
			char cv[96];
			const char *val, *type;
			tlua_setting_cvar(m, st, cv, sizeof(cv));
			val = Cvar_VariableString(&cvars_all, cv, ~0);
			if (st->group[0] && strcmp(st->group, group))
			{
				dp_strlcpy(group, st->group, sizeof(group));
				dp_strlcat(json, "{\"type\":\"section\",\"text\":", sizeof(json));
				TLua_JsonAppendString(json, sizeof(json), group);
				dp_strlcat(json, "},", sizeof(json));
			}
			type = !strcmp(st->type, "bool") ? "switch" : (!strcmp(st->type, "number") || !strcmp(st->type, "int") || !strcmp(st->type, "float")) ? "slider"
				: !strcmp(st->type, "choice") ? "choice" : !strcmp(st->type, "color") ? "colour" : !strcmp(st->type, "key") ? "keybind" : "field";
			dpsnprintf(line, sizeof(line), "{\"type\":\"%s\",\"id\":\"%s\",\"setting\":", type, st->name);
			dp_strlcat(json, line, sizeof(json));
			TLua_JsonAppendString(json, sizeof(json), cv);
			dp_strlcat(json, ",\"label\":", sizeof(json));
			TLua_JsonAppendString(json, sizeof(json), st->label);
			if (st->desc[0])
			{
				dp_strlcat(json, ",\"desc\":", sizeof(json));
				TLua_JsonAppendString(json, sizeof(json), st->desc);
			}
			dp_strlcat(json, ",\"value\":", sizeof(json));
			if (!strcmp(type, "switch"))
				dp_strlcat(json, atof(val) != 0 ? "true" : "false", sizeof(json));
			else if (!strcmp(type, "slider"))
			{
				float a = 0, b = 1, c = 0;
				int n = sscanf(st->args, "%f %f %f", &a, &b, &c);
				dpsnprintf(line, sizeof(line), "%g,\"min\":%g,\"max\":%g", atof(val), a, b);
				dp_strlcat(json, line, sizeof(json));
				if ((n == 3 && c > 0) || !strcmp(st->type, "int"))
				{
					dpsnprintf(line, sizeof(line), ",\"step\":%g", n == 3 && c > 0 ? c : 1);
					dp_strlcat(json, line, sizeof(json));
				}
			}
			else
				TLua_JsonAppendString(json, sizeof(json), val);
			if (!strcmp(type, "choice"))
			{
				char opts[128], *o;
				qbool first = true;
				dp_strlcpy(opts, st->args, sizeof(opts));
				dp_strlcat(json, ",\"options\":[", sizeof(json));
				for (o = strtok(opts, " \t"); o; o = strtok(NULL, " \t"))
				{
					if (!first)
						dp_strlcat(json, ",", sizeof(json));
					first = false;
					TLua_JsonAppendString(json, sizeof(json), o);
				}
				dp_strlcat(json, "]", sizeof(json));
			}
			dp_strlcat(json, j + 1 < m->numsettings ? "}," : "}", sizeof(json));
		}
		dp_strlcat(json, "]}", sizeof(json));
		tlua_page_setjson(p, json);
	}
}

// ---------------------------------------------------------------------------
// Commands (the stub backend's view, and the renderers' way in)

static void tlua_pages_f(cmd_state_t *cmd)
{
	int i, n = 0;
	(void)cmd;
	for (i = 0; i < TLUA_UI_MAXPAGES; i++)
		if (tlua_pages[i].used)
		{
			n++;
			Con_Printf("  %-32s %-9s %-18s %s\n", tlua_pages[i].id, tlua_pages[i].kind, tlua_pages[i].origin, tlua_pages[i].title);
		}
	Con_Printf("%d mod page(s); lua_page_json <id> prints one\n", n);
}

static void tlua_page_json_f(cmd_state_t *cmd)
{
	tlua_uipage_t *p = Cmd_Argc(cmd) >= 2 ? tlua_ui_find(Cmd_Argv(cmd, 1)) : NULL;
	if (!p)
	{
		Con_Printf("usage: lua_page_json <id> (lua_pages lists them)\n");
		return;
	}
	Con_Printf("%s\n", p->json ? p->json : "");
}

static void tlua_page_event_f(cmd_state_t *cmd)
{
	if (Cmd_Argc(cmd) < 4)
	{
		Con_Printf("usage: lua_page_event <id> <row> change|press|select [value]\n");
		return;
	}
	if (TLua_RemoteRefused(cmd))
		return;
	if (!tlua_ui_event(Cmd_Argv(cmd, 1), Cmd_Argv(cmd, 2), Cmd_Argv(cmd, 3), Cmd_Argc(cmd) >= 5 ? Cmd_Argv(cmd, 4) : ""))
		Con_Printf("lua_page_event: no such page or row, or the value was refused\n");
}

void TLua_UI_Frame(void);
void TLua_UI_Frame(void)
{
	static char lastsel[96];
	if (tlua_pages_dirty || strcmp(lastsel, lua_page_select.string))
	{
		dp_strlcpy(lastsel, lua_page_select.string, sizeof(lastsel));
		tlua_ui_publish();
	}
}

void TLua_UI_ModStopped(tlua_mod_t *m)
{
	int i;
	for (i = 0; i < TLUA_UI_MAXPAGES; i++)
	{
		tlua_uipage_t *p = &tlua_pages[i];
		if (p->used && p->realm == m->realm && !strcmp(p->mod, m->name) && strcmp(p->kind, "manifest"))
			tlua_page_unregister(m, p);
	}
	for (i = 0; i < TLUA_SVPAGES; i++)
		if (tlua_svpages[i].used && tlua_svpages[i].realm == m->realm && !strcmp(tlua_svpages[i].mod, m->name))
			tlua_svpages[i].used = false;
}

void TLua_UI_Setup(void)
{
	if (cls.state != ca_dedicated)
	{
		TLua_RegisterCvar(&lua_pages);
		TLua_RegisterCvar(&lua_page_select);
		TLua_RegisterCvar(&lua_page);
	}
#ifdef TLUA_HAVE_UIB
	TouchUIB_ModSetEventHandler(tlua_uib_event, NULL);
#endif
	Cmd_AddCommand(CF_CLIENT | CF_SERVER, "lua_pages", tlua_pages_f, "list the pages Lua mods describe (settings, menu and server pages)");
	Cmd_AddCommand(CF_CLIENT | CF_SERVER, "lua_page_json", tlua_page_json_f, "print a mod page's description as JSON (docs/LUA_API.md, UI descriptions)");
	Cmd_AddCommand(CF_CLIENT | CF_SERVER, "lua_page_event", tlua_page_event_f, "send a page event as a renderer would: lua_page_event <id> <row> change|press|select [value]");
}
