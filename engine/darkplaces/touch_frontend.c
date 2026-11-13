/*
Godnotic: the engine side of the native front-end bridge (protocol 1).

A native interface -- the Kotlin views XonoticActivity puts over the SDL
surface on Android -- draws the screens outside a match (Home, the server
browser, Settings, Mods, the pause menu) and drives the engine from here.
docs/FRONTEND_BRIDGE.md is the protocol; in short:

  out  JSON snapshots, one per topic (app, connect, cvars, servers, lan,
       maps, gametypes, mods, player, preview) and events, handed to a sink.
       On Android the sink is the resident JNI bridge (libnativeui.so,
       android/app/src/main/cpp/nativeui/), which keeps the newest snapshot
       per topic and tells the UI thread something changed. A topic is only
       sent when its JSON changed, and only the cheap ones are looked at
       every frame: app and, while a join runs, connect. cvars, servers and
       lan are looked at four times a second while a native screen shows,
       not at all during play.
  in   `fe <verb> <args>` console commands through the bridge's command
       queue, which only this thread drains (NativeUI_Frame). Every
       argument is percent-encoded by the UI, so a name or a password with
       spaces, quotes or semicolons arrives as one token and never as
       console syntax; "~" is the empty string.

touch_native_frontend 1 (XonoticActivity passes it when the bridge is new
enough) lets an attached UI own the menu: the QuakeC menu VM stays loaded,
keeps its key_dest bookkeeping (so a local game pauses under the native pause
menu exactly as under the QuakeC one) and runs its toggles, but is neither
drawn nor given keys (TouchFE_MenuHidden, hooked in menu.c). It is in charge
again on its own when the UI has not attached, detaches, or stops beating for
TOUCHFE_BEAT_TIMEOUT seconds: an old host, a crashed or frozen UI layer, or a
desktop without one never leave the player without a menu. `fe menu
qcmenu` hands the menu back on purpose ("Advanced..." in native Settings).

Everything works on the desktop too, without a sink, for headless tests
(scripts/test-frontend.sh): fe_dump prints a topic, fe_take what changed, and
`fe beat` stands in for the UI's heartbeat.
*/

#include "quakedef.h"
#include "cl_screen.h"
#include "libcurl.h"
#include "touch_frontend.h"
#include "touch_hud.h"
#include "touch_lan.h"

extern uint8_t serverlist_querystage; // netconn.c

/*
The UI bridge (touch_uib*.c, docs/UI_BRIDGE.md) when this engine has it: a
weak link, so this file builds and works with and without it. With it, an
attached native UI is a UI bridge renderer: `fe ui <json>` posts its
messages, and the bridge's messages go out on the ordered "ui" queue.
*/
#if defined(__GNUC__) || defined(__clang__)
#define TOUCHFE_WEAK __attribute__((weak))
#else
#define TOUCHFE_WEAK
#endif
typedef struct touchuib_conn_s touchuib_conn_t;
int TouchUIB_Available(void) TOUCHFE_WEAK;
touchuib_conn_t *TouchUIB_Open(const char *name, size_t name_len) TOUCHFE_WEAK;
int TouchUIB_Post(touchuib_conn_t *c, const char *json, size_t len) TOUCHFE_WEAK;
char *TouchUIB_Take(touchuib_conn_t *c, size_t *len) TOUCHFE_WEAK;
void TouchUIB_Free(char *buf) TOUCHFE_WEAK;
void TouchUIB_SetNotify(touchuib_conn_t *c, void (*fn)(void *ctx), void *ctx) TOUCHFE_WEAK;
void TouchUIB_Close(touchuib_conn_t *c) TOUCHFE_WEAK;

#define TOUCHFE_BEAT_TIMEOUT 4.0
#define TOUCHFE_SLOW_PERIOD 0.25
#define TOUCHFE_MAX_WATCH 192
#define TOUCHFE_MAX_SERVERS 512
#define TOUCHFE_MAX_MAPS 256

static cvar_t touch_native_frontend = {CF_CLIENT, "touch_native_frontend", "0",
	"1: a native interface (the Android app's Kotlin screens) may own the menu once it attaches through the front-end bridge; the QuakeC menu takes over again by itself when it is not attached or stops answering (touch_frontend.c)"};
static cvar_t fe_echo = {CF_CLIENT, "fe_echo", "0",
	"print every front-end bridge snapshot as it changes (FE> topic json), for tests"};
static cvar_t fe_state = {CF_CLIENT | CF_READONLY, "_fe_state", "0",
	"front-end bridge: 0 not attached, 1 attached and owning the menu, 2 attached but the QuakeC menu is in charge (Advanced, or no heartbeat)"};

typedef enum { FE_APP, FE_CONNECT, FE_CVARS, FE_SERVERS, FE_LAN, FE_MAPS, FE_GAMETYPES, FE_MODS, FE_PLAYER, FE_PREVIEW, FE_TOPICS } fe_topic_t;
static const char *fe_topic_names[FE_TOPICS] = {"app", "connect", "cvars", "servers", "lan", "maps", "gametypes", "mods", "player", "preview"};

static struct
{
	touchfe_sink_t sink;
	qbool have_sink;
	qbool attached;
	int ui_protocol;
	double beat;          // Sys_DirtyTime of `fe beat` (desktop, tests)
	qbool advanced;       // the QuakeC menu was handed back on purpose
	char screen[32];      // what the UI shows ("" or "none": nothing)
	int cover;            // 0 see-through, 1 dimmed, 2 opaque
	qbool sub[FE_TOPICS];
	unsigned hash[FE_TOPICS];
	char *last[FE_TOPICS];  // newest JSON per topic (fe_dump, fe_take)
	qbool fresh[FE_TOPICS]; // changed since the last fe_take
	double slow_next;
	char status_seen[MAX_QPATH];
	double refresh_started;
	int watch_count;
	char *watch[TOUCHFE_MAX_WATCH];
	// a load in progress (TouchFE_Loading), for the connect topic
	char load_msg[64];
	float load_fraction;
	double load_published;
	char events[4096];      // pending events (desktop fe_take)
	struct { char verb[16]; touchfe_verb_fn fn; } verbs[8];
	int nverbs;
	qbool was_joining;
	touchuib_conn_t *uib;   // the UI bridge connection of the attached native UI
} fe;

// ---------------------------------------------------------------------------
// JSON

typedef struct { char *s; size_t n, cap; } jb_t;

static void jb_grow(jb_t *b, size_t more)
{
	if (b->n + more + 1 <= b->cap)
		return;
	b->cap = (b->n + more + 1) * 2 + 256;
	b->s = (char *)realloc(b->s, b->cap);
}
static void jb_raw(jb_t *b, const char *s)
{
	size_t l = strlen(s);
	jb_grow(b, l);
	memcpy(b->s + b->n, s, l);
	b->n += l;
	b->s[b->n] = 0;
}
static void jb_printf(jb_t *b, const char *fmt, ...) DP_FUNC_PRINTF(2);
static void jb_printf(jb_t *b, const char *fmt, ...)
{
	char tmp[256];
	va_list args;
	va_start(args, fmt);
	dpvsnprintf(tmp, sizeof(tmp), fmt, args);
	va_end(args);
	jb_raw(b, tmp);
}
// A string with JSON escaping; bytes below 0x20 are dropped except \t and \n.
static void jb_str(jb_t *b, const char *s)
{
	jb_grow(b, strlen(s ? s : "") * 2 + 2);
	b->s[b->n++] = '"';
	for (; s && *s; s++)
	{
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\')
		{
			jb_grow(b, 2);
			b->s[b->n++] = '\\';
			b->s[b->n++] = c;
		}
		else if (c == '\n' || c == '\t')
		{
			jb_grow(b, 2);
			b->s[b->n++] = '\\';
			b->s[b->n++] = c == '\n' ? 'n' : 't';
		}
		else if (c >= 0x20)
		{
			jb_grow(b, 1);
			b->s[b->n++] = c;
		}
	}
	jb_grow(b, 1);
	b->s[b->n++] = '"';
	b->s[b->n] = 0;
}
static void jb_key(jb_t *b, const char *key)
{
	if (b->n && b->s[b->n - 1] != '{' && b->s[b->n - 1] != '[')
		jb_raw(b, ",");
	jb_str(b, key);
	jb_raw(b, ":");
}
static void jb_kstr(jb_t *b, const char *key, const char *v) { jb_key(b, key); jb_str(b, v); }
static void jb_kint(jb_t *b, const char *key, int v) { jb_key(b, key); jb_printf(b, "%d", v); }
static void jb_kbool(jb_t *b, const char *key, qbool v) { jb_key(b, key); jb_raw(b, v ? "true" : "false"); }
static void jb_knum(jb_t *b, const char *key, double v) { jb_key(b, key); jb_printf(b, "%.3f", v); }
static void jb_comma(jb_t *b)
{
	if (b->n && b->s[b->n - 1] != '{' && b->s[b->n - 1] != '[')
		jb_raw(b, ",");
}

// Colour codes (^1, ^xRGB) out of a name for a native label.
static void strip_colors(char *out, size_t size, const char *in)
{
	size_t n = 0;
	while (*in && n + 1 < size)
	{
		if (in[0] == '^' && in[1] >= '0' && in[1] <= '9') { in += 2; continue; }
		if (in[0] == '^' && in[1] == 'x' && in[2] && in[3] && in[4]) { in += 5; continue; }
		if (in[0] == '^' && in[1] == '^') { out[n++] = '^'; in += 2; continue; }
		out[n++] = *in++;
	}
	out[n] = 0;
}

static unsigned fe_hash(const char *s)
{
	unsigned h = 2166136261u;
	for (; *s; s++)
		h = (h ^ (unsigned char)*s) * 16777619u;
	return h;
}

// ---------------------------------------------------------------------------
// Out

static void fe_send(fe_topic_t t, jb_t *b, qbool force)
{
	unsigned h = fe_hash(b->s ? b->s : "");
	if (!force && fe.last[t] && h == fe.hash[t] && !strcmp(fe.last[t], b->s))
	{
		free(b->s);
		return;
	}
	fe.hash[t] = h;
	free(fe.last[t]);
	fe.last[t] = b->s; // kept: fe_dump, and the change check
	fe.fresh[t] = true;
	if (fe.have_sink && fe.sink.put)
		fe.sink.put(fe_topic_names[t], fe.last[t], 0);
	if (fe_echo.integer)
		Con_Printf("FE> %s %s\n", fe_topic_names[t], fe.last[t]);
}

static void fe_event(const char *type, const char *k1, const char *v1, const char *k2, const char *v2)
{
	jb_t b = {0};
	jb_raw(&b, "{");
	jb_kstr(&b, "type", type);
	if (k1) jb_kstr(&b, k1, v1);
	if (k2) jb_kstr(&b, k2, v2);
	jb_raw(&b, "}");
	if (fe.have_sink && fe.sink.put)
		fe.sink.put("event", b.s, 1);
	if (fe_echo.integer)
		Con_Printf("FE> event %s\n", b.s);
	if (strlen(fe.events) + strlen(b.s) + 2 < sizeof(fe.events))
	{
		dp_strlcat(fe.events, b.s, sizeof(fe.events));
		dp_strlcat(fe.events, "\n", sizeof(fe.events));
	}
	free(b.s);
}

static double beat_age(void)
{
	double age = fe.beat > 0 ? Sys_DirtyTime() - fe.beat : 1e9;
	if (fe.have_sink && fe.sink.beat_age_ms)
	{
		long long ms = fe.sink.beat_age_ms();
		if (ms >= 0 && ms / 1000.0 < age)
			age = ms / 1000.0;
	}
	return age;
}

// Attached, beating and not handed back: the native UI owns the menu.
static qbool fe_owns_menu(void)
{
	return touch_native_frontend.integer && fe.attached && !fe.advanced && beat_age() < TOUCHFE_BEAT_TIMEOUT;
}

static void fe_update_state(void)
{
	int st = !fe.attached ? 0 : fe_owns_menu() ? 1 : 2;
	if (fe_state.integer != st)
	{
		// The touch HUD's MENU (and the pause on leaving the app) open the
		// QuakeC game menu dialog (commands.cfg: menu_showgamemenudialog ->
		// menu_cmd directmenu GameMenu), which only shows itself from m_draw,
		// which the hidden menu does not run. While the native UI owns the
		// menu the same alias opens the (hidden) menu plainly, which the
		// native pause menu stands for; it is put back when it does not.
		if (st == 1)
			Cbuf_AddText(cmd_local, "\nalias menu_showgamemenudialog \"togglemenu 1\"\n");
		else if (fe_state.integer == 1)
			Cbuf_AddText(cmd_local, "\nalias menu_showgamemenudialog \"menu_cmd directmenu GameMenu\"\n");
		Cvar_SetValueQuick(&fe_state, st);
	}
}

static void fe_switch_changed(cvar_t *var)
{
	(void)var;
	fe_update_state();
}

qbool TouchFE_MenuHidden(void)
{
	return fe_owns_menu();
}

static qbool in_menu(void)
{
	return key_dest == key_menu || key_dest == key_menu_grabbed;
}

static const char *app_state(void)
{
	if (cls.state == ca_dedicated)
		return "dedicated";
	if (cls.connect_trying || (cls.state == ca_connected && cls.signon < SIGNONS))
		return scr_loading ? "loading" : "connecting";
	if (cls.state == ca_connected)
		return in_menu() ? "paused" : "ingame";
	return "menu";
}

static qbool uib_present(void)
{
	return TouchUIB_Available && TouchUIB_Open && TouchUIB_Post && TouchUIB_Take && TouchUIB_Free && TouchUIB_Available();
}

// The bridge queued messages for the native UI: on the "ui" queue, in order.
static void uib_drain(void *ctx)
{
	char *m;
	size_t len;
	(void)ctx;
	while (fe.uib && (m = TouchUIB_Take(fe.uib, &len)))
	{
		TouchFE_Put("ui", m, 1);
		TouchUIB_Free(m);
	}
}

static void uib_close(void)
{
	if (fe.uib && TouchUIB_Close)
		TouchUIB_Close(fe.uib);
	fe.uib = NULL;
}

static touchfe_verb_fn fe_verb(const char *verb)
{
	int i;
	for (i = 0; i < fe.nverbs; i++)
		if (!strcmp(fe.verbs[i].verb, verb))
			return fe.verbs[i].fn;
	return NULL;
}

static void publish_app(void)
{
	jb_t b = {0};
	char addr[128] = "";
	if (cls.state == ca_connected && cls.netcon)
		LHNETADDRESS_ToString(&cls.netcon->peeraddress, addr, sizeof(addr), true);
	jb_raw(&b, "{");
	jb_kint(&b, "proto", TOUCHFE_PROTOCOL);
	jb_kbool(&b, "enabled", touch_native_frontend.integer != 0);
	jb_kbool(&b, "attached", fe.attached);
	jb_kbool(&b, "owns_menu", fe_owns_menu());
	jb_kbool(&b, "advanced", fe.advanced);
	jb_kstr(&b, "state", app_state());
	jb_kbool(&b, "menu", in_menu());
	jb_kbool(&b, "console", (key_consoleactive & KEY_CONSOLEACTIVE_USER) != 0);
	jb_kbool(&b, "local", sv.active);
	jb_kbool(&b, "paused", cl.paused);
	jb_kbool(&b, "hosting", TouchLAN_HostingOnPurpose());
	jb_kint(&b, "maxclients", sv.active ? svs.maxclients : 0);
	jb_kstr(&b, "map", cls.state == ca_connected ? cl.worldbasename : "");
	jb_kstr(&b, "server", sv.active ? "" : addr);
	jb_kbool(&b, "demo", cls.demoplayback);
	jb_kstr(&b, "version", engineversion);
	jb_kbool(&b, "ui_bridge", fe_verb("ui") != NULL || uib_present());
	jb_raw(&b, "}");
	fe_send(FE_APP, &b, false);
}

// cl_connect_status turned into events: a password prompt, a refusal, a drop.
static void watch_status(void)
{
	char addr[128];
	const char *s = cl_connect_status;
	if (!strcmp(s, fe.status_seen))
		return;
	dp_strlcpy(fe.status_seen, s, sizeof(fe.status_seen));
	LHNETADDRESS_ToString(&cls.connect_address, addr, sizeof(addr), true);
	if (!strcmp(s, "Connect: wrong password"))
		fe_event("password", "kind", "wrong", "address", addr);
	else if (!strcmp(s, "Connect: this game needs a password"))
		fe_event("password", "kind", "required", "address", addr);
	else if (!strncmp(s, "Connect: rejected, ", 19))
		fe_event("rejected", "reason", s + 19, "address", addr);
	else if (!strncmp(s, "Connect: failed", 15) || !strcmp(s, "No network"))
		fe_event("failed", "reason", s, "address", addr);
	else if (!strncmp(s, "Disconnect: ", 12))
		fe_event("disconnected", "reason", s + 12, NULL, NULL);
}

static void publish_connect(qbool force)
{
	jb_t b = {0};
	char addr[128] = "";
	int n = 0, i;
	double bytes = 0, speed = 0, progress = 0;
	Curl_downloadinfo_t *dl;
	const char *stage = "idle";
	if (cls.connect_trying)
		LHNETADDRESS_ToString(&cls.connect_address, addr, sizeof(addr), true);
	else if (cls.state == ca_connected && cls.netcon)
		LHNETADDRESS_ToString(&cls.netcon->peeraddress, addr, sizeof(addr), true);
	dl = Curl_GetDownloadInfo(&n, NULL, NULL, 0);
	for (i = 0; i < n; i++)
	{
		if (dl[i].queued)
			continue;
		progress = dl[i].progress;
		speed += dl[i].speed;
	}
	if (cls.connect_trying)
		stage = "connecting";
	else if (cls.state == ca_connected && cls.signon < SIGNONS)
		stage = n > 0 ? "downloading" : (scr_loading ? "loading" : "signon");
	else if (scr_loading)
		stage = "loading";
	(void)bytes;
	jb_raw(&b, "{");
	jb_kstr(&b, "stage", stage);
	jb_kstr(&b, "status", cl_connect_status);
	jb_kstr(&b, "address", addr);
	jb_kint(&b, "signon", cls.signon);
	jb_kint(&b, "signons", SIGNONS);
	jb_kint(&b, "downloads", n);
	if (n > 0)
	{
		jb_kstr(&b, "file", dl[0].filename);
		jb_knum(&b, "download_progress", progress);
		jb_knum(&b, "speed", speed); // bytes per second
	}
	jb_kstr(&b, "load", scr_loading ? fe.load_msg : "");
	jb_knum(&b, "load_progress", scr_loading ? fe.load_fraction : 0);
	jb_kbool(&b, "local", sv.active);
	jb_raw(&b, "}");
	if (dl)
		Z_Free(dl);
	fe_send(FE_CONNECT, &b, force);
}

// A cvar the UI may read and change: not private, and in the schema.
static qbool cvar_allowed(const char *name, qbool write)
{
	static const char *prefixes[] = {
		"cl_touch_", "touch_", "_touch_", "hud_", "crosshair", "sensitivity", "m_", "joy", "fov",
		"vid_", "r_", "gl_", "cl_maxfps", "cl_maxidlefps", "cl_particles", "cl_decals", "snd_",
		"volume", "bgmvolume", "mastervolume", "_cl_name", "_cl_color", "_cl_playermodel",
		"_cl_playerskin", "cl_autoswitch", "cl_movement", "cl_bob", "cl_zoom", "showfps", "scr_",
		"menu_", "net_slist_", "_lua_", "_cl_lua_", "_sv_lua_", "cl_lua_", "sv_lua_", "lua_",
		"g_", "bot_", "skill", "timelimit", "fraglimit", "_menu_", "cl_privacy", "cl_allow_uid",
		"cl_allow_uidtracking", "cl_allow_uidranking", "cl_voice", "con_", "prvm_language",
		"cl_curl_", "cl_esp", "sv_touch_assist", "cl_gentle", "cl_hidewaypoints", "cl_autodemo",
		"_android_", "sys_perfhint", "touch_perf", "cl_deathglow", "cl_reticle", "chase_active",
		"cl_simple", "cl_casings", "cl_weaponpriority", "cl_noteam", "notification_", "utf8_",
		NULL
	};
	cvar_t *v;
	int i;
	if (!name || !*name || strstr(name, "password") || strstr(name, "rcon"))
		return false;
	for (i = 0; prefixes[i]; i++)
		if (!strncmp(name, prefixes[i], strlen(prefixes[i])))
			break;
	if (!prefixes[i])
		return false;
	v = Cvar_FindVar(&cvars_all, name, ~0);
	if (v && (v->flags & CF_PRIVATE))
		return false;
	if (write && v && (v->flags & CF_READONLY))
		return false;
	return true;
}

static void publish_cvars(qbool force)
{
	jb_t b = {0};
	int i;
	jb_raw(&b, "{");
	for (i = 0; i < fe.watch_count; i++)
	{
		cvar_t *v = Cvar_FindVar(&cvars_all, fe.watch[i], ~0);
		jb_key(&b, fe.watch[i]);
		if (v && cvar_allowed(fe.watch[i], false))
			jb_str(&b, v->string);
		else
			jb_raw(&b, "null");
	}
	jb_raw(&b, "}");
	fe_send(FE_CVARS, &b, force);
}

static const char *cvar_str(const char *name)
{
	cvar_t *v = Cvar_FindVar(&cvars_all, name, ~0);
	return v ? v->string : "";
}
static int cvar_int(const char *name)
{
	cvar_t *v = Cvar_FindVar(&cvars_all, name, ~0);
	return v ? v->integer : 0;
}

static void publish_player(qbool force)
{
	jb_t b = {0};
	char clean[256];
	strip_colors(clean, sizeof(clean), cvar_str("_cl_name"));
	jb_raw(&b, "{");
	jb_kstr(&b, "name", cvar_str("_cl_name"));
	jb_kstr(&b, "name_plain", clean);
	jb_kstr(&b, "model", cvar_str("_cl_playermodel"));
	jb_kint(&b, "skin", cvar_int("_cl_playerskin"));
	jb_kint(&b, "color", cvar_int("_cl_color"));
	jb_raw(&b, "}");
	fe_send(FE_PLAYER, &b, force);
}

static qbool private_address(const char *a)
{
	int b0, b1;
	if (sscanf(a, "%d.%d.", &b0, &b1) != 2)
		return false;
	return b0 == 10 || b0 == 127 || (b0 == 172 && b1 >= 16 && b1 <= 31) || (b0 == 192 && b1 == 168)
		|| (b0 == 169 && b1 == 254) || (b0 == 100 && b1 >= 64 && b1 <= 127);
}

static void publish_servers(qbool force)
{
#ifdef CONFIG_MENU
	jb_t b = {0};
	unsigned i, rows = 0;
	char clean[256];
	jb_raw(&b, "{");
	jb_kbool(&b, "refreshing", serverlist_querystage != 0 || (fe.refresh_started > 0 && host.realtime - fe.refresh_started < 4));
	jb_kbool(&b, "lan_only", cvar_int("net_slist_lan_only") != 0);
	jb_kstr(&b, "favorites", cvar_str("net_slist_favorites"));
	jb_key(&b, "rows");
	jb_raw(&b, "[");
	for (i = 0; i < serverlist_cachecount && rows < TOUCHFE_MAX_SERVERS; i++)
	{
		const serverlist_entry_t *e = ServerList_GetCacheEntry(i);
		const serverlist_info_t *in = &e->info;
		if (!e->responded || !in->cname[0])
			continue;
		jb_comma(&b);
		jb_raw(&b, "{");
		jb_kstr(&b, "a", in->cname);
		strip_colors(clean, sizeof(clean), in->name);
		jb_kstr(&b, "n", clean);
		jb_kstr(&b, "m", in->map);
		jb_kstr(&b, "g", in->game);
		jb_kstr(&b, "mod", in->mod);
		jb_kint(&b, "p", in->ping);
		jb_kint(&b, "h", in->numhumans);
		jb_kint(&b, "b", in->numbots);
		jb_kint(&b, "x", in->maxplayers);
		jb_kint(&b, "pr", in->protocol);
		jb_kstr(&b, "q", in->qcstatus);
		jb_kbool(&b, "l", strstr(va(clean, sizeof(clean), "%s:", in->qcstatus), ":L1:") != NULL);
		jb_kbool(&b, "f", in->isfavorite);
		jb_kbool(&b, "lan", private_address(in->cname));
		jb_raw(&b, "}");
		rows++;
	}
	jb_raw(&b, "]");
	jb_kint(&b, "count", rows);
	jb_raw(&b, "}");
	fe_send(FE_SERVERS, &b, force);
#endif
}

static void publish_lan(qbool force)
{
	jb_t b = {0};
	jb_raw(&b, "{");
	jb_kbool(&b, "available", cvar_int("touch_lan_available") != 0);
	jb_kbool(&b, "hosting", cvar_int("touch_lan_hosting") != 0);
	jb_kbool(&b, "on_purpose", TouchLAN_HostingOnPurpose());
	jb_kint(&b, "port", cvar_int("touch_lan_port"));
	jb_kstr(&b, "addresses", cvar_str("touch_lan_addresses"));
	jb_kstr(&b, "players", cvar_str("touch_lan_players"));
	jb_kint(&b, "playercount", cvar_int("touch_lan_playercount"));
	jb_kint(&b, "others", TouchLAN_OtherHumans());
	jb_kint(&b, "portcheck", cvar_int("_touch_lan_portcheck"));
	jb_kbool(&b, "port_free", cvar_int("_touch_lan_port_free") != 0);
	jb_kint(&b, "port_suggest", cvar_int("_touch_lan_port_suggest"));
	jb_kstr(&b, "hostname", cvar_str("touch_lan_hostname"));
	jb_kstr(&b, "gametype", cvar_str("touch_lan_gametype"));
	jb_kstr(&b, "map", cvar_str("touch_lan_map"));
	jb_kint(&b, "bots", cvar_int("touch_lan_bots"));
	jb_kint(&b, "maxplayers", cvar_int("touch_lan_maxplayers"));
	jb_kint(&b, "host_port", cvar_int("touch_lan_host_port"));
	jb_kint(&b, "assist", cvar_int("touch_lan_assist"));
	jb_kbool(&b, "password_set", cvar_int("touch_lan_host_password_set") != 0);
	jb_kbool(&b, "password_remember", cvar_int("touch_lan_host_password_remember") != 0);
	jb_kbool(&b, "join_password_remember", cvar_int("cl_touch_lan_password_remember") != 0);
	// Every other LAN cvar as it is, so what touch_lan.c adds (labelled join
	// addresses across VPNs, a join link, remembered hosts) reaches the UI
	// without a protocol change. Private ones (passwords) never.
	jb_key(&b, "cvars");
	jb_raw(&b, "{");
	{
		cvar_t *v;
		for (v = cvars_all.vars; v; v = v->next)
			if ((!strncmp(v->name, "touch_lan_", 10) || !strncmp(v->name, "_touch_lan_", 11)
				|| !strncmp(v->name, "net_slist_lan", 13) || !strncmp(v->name, "net_slist_remember", 18))
				&& !(v->flags & CF_PRIVATE) && !strstr(v->name, "password"))
				jb_kstr(&b, v->name, v->string);
	}
	jb_raw(&b, "}");
	jb_raw(&b, "}");
	fe_send(FE_LAN, &b, force);
}

static void publish_mods(qbool force)
{
	jb_t b = {0};
	jb_raw(&b, "{");
	jb_kstr(&b, "mods", cvar_str("_cl_lua_mods"));
	jb_kstr(&b, "plugins", cvar_str("_sv_lua_plugins"));
	jb_kstr(&b, "permissions", cvar_str("_cl_lua_permissions"));
	jb_kstr(&b, "select", cvar_str("_lua_select"));
	jb_kstr(&b, "detail", cvar_str("_lua_detail"));
	jb_kstr(&b, "install", cvar_str("_lua_install"));
	jb_kstr(&b, "last", cvar_str("_lua_last"));
	jb_raw(&b, "}");
	fe_send(FE_MODS, &b, force);
}

// The game modes of the Create dialog (qcsrc/common/gametypes): id as
// mapinfo spells it, the cvar MapInfo_SwitchGameType sets, title, teams.
static const struct { const char *id, *cvar, *title; int teams; const char *also; } fe_gametypes[] = {
	{"dm", "g_dm", "Deathmatch", 0, NULL},
	{"tdm", "g_tdm", "Team Deathmatch", 1, NULL},
	{"ctf", "g_ctf", "Capture the Flag", 1, NULL},
	{"ca", "g_ca", "Clan Arena", 1, NULL},
	{"ft", "g_freezetag", "Freeze Tag", 1, NULL},
	{"duel", "g_duel", "Duel", 0, "dm"},
	{"lms", "g_lms", "Last Man Standing", 0, "dm"},
	{"ka", "g_keepaway", "Keepaway", 0, "dm"},
	{"mayhem", "g_mayhem", "Mayhem", 0, "dm"},
	{"tmayhem", "g_tmayhem", "Team Mayhem", 1, "tdm"},
	{"surv", "g_survival", "Survival", 0, "lms"},
	{"kh", "g_keyhunt", "Key Hunt", 1, NULL},
	{"dom", "g_domination", "Domination", 1, NULL},
	{"tka", "g_tka", "Team Keepaway", 1, NULL},
	{"ons", "g_onslaught", "Onslaught", 1, NULL},
	{"as", "g_assault", "Assault", 1, NULL},
	{"rc", "g_race", "Race", 0, NULL},
	{"cts", "g_cts", "Complete the Stage", 0, NULL},
	{"nb", "g_nexball", "Nexball", 1, NULL},
	{"inv", "g_invasion", "Invasion", 0, NULL},
	{NULL, NULL, NULL, 0, NULL}
};

static int gametype_index(const char *id)
{
	int i;
	for (i = 0; fe_gametypes[i].id; i++)
		if (!strcasecmp(fe_gametypes[i].id, id) || !strcasecmp(fe_gametypes[i].cvar + 2, id))
			return i;
	return -1;
}

static void publish_gametypes(void)
{
	jb_t b = {0};
	int i;
	jb_raw(&b, "{\"list\":[");
	for (i = 0; fe_gametypes[i].id; i++)
	{
		jb_comma(&b);
		jb_raw(&b, "{");
		jb_kstr(&b, "id", fe_gametypes[i].id);
		jb_kstr(&b, "cvar", fe_gametypes[i].cvar);
		jb_kstr(&b, "title", fe_gametypes[i].title);
		jb_kbool(&b, "teams", fe_gametypes[i].teams);
		jb_kbool(&b, "current", cvar_int(fe_gametypes[i].cvar) != 0);
		jb_raw(&b, "}");
	}
	jb_raw(&b, "]}");
	fe_send(FE_GAMETYPES, &b, true);
}

// maps/*.mapinfo from every pack and pk3dir: title, author and the modes the
// map lists, plus the ones Xonotic allows on any map of a listed mode.
static void publish_maps(void)
{
	jb_t b = {0};
	fssearch_t *search = FS_Search("maps/*.mapinfo", true, true, NULL);
	int i, rows = 0;
	jb_raw(&b, "{\"list\":[");
	for (i = 0; search && i < search->numfilenames && rows < TOUCHFE_MAX_MAPS; i++)
	{
		char name[MAX_QPATH], title[128] = "", author[128] = "", desc[256] = "", types[256] = "";
		char line[512], *p, *text, *nl;
		const char *f = search->filenames[i];
		fs_offset_t size;
		qbool hidden = false, has[64] = {0};
		int g;
		if (strncmp(f, "maps/", 5) || strchr(f + 5, '/'))
			continue; // maps/autogenerated/ and the like
		dp_strlcpy(name, f + 5, sizeof(name));
		FS_StripExtension(name, name, sizeof(name));
		if (!FS_FileExists(va(line, sizeof(line), "maps/%s.bsp", name)))
			continue;
		text = (char *)FS_LoadFile(f, tempmempool, true, &size);
		if (!text)
			continue;
		for (p = text; p && *p; p = nl)
		{
			size_t l;
			nl = strchr(p, '\n');
			l = nl ? (size_t)(nl - p) : strlen(p);
			if (nl)
				nl++;
			if (l >= sizeof(line))
				l = sizeof(line) - 1;
			memcpy(line, p, l);
			line[l] = 0;
			while (l && (line[l - 1] == '\r' || line[l - 1] == ' '))
				line[--l] = 0;
			if (!strncmp(line, "title ", 6))
				dp_strlcpy(title, line + 6, sizeof(title));
			else if (!strncmp(line, "author ", 7))
				dp_strlcpy(author, line + 7, sizeof(author));
			else if (!strncmp(line, "description ", 12))
				dp_strlcpy(desc, line + 12, sizeof(desc));
			else if (!strcmp(line, "hidden") || !strcmp(line, "forbidden"))
				hidden = true;
			else if (!strncmp(line, "gametype ", 9) || !strncmp(line, "type ", 5))
			{
				char id[32];
				sscanf(line + (line[0] == 'g' ? 9 : 5), "%31s", id);
				if ((g = gametype_index(id)) >= 0)
					has[g] = true;
			}
		}
		Mem_Free(text);
		if (hidden)
			continue;
		for (g = 0; fe_gametypes[g].id; g++)
			if (fe_gametypes[g].also && !has[g])
			{
				int base = gametype_index(fe_gametypes[g].also);
				if (base >= 0 && has[base])
					has[g] = true;
			}
		for (g = 0; fe_gametypes[g].id; g++)
			if (has[g])
			{
				if (types[0])
					dp_strlcat(types, " ", sizeof(types));
				dp_strlcat(types, fe_gametypes[g].id, sizeof(types));
			}
		if (!types[0])
			continue;
		jb_comma(&b);
		jb_raw(&b, "{");
		jb_kstr(&b, "name", name);
		jb_kstr(&b, "title", title[0] ? title : name);
		jb_kstr(&b, "author", author);
		jb_kstr(&b, "description", desc);
		jb_kstr(&b, "types", types);
		jb_raw(&b, "}");
		rows++;
	}
	if (search)
		FS_FreeSearch(search);
	jb_raw(&b, "]}");
	fe_send(FE_MAPS, &b, true);
}

// A picture out of the engine's filesystem (packs, pk3dirs) copied into the
// userdir, where the UI can decode it: ref "pk3:<path>" with or without an
// extension (.jpg, then .png: what Android decodes), or a bare map name for
// its preview (maps/<name>). Answered on the queue as an "image" message
// {ref, path}, path "" when there is none.
static void publish_image(const char *ref)
{
	static const char *exts[] = {"", ".jpg", ".png", NULL};
	jb_t b = {0};
	char base[MAX_QPATH], path[MAX_QPATH], out[MAX_QPATH], full[MAX_OSPATH] = "", safe[MAX_QPATH];
	const char *p;
	int i, n = 0;
	if (!strncmp(ref, "pk3:", 4))
		dp_strlcpy(base, ref + 4, sizeof(base));
	else
		dpsnprintf(base, sizeof(base), "maps/%s", ref);
	for (p = base; *p && n + 1 < (int)sizeof(safe); p++)
		safe[n++] = (isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == '.') ? *p : '_';
	safe[n] = 0;
	for (i = 0; exts[i] && base[0] && !strstr(base, ".."); i++)
	{
		fs_offset_t size;
		unsigned char *data;
		const char *ext = exts[i][0] ? exts[i] : FS_FileExtension(base);
		if (!exts[i][0] && strcasecmp(ext, "jpg") && strcasecmp(ext, "png") && strcasecmp(ext, "jpeg"))
			continue; // a bare path is only taken as it is when Android can decode it
		dpsnprintf(path, sizeof(path), "%s%s", base, exts[i]);
		dpsnprintf(out, sizeof(out), "fe_cache/%s%s", safe, exts[i]);
		if (FS_SysFileExists(va(full, sizeof(full), "%s%s", fs_gamedir, out)))
			break;
		data = FS_LoadFile(path, tempmempool, true, &size);
		if (!data)
		{
			full[0] = 0;
			continue;
		}
		FS_WriteFile(out, data, size);
		Mem_Free(data);
		dpsnprintf(full, sizeof(full), "%s%s", fs_gamedir, out);
		break;
	}
	if (!exts[i])
		full[0] = 0;
	jb_raw(&b, "{");
	jb_kstr(&b, "ref", ref);
	jb_kstr(&b, "path", full);
	jb_raw(&b, "}");
	TouchFE_Put("image", b.s, 1);
	// fe_dump preview keeps working for tests
	fe_send(FE_PREVIEW, &b, true);
}

static void publish_preview(const char *map)
{
	publish_image(map);
}

// ---------------------------------------------------------------------------
// In

static void fe_decode(char *out, size_t size, const char *in)
{
	size_t n = 0;
	if (!strcmp(in, "~"))
		in = "";
	while (*in && n + 1 < size)
	{
		if (in[0] == '%' && isxdigit((unsigned char)in[1]) && isxdigit((unsigned char)in[2]))
		{
			char hex[3] = {in[1], in[2], 0};
			int c = (int)strtol(hex, NULL, 16);
			if (c >= 0x20 || c == '\t') // no control bytes, no newlines into commands
				out[n++] = (char)c;
			in += 3;
		}
		else
			out[n++] = *in++;
	}
	out[n] = 0;
}

// The current fe command's arguments, decoded once (fe_decode_args): every
// arg() of one command stays valid while it runs, macros evaluating it twice
// included.
#define FE_MAXARGS 24
static char fe_args[FE_MAXARGS][1024];
static int fe_nargs;
static void fe_decode_args(cmd_state_t *cmd)
{
	int i;
	fe_nargs = min(Cmd_Argc(cmd), FE_MAXARGS);
	for (i = 0; i < FE_MAXARGS; i++)
		fe_decode(fe_args[i], sizeof(fe_args[i]), i < fe_nargs ? Cmd_Argv(cmd, i) : "");
}
static const char *arg(cmd_state_t *cmd, int i)
{
	(void)cmd;
	return i >= 0 && i < FE_MAXARGS ? fe_args[i] : "";
}

// Safe inside a quoted console argument.
static qbool quotable(const char *s)
{
	return !strpbrk(s, "\"\n\r;$\\");
}
static qbool plain_name(const char *s)
{
	if (!*s)
		return false;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-' && *s != '.')
			return false;
	return true;
}

static void fe_set_cvar(const char *name, const char *value)
{
	cvar_t *v = Cvar_FindVar(&cvars_all, name, ~0);
	if (v)
		Cvar_SetQuick(v, value);
	else
		Cvar_Get(&cvars_all, name, value, CF_CLIENT, NULL);
}

static void fe_set_gametype(int g)
{
	int i;
	for (i = 0; fe_gametypes[i].id; i++)
		fe_set_cvar(fe_gametypes[i].cvar, i == g ? "1" : "0");
}

static void fe_detach(const char *why)
{
	if (!fe.attached)
		return;
	Con_Printf("Front-end bridge: detached (%s); the QuakeC menu is in charge\n", why);
	uib_close();
	fe.attached = false;
	fe.advanced = false;
	fe.screen[0] = 0;
	fe.cover = 0;
}

static void fe_mod(cmd_state_t *cmd)
{
	const char *verb = arg(cmd, 2), *realm = arg(cmd, 3), *name = arg(cmd, 4), *extra = arg(cmd, 5);
	const char *p = !strcmp(realm, "plugin") ? "sv_lua_plugin_" : "lua_mod_";
	char line[1200];
	if (!strcmp(verb, "confirm")) { Cbuf_AddText(cmd_local, "\nlua_install_confirm\n"); return; }
	if (!strcmp(verb, "cancel")) { Cbuf_AddText(cmd_local, "\nlua_install_cancel\n"); return; }
	if (!strcmp(verb, "install"))
	{
		if (!*name || !quotable(name)) { Con_Printf("fe mod install: refused source\n"); return; }
		dpsnprintf(line, sizeof(line), "\n%sinstall \"%s\"\n", p, name);
		Cbuf_AddText(cmd_local, line);
		return;
	}
	if (!strcmp(verb, "select"))
	{
		fe_set_cvar("_lua_select", *name && plain_name(name) ? va(line, sizeof(line), "%s %s", !strcmp(realm, "plugin") ? "plugin" : "mod", name) : "");
		return;
	}
	if (!plain_name(name)) { Con_Printf("fe mod %s: refused name\n", verb); return; }
	if (!strcmp(verb, "enable") || !strcmp(verb, "disable"))
		dpsnprintf(line, sizeof(line), "\n%s%s %s\n", p, verb, name);
	else if ((!strcmp(verb, "grant") || !strcmp(verb, "revoke")) && plain_name(extra))
		dpsnprintf(line, sizeof(line), "\n%spermission %s %s %s\n", p, name, verb, extra);
	else if (!strcmp(verb, "uninstall"))
		dpsnprintf(line, sizeof(line), "\n%suninstall %s%s\n", p, name, !strcmp(extra, "keepdata") ? " keepdata" : "");
	else { Con_Printf("fe mod: unknown verb %s\n", verb); return; }
	Cbuf_AddText(cmd_local, line);
}

// The Host screen's Start (dialog_touch_lan.qc, TouchLan_Start), with the
// map and mode checked by the UI against the maps topic.
static void fe_host(cmd_state_t *cmd)
{
	const char *name = arg(cmd, 2), *type = arg(cmd, 3), *map = arg(cmd, 4);
	int bots = bound(0, atoi(arg(cmd, 5)), 32);
	int slots = bound(2, max(atoi(arg(cmd, 6)), bots + 2), 64);
	int port = atoi(arg(cmd, 7)), assist = bound(0, atoi(arg(cmd, 8)), 7);
	const char *pw = arg(cmd, 9), *remember = arg(cmd, 10);
	int g = gametype_index(type);
	char line[512], hname[128], *c;
	cvar_t *v;
	if (g < 0 || !plain_name(map) || port < 1024 || port > 65535)
	{
		fe_event("host_failed", "reason", "bad request", NULL, NULL);
		return;
	}
	fe_set_cvar("_touch_lan_portcheck", va(line, sizeof(line), "%d", port));
	if (!cvar_int("_touch_lan_port_free"))
	{
		fe_event("host_failed", "reason", "port", "suggest", cvar_str("_touch_lan_port_suggest"));
		publish_lan(true);
		return;
	}
	dp_strlcpy(hname, name, sizeof(hname));
	for (c = hname; *c; c++)
		if (*c == '"' || *c == '\\' || *c == ';' || *c == '$')
			*c = '\'';
	fe_set_cvar("touch_lan_hostname", hname);
	fe_set_cvar("touch_lan_gametype", fe_gametypes[g].id);
	fe_set_cvar("touch_lan_map", map);
	fe_set_cvar("touch_lan_bots", va(line, sizeof(line), "%d", bots));
	fe_set_cvar("touch_lan_maxplayers", va(line, sizeof(line), "%d", slots));
	fe_set_cvar("touch_lan_host_port", va(line, sizeof(line), "%d", port));
	fe_set_cvar("touch_lan_assist", va(line, sizeof(line), "%d", assist));
	if (*remember)
		fe_set_cvar("touch_lan_host_password_remember", atoi(remember) ? "1" : "0");
	// a new password replaces the remembered one; "~" (empty) keeps it
	if (*pw && (v = Cvar_FindVar(&cvars_all, "touch_lan_host_password", ~0)))
		Cvar_SetQuick(v, pw);
	Cbuf_AddText(cmd_local, "\ndisconnect; wait; g_campaign 0\n");
	Cbuf_AddText(cmd_local, va(line, sizeof(line), "maxplayers %d\nport %d\n", slots, port));
	Cbuf_AddText(cmd_local, "sv_public 0\nsv_status_privacy 1\n");
	Cbuf_AddText(cmd_local, va(line, sizeof(line), "hostname \"%s\"\nbot_number %d\nset sv_touch_assist %d\n", hname, bots, assist));
	Cbuf_AddText(cmd_local, "touch_lan_host_password_apply\n");
	fe_set_gametype(g);
	Cbuf_AddText(cmd_local, va(line, sizeof(line), "map %s\n", map));
}

// A local bot match (Singleplayer / quick play): one slot for the player and
// one per bot, no LAN listing.
static void fe_play(cmd_state_t *cmd)
{
	const char *type = arg(cmd, 2), *map = arg(cmd, 3);
	int bots = bound(0, atoi(arg(cmd, 4)), 32);
	const char *sk = arg(cmd, 5), *tl = arg(cmd, 6), *fl = arg(cmd, 7);
	int g = gametype_index(type), i;
	char line[256];
	if (g < 0 || !plain_name(map))
	{
		fe_event("play_failed", "reason", "bad request", NULL, NULL);
		return;
	}
	Cbuf_AddText(cmd_local, "\ndisconnect; wait; g_campaign 0\n");
	Cbuf_AddText(cmd_local, va(line, sizeof(line), "maxplayers %d\nsv_public 0\nbot_number %d\n", max(bots + 1, 2), bots));
	if (*sk)
		Cbuf_AddText(cmd_local, va(line, sizeof(line), "skill %d\n", bound(0, atoi(sk), 10)));
	if (*tl)
		Cbuf_AddText(cmd_local, va(line, sizeof(line), "timelimit_override %d\n", bound(-1, atoi(tl), 999)));
	if (*fl)
		Cbuf_AddText(cmd_local, va(line, sizeof(line), "fraglimit_override %d\n", bound(-1, atoi(fl), 9999)));
	// mutators: the rest of the arguments, cvars set to 1 for this match (g_instagib, ...)
	for (i = 8; i < Cmd_Argc(cmd) && i < 24; i++)
	{
		const char *m = arg(cmd, i);
		if (plain_name(m) && !strncmp(m, "g_", 2))
			Cbuf_AddText(cmd_local, va(line, sizeof(line), "settemp %s 1\n", m));
	}
	fe_set_gametype(g);
	Cbuf_AddText(cmd_local, va(line, sizeof(line), "map %s\n", map));
}

static void fe_menu(cmd_state_t *cmd)
{
	const char *what = arg(cmd, 2);
	char line[256];
	if (!strcmp(what, "resume"))
		Cbuf_AddText(cmd_local, "\ntogglemenu 0\n");
	else if (!strcmp(what, "leave"))
		Cbuf_AddText(cmd_local, "\ndisconnect\ntogglemenu 1\n"); // the (hidden) menu open again, as after any disconnect
	else if (!strcmp(what, "quit"))
		Cbuf_AddText(cmd_local, "\nquit\n");
	else if (!strcmp(what, "console"))
		Cbuf_AddText(cmd_local, "\ntoggleconsole\n");
	else if (!strcmp(what, "qcmenu"))
	{
		const char *dialog = arg(cmd, 3);
		fe.advanced = true;
		Cbuf_AddText(cmd_local, "\ntogglemenu 1\n");
		if (plain_name(dialog))
			Cbuf_AddText(cmd_local, va(line, sizeof(line), "menu_cmd directmenu %s\n", dialog));
	}
	else if (!strcmp(what, "native"))
	{
		fe.advanced = false;
		// in a match the QuakeC menu closes with it; outside one it stays
		// open (hidden) as the menu the native screens stand for
		if (cls.state == ca_connected)
			Cbuf_AddText(cmd_local, "\ntogglemenu 0\n");
		else
			Cbuf_AddText(cmd_local, "\ntogglemenu 1\n");
	}
	else if (!strcmp(what, "open"))
		Cbuf_AddText(cmd_local, "\ntogglemenu 1\n");
}

static fe_topic_t topic_index(const char *name)
{
	int t;
	for (t = 0; t < FE_TOPICS; t++)
		if (!strcmp(fe_topic_names[t], name))
			return (fe_topic_t)t;
	return FE_TOPICS;
}

static void publish_topic(fe_topic_t t)
{
	switch (t)
	{
	case FE_APP: { unsigned h = fe.hash[t]; publish_app(); if (h == fe.hash[t]) { jb_t b = {0}; jb_raw(&b, fe.last[t]); fe_send(t, &b, true); } break; }
	case FE_CONNECT: publish_connect(true); break;
	case FE_CVARS: publish_cvars(true); break;
	case FE_SERVERS: publish_servers(true); break;
	case FE_LAN: publish_lan(true); break;
	case FE_MAPS: publish_maps(); break;
	case FE_GAMETYPES: publish_gametypes(); break;
	case FE_MODS: publish_mods(true); break;
	case FE_PLAYER: publish_player(true); break;
	default: break;
	}
}

static void TouchFE_Cmd_f(cmd_state_t *cmd)
{
	const char *verb = Cmd_Argc(cmd) > 1 ? Cmd_Argv(cmd, 1) : "";
	char line[512];
	int i;
	fe_decode_args(cmd);
	if (!strcmp(verb, "attach"))
	{
		fe.ui_protocol = atoi(arg(cmd, 2));
		if (fe.ui_protocol != TOUCHFE_PROTOCOL)
		{
			Con_Printf("Front-end bridge: the UI speaks protocol %d, the engine %d; not attaching\n", fe.ui_protocol, TOUCHFE_PROTOCOL);
			fe_event("refused", "reason", "protocol", NULL, NULL);
			return;
		}
		fe.attached = true;
		fe.beat = Sys_DirtyTime();
		Con_Printf("Front-end bridge: attached (protocol %d)%s\n", fe.ui_protocol, touch_native_frontend.integer ? "" : "; touch_native_frontend is 0, so the QuakeC menu stays in charge");
		for (i = 0; i < FE_TOPICS; i++)
			fe.hash[i] = 0;
		publish_app();
	}
	else if (!strcmp(verb, "detach"))
		fe_detach(Cmd_Argc(cmd) > 2 ? arg(cmd, 2) : "the UI let go");
	else if (!strcmp(verb, "beat"))
		fe.beat = Sys_DirtyTime();
	else if (!strcmp(verb, "screen"))
	{
		dp_strlcpy(fe.screen, arg(cmd, 2), sizeof(fe.screen));
		fe.cover = bound(0, atoi(arg(cmd, 3)), 2);
		fe.slow_next = 0;
	}
	else if (!strcmp(verb, "sub"))
	{
		fe_topic_t t = topic_index(arg(cmd, 2));
		if (t < FE_TOPICS)
		{
			fe.sub[t] = atoi(arg(cmd, 3)) != 0;
			if (fe.sub[t])
				publish_topic(t);
		}
	}
	else if (!strcmp(verb, "get"))
	{
		fe_topic_t t = topic_index(arg(cmd, 2));
		if (t < FE_TOPICS)
			publish_topic(t);
	}
	else if (!strcmp(verb, "watch"))
	{
		// fe watch <cvar>...: replaces the watched set (the cvars topic)
		for (i = 0; i < fe.watch_count; i++)
			Z_Free(fe.watch[i]);
		fe.watch_count = 0;
		for (i = 2; i < Cmd_Argc(cmd) && fe.watch_count < TOUCHFE_MAX_WATCH; i++)
		{
			char n[256];
			fe_decode(n, sizeof(n), Cmd_Argv(cmd, i));
			if (cvar_allowed(n, false))
				fe.watch[fe.watch_count++] = Z_strdup(n);
		}
		publish_cvars(true);
	}
	else if (!strcmp(verb, "watch_add"))
	{
		for (i = 2; i < Cmd_Argc(cmd) && fe.watch_count < TOUCHFE_MAX_WATCH; i++)
		{
			char n[256];
			int j;
			fe_decode(n, sizeof(n), Cmd_Argv(cmd, i));
			for (j = 0; j < fe.watch_count && strcmp(fe.watch[j], n); j++)
				;
			if (j == fe.watch_count && cvar_allowed(n, false))
				fe.watch[fe.watch_count++] = Z_strdup(n);
		}
		publish_cvars(true);
	}
	else if (!strcmp(verb, "set"))
	{
		const char *name = arg(cmd, 2), *value = arg(cmd, 3);
		if (!cvar_allowed(name, true))
		{
			Con_Printf("fe set: %s is not in the front-end schema\n", name);
			fe_event("refused", "cvar", name, NULL, NULL);
			return;
		}
		fe_set_cvar(name, value);
		publish_cvars(false);
	}
	else if (!strcmp(verb, "refresh"))
	{
#ifdef CONFIG_MENU
		const char *which = arg(cmd, 2);
		cvar_t *lan = Cvar_FindVar(&cvars_all, "net_slist_lan_only", ~0);
		if (lan)
			Cvar_SetQuick(lan, !strcmp(which, "lan") ? "1" : "0");
		ServerList_QueryList(!strcmp(arg(cmd, 3), "keep") ? false : true, true, false, false);
		fe.refresh_started = host.realtime;
		fe.sub[FE_SERVERS] = true;
		publish_servers(true);
#endif
	}
	else if (!strcmp(verb, "favorite"))
	{
		const char *addr = arg(cmd, 2);
		qbool on = atoi(arg(cmd, 3)) != 0;
		char list[4096] = "", tok[256];
		const char *p = cvar_str("net_slist_favorites");
		qbool found = false;
		if (!quotable(addr) || strchr(addr, ' ') || !*addr)
			return;
		while (COM_ParseToken_Console(&p))
		{
			dp_strlcpy(tok, com_token, sizeof(tok));
			if (!strcmp(tok, addr))
			{
				found = true;
				if (!on)
					continue;
			}
			if (list[0])
				dp_strlcat(list, " ", sizeof(list));
			dp_strlcat(list, tok, sizeof(list));
		}
		if (on && !found)
		{
			if (list[0])
				dp_strlcat(list, " ", sizeof(list));
			dp_strlcat(list, addr, sizeof(list));
		}
		fe_set_cvar("net_slist_favorites", list);
#ifdef CONFIG_MENU
		ServerList_RebuildViewList(NULL);
#endif
		publish_servers(true);
	}
	else if (!strcmp(verb, "connect"))
	{
		const char *addr = arg(cmd, 2);
		if (!*addr || !quotable(addr))
			return;
		Cbuf_AddText(cmd_local, va(line, sizeof(line), "\nconnect \"%s\"\n", addr));
	}
	else if (!strcmp(verb, "password"))
	{
		// fe password <address> <password> [remember]: the password prompt's
		// Join (TouchLan_PasswordJoin); the password only ever lives in the
		// private cvar touch_lan.c answers the challenge from
		const char *addr = arg(cmd, 2), *pw = arg(cmd, 3), *remember = arg(cmd, 4);
		cvar_t *v = Cvar_FindVar(&cvars_all, "_cl_touch_lan_password", ~0);
		if (!*addr || !quotable(addr) || !*pw || !v)
			return;
		Cvar_SetQuick(v, pw);
		fe_set_cvar("_cl_touch_lan_password_server", addr);
		if (*remember)
			fe_set_cvar("cl_touch_lan_password_remember", atoi(remember) ? "1" : "0");
		Cbuf_AddText(cmd_local, va(line, sizeof(line), "\nconnect \"%s\"\n", addr));
	}
	else if (!strcmp(verb, "cancel"))
	{
		if (!TouchHUD_LoadingBack())
		{
			cls.connect_trying = false;
			Cbuf_AddText(cmd_local, "\ndisconnect\n");
		}
	}
	else if (!strcmp(verb, "portcheck"))
	{
		fe_set_cvar("_touch_lan_portcheck", va(line, sizeof(line), "%d", atoi(arg(cmd, 2))));
		publish_lan(true);
	}
	else if (!strcmp(verb, "host"))
		fe_host(cmd);
	else if (!strcmp(verb, "stophost"))
	{
		cvar_t *v = Cvar_FindVar(&cvars_all, "sv_touch_lan_password", ~0);
		Cbuf_AddText(cmd_local, "\ndisconnect\n");
		if (v)
			Cvar_SetQuick(v, "");
	}
	else if (!strcmp(verb, "clear_host_password"))
	{
		cvar_t *v = Cvar_FindVar(&cvars_all, "touch_lan_host_password", ~0);
		if (v)
			Cvar_SetQuick(v, "");
		publish_lan(true);
	}
	else if (!strcmp(verb, "play"))
		fe_play(cmd);
	else if (!strcmp(verb, "campaign"))
		Cbuf_AddText(cmd_local, "\nmenu_cmd directmenu Campaign\n");
	else if (!strcmp(verb, "menu"))
		fe_menu(cmd);
	else if (!strcmp(verb, "mod"))
		fe_mod(cmd);
	else if (!strcmp(verb, "preview"))
		publish_preview(arg(cmd, 2));
	else if (!strcmp(verb, "image"))
		publish_image(arg(cmd, 2));
	else if (!strcmp(verb, "exec"))
	{
		// fe exec <cfg>: a Touch preset (touch/*.cfg, profiles), never a path out
		const char *cfg = arg(cmd, 2);
		if (plain_name(cfg) || (!strncmp(cfg, "touch/", 6) && plain_name(cfg + 6)) || (!strncmp(cfg, "touch/profiles/", 15) && plain_name(cfg + 15)))
			Cbuf_AddText(cmd_local, va(line, sizeof(line), "\nexec %s\n", cfg));
	}
	else if (!strcmp(verb, "save"))
		Cbuf_AddText(cmd_local, "\nsaveconfig\n");
	else if (!strcmp(verb, "ui") && !fe_verb("ui"))
	{
		// a UI bridge message from the native renderer: decoded whole, not through fe_args
		const char *raw = Cmd_Argc(cmd) > 2 ? Cmd_Argv(cmd, 2) : "";
		size_t n = strlen(raw) + 1;
		char *json = (char *)malloc(n);
		if (!json)
			return;
		fe_decode(json, n, raw);
		if (!fe.uib && uib_present() && fe.attached)
		{
			fe.uib = TouchUIB_Open("android", 7);
			if (fe.uib && TouchUIB_SetNotify)
				TouchUIB_SetNotify(fe.uib, uib_drain, NULL);
		}
		if (fe.uib && TouchUIB_Post(fe.uib, json, strlen(json)) == -1)
			uib_close(); // closed by the engine: the next message opens a new one (a hello)
		free(json);
		uib_drain(NULL);
	}
	else if (fe_verb(verb))
		fe_verb(verb)(arg(cmd, 2));
	else
		Con_Printf("fe: unknown verb \"%s\" (docs/FRONTEND_BRIDGE.md)\n", verb);
	fe_update_state();
}

// fe_dump <topic>: the topic's newest JSON, built now.
static void TouchFE_Dump_f(cmd_state_t *cmd)
{
	fe_topic_t t = topic_index(Cmd_Argc(cmd) > 1 ? Cmd_Argv(cmd, 1) : "");
	if (t >= FE_TOPICS)
	{
		Con_Printf("usage: fe_dump app|connect|cvars|servers|lan|maps|gametypes|mods|player|preview\n");
		return;
	}
	if (t != FE_PREVIEW)
		publish_topic(t);
	Con_Printf("FE %s %.8000s\n", fe_topic_names[t], fe.last[t] ? fe.last[t] : "null");
}

// fe_take: the pending events, then the topics that changed since the last
// fe_take. Long topics are cut at 8000 bytes (the console's line limit).
static void TouchFE_Take_f(cmd_state_t *cmd)
{
	int t;
	char *p, *nl;
	for (p = fe.events; *p; p = nl + 1)
	{
		nl = strchr(p, '\n');
		if (!nl)
			break;
		*nl = 0;
		Con_Printf("FE event %s\n", p);
	}
	fe.events[0] = 0;
	for (t = 0; t < FE_TOPICS; t++)
		if (fe.fresh[t])
		{
			fe.fresh[t] = false;
			Con_Printf("FE %s %.8000s\n", fe_topic_names[t], fe.last[t]);
		}
}

// ---------------------------------------------------------------------------
// Frame

void TouchFE_Put(const char *topic, const char *json, int event)
{
	if (!topic || !json)
		return;
	if (fe.have_sink && fe.sink.put)
		fe.sink.put(topic, json, event);
	if (fe_echo.integer)
		Con_Printf("FE> %s %s\n", topic, json);
}

void TouchFE_RegisterVerb(const char *verb, touchfe_verb_fn fn)
{
	if (fe.nverbs < (int)(sizeof(fe.verbs) / sizeof(fe.verbs[0])) && strlen(verb) < sizeof(fe.verbs[0].verb))
	{
		dp_strlcpy(fe.verbs[fe.nverbs].verb, verb, sizeof(fe.verbs[0].verb));
		fe.verbs[fe.nverbs++].fn = fn;
	}
}

void TouchFE_SetSink(const touchfe_sink_t *sink)
{
	int t;
	fe.sink = *sink;
	fe.have_sink = true;
	for (t = 0; t < FE_TOPICS; t++)
		if (fe.last[t])
			fe.sink.put(fe_topic_names[t], fe.last[t], 0);
}

// The hidden QuakeC menu does not run m_draw, where it notices that the
// engine took the keys back (a join hands them to the game without telling
// the menu). Tell it here, or it still thinks it is open, and the next
// `togglemenu` (the pause button) closes it instead of opening it.
static void fe_sync_menu(void)
{
	static int prev_kd = -1;
	int kd = key_dest;
	if (prev_kd != -1 && fe_owns_menu() && kd == key_game
		&& (prev_kd == key_menu || prev_kd == key_menu_grabbed))
	{
#ifdef CONFIG_MENU
		if (MR_ToggleMenu)
			MR_ToggleMenu(0);
#endif
	}
	prev_kd = kd;
}

void TouchFE_Frame(void)
{
	qbool visible, joining;
	if (cls.state == ca_dedicated)
		return;
	if (!touch_native_frontend.integer && !fe.attached)
		return; // nothing to tell anyone
	fe_sync_menu();
	if (fe.uib)
		uib_drain(NULL);
	publish_app();
	watch_status();
	if (fe.attached && beat_age() > 30 && !cls.demoplayback)
		fe_detach("no heartbeat for 30 s");
	fe_update_state();
	joining = cls.connect_trying || (cls.state == ca_connected && cls.signon < SIGNONS) || scr_loading;
	if (joining || fe.was_joining || fe.sub[FE_CONNECT])
		publish_connect(false); // and once more when a join ends: refused, failed or in
	fe.was_joining = joining;
	visible = fe.attached && fe.screen[0] && strcmp(fe.screen, "none");
	if (!visible || host.realtime < fe.slow_next)
		return;
	fe.slow_next = host.realtime + TOUCHFE_SLOW_PERIOD;
	if (fe.watch_count)
		publish_cvars(false);
	if (fe.sub[FE_SERVERS])
		publish_servers(false);
	if (fe.sub[FE_LAN])
		publish_lan(false);
	if (fe.sub[FE_MODS])
		publish_mods(false);
	if (fe.sub[FE_PLAYER])
		publish_player(false);
}

// Covered by a native screen in front of the menu or a join. Never in a
// match: the screen comes down the moment play starts, and the first frame
// of play must already be on the surface by then.
static int fe_covered(void)
{
	if (!fe_owns_menu() || !fe.screen[0] || !strcmp(fe.screen, "none"))
		return 0;
	if (!in_menu() && cls.state == ca_connected && cls.signon == SIGNONS)
		return 0;
	if (key_consoleactive & KEY_CONSOLEACTIVE_USER)
		return 0; // the console is drawn over it, with the UI out of the way
	return fe.cover;
}

qbool TouchFE_SkipDraw(void)
{
	return fe_covered() >= 2;
}

double TouchFE_MaxFPS(double maxfps)
{
	extern cvar_t cl_maxidlefps;
	static double since;
	int c = fe_covered();
	if (!c)
	{
		since = 0;
		return maxfps;
	}
	if (!since)
		since = host.realtime;
	// The first 45 s behind a covering screen run at the normal rate (nothing
	// is drawn): touch_warm prepares the next game a slice per frame then.
	if (host.realtime - since < 45 && Cvar_VariableValue(&cvars_all, "touch_warm", ~0))
		return maxfps;
	if (cl_maxidlefps.value > 0 && (maxfps <= 0 || maxfps > cl_maxidlefps.value))
		return cl_maxidlefps.value;
	return maxfps;
}

void TouchFE_Loading(const char *msg, float fraction)
{
	if (!fe.attached)
		return;
	dp_strlcpy(fe.load_msg, msg ? msg : "", sizeof(fe.load_msg));
	fe.load_fraction = fraction;
	// a load runs inside one frame: tell the UI from here, a few times a second
	if (Sys_DirtyTime() - fe.load_published > 0.2)
	{
		fe.load_published = Sys_DirtyTime();
		publish_connect(false);
	}
	if (fe.have_sink && fe.sink.cancel_requested && fe.sink.cancel_requested())
		TouchHUD_LoadingBack(); // the abort point right after this stops the load
}

void TouchFE_Init(void)
{
	Cvar_RegisterVariable(&touch_native_frontend);
	Cvar_RegisterVariable(&fe_echo);
	Cvar_RegisterVariable(&fe_state);
	Cvar_RegisterCallback(&touch_native_frontend, fe_switch_changed);
	Cmd_AddCommand(CF_CLIENT, "fe", TouchFE_Cmd_f, "native front-end bridge intent: fe <verb> <percent-encoded args> (docs/FRONTEND_BRIDGE.md)");
	Cmd_AddCommand(CF_CLIENT, "fe_dump", TouchFE_Dump_f, "print a front-end bridge topic as JSON: fe_dump app|connect|cvars|servers|lan|maps|gametypes|mods|player|preview");
	Cmd_AddCommand(CF_CLIENT, "fe_take", TouchFE_Take_f, "print the front-end bridge topics that changed since the last fe_take, and the pending events");
}
