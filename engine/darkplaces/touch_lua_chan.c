/*
Godnotic: messages between server mods and client mods, and pages a server
shows on a player's screen (docs/LUA_API.md, "Messages between server and
client mods").

xt.channel(name) is the same object on both sides: a server mod sends a
small value (a table of strings, numbers and booleans, or one of those) to
one player or to every player; a client mod listening on that channel name
gets it, and can send requests back. This is what lets a server-side game
mode ship a client HUD.

The wire, with no new message type, so stock peers are never confused:

  server -> client   svc_stufftext lines that start with "//", which a stock
                     client's console treats as a comment and ignores:
                       //xt1h <version>                         hello: this server speaks it
                       //xt1m <channel> <value>                 a message
                       //xt1p <page> <part> <parts> <chunk>     a page (JSON, in parts)
                       //xt1c <page prefix>                     close pages
                     This game's client takes these lines before the console
                     (csprogs.c, CL_VM_Parse_StuffCmd) and never runs them.
  client -> server   clc_stringcmd, only after the server's hello (so only
                     to servers that know them):
                       xthello <version>
                       xtmsg <channel> <value>
                       xtpage <page> <row> <event> <value>

<value> is JSON with every byte outside A-Z a-z 0-9 _ . ~ - written as %XX, so a frame is one line of tokens without spaces, quotes, ;
or $. Everything read from the other side is bounds-checked before it is
used: names by their character set and length, values at most 1 KB of JSON
(16 KB for a page, 64 parts), JSON depth 32, and rates per player: the server
sends a player at most 20 messages a second (a burst of 40) and only into the
free space of its reliable buffer; a player sends at most 5 messages a second
(burst 10) and 20 page answers; a client takes at most 60 a second from the
server. Over the limits a message is dropped, never queued without bound.

The player decides: cl_lua_channels 0 turns all of it off; lua_channel_block
<server|*> [channel] refuses one server's (or every server's) messages to
the mods, kept in luastate/mods/channels_blocked.txt; cl_lua_server_pages 0
refuses pages from servers. A page a server sends is marked from_server and
the renderers show "Page from the server" over it.
*/

#include "quakedef.h"
#include "progsvm.h"
#include "touch_lua.h"
#include "touch_lua_int.h"
#include "touch_lua_api_gen.h"

static cvar_t cl_lua_channels = {CF_CLIENT | CF_ARCHIVE, "cl_lua_channels", "1", "let the servers you join talk to your Lua mods over mod channels (lua_channel_block refuses one server)"};
static cvar_t cl_lua_server_pages = {CF_CLIENT | CF_ARCHIVE, "cl_lua_server_pages", "1", "let server mods show pages (forms, menus) on your screen; they are marked as pages from the server"};

void TLua_RegisterCvar(cvar_t *var);
void TLua_Guard(cvar_t *var);
size_t TLua_JsonEncode(lua_State *L, int idx, char *out, size_t outsize);
qbool TLua_JsonDecode(lua_State *L, const char *s, size_t len);
void TLua_UI_ServerReply(int player, const char *id, const char *row, const char *event, const char *value);
void TLua_UI_ServerPage(const char *pageid, const char *json, const char *servermod);
void TLua_UI_ServerPageClose(const char *prefix);

#define TLUA_CHAN_VERSION 1
#define TLUA_CHAN_MAXVALUE 1024        // JSON bytes of one message
#define TLUA_CHAN_MAXPAGE (16 * 1024)  // JSON bytes of one page
#define TLUA_CHAN_CHUNK 900            // encoded bytes of one page part
#define TLUA_CHAN_MAXSUBS 128

// ---------------------------------------------------------------------------
// Encoding

static qbool tlua_chan_name_ok(const char *s, size_t max)
{
	size_t n = strlen(s);
	if (!n || n > max)
		return false;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '.' || *s == '@'))
			return false;
	return true;
}

static size_t tlua_chan_encode(const char *in, size_t len, char *out, size_t size)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t i, o = 0;
	for (i = 0; i < len; i++)
	{
		unsigned char c = (unsigned char)in[i];
		if (c < 128 && (isalnum(c) || c == '_' || c == '.' || c == '~' || c == '-'))
		{
			if (o + 2 > size)
				return 0;
			out[o++] = (char)c;
		}
		else
		{
			if (o + 4 > size)
				return 0;
			out[o++] = '%';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 15];
		}
	}
	out[o] = 0;
	return o;
}

static int tlua_hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

// -1 on a bad escape, an unexpected byte or no room
static long tlua_chan_decode(const char *in, size_t len, char *out, size_t size)
{
	size_t i, o = 0;
	for (i = 0; i < len; i++)
	{
		unsigned char c = (unsigned char)in[i];
		if (o + 1 >= size)
			return -1;
		if (c == '%')
		{
			int h, l;
			if (i + 2 >= len)
				return -1;
			h = tlua_hexval(in[i + 1]);
			l = tlua_hexval(in[i + 2]);
			if (h < 0 || l < 0)
				return -1;
			out[o++] = (char)(h * 16 + l);
			i += 2;
		}
		else if (c > 32 && c < 127 && c != '"' && c != ';' && c != '$' && c != '\\')
			out[o++] = (char)c;
		else
			return -1;
	}
	out[o] = 0;
	return (long)o;
}

// ---------------------------------------------------------------------------
// Subscriptions: channel:on(fn) in either realm

typedef struct tlua_sub_s { qbool used; tlua_realm_t *realm; char mod[TLUA_NAME_LEN + 1]; char channel[33]; int ref; } tlua_sub_t;
static tlua_sub_t tlua_subs[TLUA_CHAN_MAXSUBS];
static char tlua_key_chanmt;

typedef struct tlua_chanmsg_s { int player; const char *json; size_t len; } tlua_chanmsg_t;
static int tlua_chanmsg_push(lua_State *L, void *ud)
{
	tlua_chanmsg_t *c = (tlua_chanmsg_t *)ud;
	int n = 0;
	if (!TLua_JsonDecode(L, c->json, c->len))
		lua_pushnil(L);
	n++;
	if (c->player)
	{
		// the server: fn(player, value); the client: fn(value)
		lua_pushinteger(L, c->player);
		lua_insert(L, -2);
		n++;
	}
	return n;
}

static void tlua_chan_deliver(tlua_realm_t *r, const char *channel, int player, const char *json, size_t len)
{
	int i;
	tlua_chanmsg_t c;
	c.player = player;
	c.json = json;
	c.len = len;
	for (i = 0; i < TLUA_CHAN_MAXSUBS; i++)
	{
		tlua_sub_t *s = &tlua_subs[i];
		tlua_mod_t *m;
		if (!s->used || s->realm != r || strcmp(s->channel, channel))
			continue;
		m = TLua_Find(r, s->mod);
		if (m && m->state == TLUA_RUNNING)
			TLua_CallRef(m, s->ref, tlua_chanmsg_push, &c, NULL);
	}
}

// ---------------------------------------------------------------------------
// The server side

static double tlua_sv_tok[MAX_SCOREBOARD + 1], tlua_sv_tokt[MAX_SCOREBOARD + 1];   // to a player
static double tlua_cl_tok[MAX_SCOREBOARD + 1], tlua_cl_tokt[MAX_SCOREBOARD + 1];   // from a player
static double tlua_pg_tok[MAX_SCOREBOARD + 1], tlua_pg_tokt[MAX_SCOREBOARD + 1];   // page answers
static double tlua_ps_tok[MAX_SCOREBOARD + 1], tlua_ps_tokt[MAX_SCOREBOARD + 1];   // pages to a player
static qbool tlua_sv_capable[MAX_SCOREBOARD + 1];     // answered our hello
static const void *tlua_sv_helloed[MAX_SCOREBOARD + 1]; // the connection we said hello to

static qbool tlua_take(double *tok, double *tokt, int slot, double rate, double burst)
{
	double dt = host.realtime - tokt[slot];
	tokt[slot] = host.realtime;
	if (dt < 0)
		dt = 0;
	tok[slot] = min(burst, tok[slot] + dt * rate);
	if (tok[slot] < 1)
		return false;
	tok[slot] -= 1;
	return true;
}

static qbool tlua_sv_frame_to(int slot, const char *text, qbool limited)
{
	client_t *c;
	sizebuf_t *msg;
	if (!sv.active || slot < 1 || slot > svs.maxclients || slot > MAX_SCOREBOARD)
		return false;
	c = svs.clients + slot - 1;
	if (!c->active || !c->netconnection || !tlua_sv_capable[slot])
		return false;
	msg = &c->netconnection->message;
	if (msg->cursize + (int)strlen(text) + 64 > msg->maxsize)
		return false;
	if (limited && !tlua_take(tlua_sv_tok, tlua_sv_tokt, slot, 20, 40))
		return false;
	MSG_WriteChar(msg, svc_stufftext);
	MSG_WriteString(msg, text);
	return true;
}

void TLua_Chan_ServerFrame(void);
void TLua_Chan_ServerFrame(void)
{
	int i;
	if (!sv.active || !tlua_server.loaded)
		return;
	for (i = 0; i < svs.maxclients && i < MAX_SCOREBOARD; i++)
	{
		client_t *c = svs.clients + i;
		int slot = i + 1;
		if (!c->active || !c->netconnection || !c->begun)
		{
			tlua_sv_helloed[slot] = NULL;
			tlua_sv_capable[slot] = false;
			continue;
		}
		if (tlua_sv_helloed[slot] != c->netconnection)
		{
			sizebuf_t *msg = &c->netconnection->message;
			char text[64];
			tlua_sv_helloed[slot] = c->netconnection;
			tlua_sv_capable[slot] = false;
			dpsnprintf(text, sizeof(text), "//xt%dh %d\n", TLUA_CHAN_VERSION, TLUA_CHAN_VERSION);
			if (msg->cursize + 80 < msg->maxsize)
			{
				MSG_WriteChar(msg, svc_stufftext);
				MSG_WriteString(msg, text);
			}
		}
	}
}

// xthello / xtmsg / xtpage: stringcmds a client sends (CF_SERVER_FROM_CLIENT)
static int tlua_hostslot(cmd_state_t *cmd)
{
	int slot;
	if (cmd->source != src_client || !host_client || !sv.active)
		return 0;
	slot = (int)(host_client - svs.clients) + 1;
	return slot >= 1 && slot <= MAX_SCOREBOARD ? slot : 0;
}

static void tlua_xthello_f(cmd_state_t *cmd)
{
	int slot = tlua_hostslot(cmd);
	if (slot && Cmd_Argc(cmd) >= 2 && atoi(Cmd_Argv(cmd, 1)) >= 1)
		tlua_sv_capable[slot] = true;
}

static void tlua_xtmsg_f(cmd_state_t *cmd)
{
	static char json[TLUA_CHAN_MAXVALUE + 1];
	int slot = tlua_hostslot(cmd);
	const char *ch, *enc;
	long n;
	if (!slot || Cmd_Argc(cmd) != 3 || TLua_Busy())
		return;
	ch = Cmd_Argv(cmd, 1);
	enc = Cmd_Argv(cmd, 2);
	if (!tlua_chan_name_ok(ch, 32) || strlen(enc) > TLUA_CHAN_MAXVALUE * 3)
		return;
	if (!tlua_take(tlua_cl_tok, tlua_cl_tokt, slot, 5, 10))
		return;
	n = tlua_chan_decode(enc, strlen(enc), json, sizeof(json));
	if (n < 0)
		return;
	tlua_chan_deliver(&tlua_server, ch, slot, json, (size_t)n);
}

static void tlua_xtpage_f(cmd_state_t *cmd)
{
	static char value[512];
	int slot = tlua_hostslot(cmd);
	const char *page, *row, *ev;
	if (!slot || Cmd_Argc(cmd) != 5 || TLua_Busy())
		return;
	page = Cmd_Argv(cmd, 1);
	row = Cmd_Argv(cmd, 2);
	ev = Cmd_Argv(cmd, 3);
	if (!tlua_chan_name_ok(page, 80) || !tlua_chan_name_ok(row, 32) || (strcmp(ev, "change") && strcmp(ev, "press") && strcmp(ev, "select")))
		return;
	if (!tlua_take(tlua_pg_tok, tlua_pg_tokt, slot, 20, 20))
		return;
	if (tlua_chan_decode(Cmd_Argv(cmd, 4), strlen(Cmd_Argv(cmd, 4)), value, sizeof(value)) < 0)
		return;
	TLua_UI_ServerReply(slot, page, row, ev, value);
}

qbool TLua_Chan_SendPage(int player, const char *pageid, const char *json)
{
	static char enc[TLUA_CHAN_MAXPAGE * 3 + 1];
	char frame[TLUA_CHAN_CHUNK + 200];
	size_t len = strlen(json), n, off;
	int parts, i;
	client_t *c;
	if (len > TLUA_CHAN_MAXPAGE || player < 1 || player > svs.maxclients || player > MAX_SCOREBOARD || !tlua_sv_capable[player])
		return false;
	n = tlua_chan_encode(json, len, enc, sizeof(enc));
	if (!n)
		return false;
	parts = (int)((n + TLUA_CHAN_CHUNK - 1) / TLUA_CHAN_CHUNK);
	if (parts > 64)
		return false;
	c = svs.clients + player - 1;
	if (!c->netconnection || c->netconnection->message.cursize + (int)n + parts * 64 + 256 > c->netconnection->message.maxsize)
		return false;
	if (!tlua_take(tlua_ps_tok, tlua_ps_tokt, player, 2, 2))
		return false;
	for (i = 0, off = 0; i < parts; i++, off += TLUA_CHAN_CHUNK)
	{
		size_t k = min((size_t)TLUA_CHAN_CHUNK, n - off);
		dpsnprintf(frame, sizeof(frame), "//xt%dp %s %d %d %.*s\n", TLUA_CHAN_VERSION, pageid, i, parts, (int)k, enc + off);
		tlua_sv_frame_to(player, frame, false);
	}
	return true;
}

qbool TLua_Chan_ClosePage(int player, const char *pageid)
{
	char frame[160];
	dpsnprintf(frame, sizeof(frame), "//xt%dc %s\n", TLUA_CHAN_VERSION, pageid);
	return tlua_sv_frame_to(player, frame, false);
}

// ---------------------------------------------------------------------------
// The client side

static qbool tlua_cl_capable;           // the server said hello
static char tlua_cl_blocked[4096];      // "server channel" lines (channel "*": all)
static qbool tlua_cl_blocked_loaded;
static double tlua_cl_out, tlua_cl_outt; // requests to the server
static double tlua_cl_in, tlua_cl_int;   // messages from it
static double tlua_cl_pg, tlua_cl_pgt;   // page answers

// the page being received: parts in order
static char tlua_cl_page_id[96];
static int tlua_cl_page_next, tlua_cl_page_parts;
static char tlua_cl_page_buf[TLUA_CHAN_MAXPAGE * 3 + 1];
static size_t tlua_cl_page_len;

static const char *tlua_cl_server(void)
{
	if (sv.active)
		return "local";
	return cls.netcon ? cls.netcon->address : "";
}

static void tlua_cl_blocked_path(char *out, size_t size)
{
	TLua_StateDir(&tlua_client, out, size);
}

static void tlua_cl_blocked_load(void)
{
	char path[1100], dir[1024];
	tlua_cl_blocked_loaded = true;
	tlua_cl_blocked[0] = 0;
	tlua_cl_blocked_path(dir, sizeof(dir));
	dpsnprintf(path, sizeof(path), "%schannels_blocked.txt", dir);
	if (TLua_ReadState(path, tlua_cl_blocked, sizeof(tlua_cl_blocked)) < 0)
		tlua_cl_blocked[0] = 0;
}

static qbool tlua_cl_is_blocked(const char *channel)
{
	const char *line;
	const char *server = tlua_cl_server();
	if (!tlua_cl_blocked_loaded)
		tlua_cl_blocked_load();
	for (line = tlua_cl_blocked; *line; )
	{
		char srv[128] = "", ch[64] = "";
		const char *e = strchr(line, '\n');
		char one[200];
		size_t n = e ? (size_t)(e - line) : strlen(line);
		TLua_CopyN(one, sizeof(one), line, n);
		if (sscanf(one, "%127s %63s", srv, ch) >= 1)
			if ((!strcmp(srv, "*") || !strcmp(srv, server)) && (!ch[0] || !strcmp(ch, "*") || !strcmp(ch, channel)))
				return true;
		line = e ? e + 1 : line + n;
	}
	return false;
}

static void tlua_cl_send(const char *text)
{
	if (cls.state != ca_connected || !cls.netcon || cls.demoplayback)
		return;
	MSG_WriteByte(&cls.netcon->message, clc_stringcmd);
	MSG_WriteString(&cls.netcon->message, text);
}

// csprogs.c, CL_VM_Parse_StuffCmd: a line from the server; true when it was
// one of ours (then it never reaches the console)
qbool TouchLua_CL_StuffCmd(const char *msg, size_t len)
{
	static char json[TLUA_CHAN_MAXVALUE + 1];
	char a[100], b[100], c[16], d[16];
	const char *rest;
	if (len < 6 || strncmp(msg, "//xt", 4) || msg[4] < '1' || msg[4] > '9')
		return false;
	if (len > sizeof(tlua_cl_page_buf) || (const char *)memchr(msg, '\n', len) != msg + len - 1)
		return true; // ours, but malformed (or several lines): dropped
	if (msg[4] != '0' + TLUA_CHAN_VERSION)
		return true; // a newer protocol: ignored
	if (!cl_lua_channels.integer || !tlua_client.loaded)
		return true;
	switch (msg[5])
	{
	case 'h':
		if (!tlua_cl_capable)
		{
			char text[32];
			tlua_cl_capable = true;
			dpsnprintf(text, sizeof(text), "xthello %d", TLUA_CHAN_VERSION);
			tlua_cl_send(text);
		}
		return true;
	case 'm':
	{
		long n;
		const char *sp, *end = msg + len - 1;
		if (msg[6] != ' ')
			return true;
		sp = (const char *)memchr(msg + 7, ' ', (size_t)(end - (msg + 7)));
		if (!sp || (size_t)(sp - (msg + 7)) >= sizeof(a))
			return true;
		memcpy(a, msg + 7, (size_t)(sp - (msg + 7)));
		a[sp - (msg + 7)] = 0;
		rest = sp + 1;
		if (!tlua_chan_name_ok(a, 32) || (size_t)(end - rest) > TLUA_CHAN_MAXVALUE * 3)
			return true;
		if (!tlua_take(&tlua_cl_in, &tlua_cl_int, 0, 60, 60) || tlua_cl_is_blocked(a))
			return true;
		n = tlua_chan_decode(rest, (size_t)(end - rest), json, sizeof(json));
		if (n >= 0 && !TLua_Busy())
			tlua_chan_deliver(&tlua_client, a, 0, json, (size_t)n);
		return true;
	}
	case 'p':
	{
		int part, parts;
		const char *data;
		if (sscanf(msg + 6, " %95s %15s %15s", a, c, d) != 3 || !tlua_chan_name_ok(a, 80))
			return true;
		part = atoi(c);
		parts = atoi(d);
		data = msg + 7 + strlen(a) + 1 + strlen(c) + 1 + strlen(d) + 1;
		if (data >= msg + len || parts < 1 || parts > 64 || part < 0 || part >= parts)
			return true;
		if (part == 0)
		{
			dp_strlcpy(tlua_cl_page_id, a, sizeof(tlua_cl_page_id));
			tlua_cl_page_next = 0;
			tlua_cl_page_parts = parts;
			tlua_cl_page_len = 0;
		}
		if (strcmp(a, tlua_cl_page_id) || part != tlua_cl_page_next || parts != tlua_cl_page_parts)
			return true; // out of order: the whole page is dropped
		{
			size_t k = (size_t)(msg + len - 1 - data);
			if (tlua_cl_page_len + k >= sizeof(tlua_cl_page_buf) || k > TLUA_CHAN_CHUNK)
			{
				tlua_cl_page_id[0] = 0;
				return true;
			}
			memcpy(tlua_cl_page_buf + tlua_cl_page_len, data, k);
			tlua_cl_page_len += k;
			tlua_cl_page_buf[tlua_cl_page_len] = 0;
		}
		if (++tlua_cl_page_next == parts)
		{
			static char pagejson[TLUA_CHAN_MAXPAGE + 1];
			long n = tlua_chan_decode(tlua_cl_page_buf, tlua_cl_page_len, pagejson, sizeof(pagejson));
			char mod[64];
			const char *dot = strchr(a, '.');
			size_t ml = min(sizeof(mod) - 1, dot ? (size_t)(dot - a) : strlen(a));
			memcpy(mod, a, ml);
			mod[ml] = 0;
			tlua_cl_page_id[0] = 0;
			if (n > 0 && cl_lua_server_pages.integer && !tlua_cl_is_blocked("pages"))
				TLua_UI_ServerPage(a, pagejson, mod);
		}
		return true;
	}
	case 'c':
		if (sscanf(msg + 6, " %95s", b) == 1 && tlua_chan_name_ok(b, 96))
			TLua_UI_ServerPageClose(b);
		return true;
	}
	return true;
}

qbool TLua_Chan_PageReply(const char *pageid, const char *row, const char *event, const char *value);
qbool TLua_Chan_PageReply(const char *pageid, const char *row, const char *event, const char *value)
{
	char enc[1600], text[2000];
	if (!tlua_cl_capable || !tlua_chan_name_ok(pageid, 80) || strlen(value) > 500)
		return false;
	if (!tlua_take(&tlua_cl_pg, &tlua_cl_pgt, 0, 20, 20))
		return false;
	if (value[0] && !tlua_chan_encode(value, strlen(value), enc, sizeof(enc)))
		return false;
	if (!value[0])
		dp_strlcpy(enc, "%00", sizeof(enc)); // an empty value still is one token
	dpsnprintf(text, sizeof(text), "xtpage %s %s %s %s", pageid, row, event, enc);
	tlua_cl_send(text);
	return true;
}

// ---------------------------------------------------------------------------
// Lua API: xt.channel(name) -> channel; channel:send(...), channel:on(fn)

static const char *tlua_chan_self(lua_State *L)
{
	const char *name;
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_getfield(L, 1, "name");
	name = lua_tostring(L, -1);
	if (!name)
		luaL_error(L, "not a channel");
	lua_pop(L, 1);
	return name;
}

static int tlua_chan_on(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	const char *name = tlua_chan_self(L);
	int i, mine = 0;
	tlua_sub_t *free_s = NULL;
	luaL_checktype(L, 2, LUA_TFUNCTION);
	for (i = 0; i < TLUA_CHAN_MAXSUBS; i++)
	{
		tlua_sub_t *s = &tlua_subs[i];
		if (s->used && s->realm == m->realm && !strcmp(s->mod, m->name))
			mine++;
		else if (!s->used && !free_s)
			free_s = s;
	}
	if (mine >= 16 || !free_s)
		return luaL_error(L, "channel:on: too many handlers (16 per mod)");
	free_s->used = true;
	free_s->realm = m->realm;
	dp_strlcpy(free_s->mod, m->name, sizeof(free_s->mod));
	dp_strlcpy(free_s->channel, name, sizeof(free_s->channel));
	free_s->ref = TLua_Ref(L, 2);
	return 0;
}

static int tlua_chan_send(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	const char *name = tlua_chan_self(L);
	static char json[TLUA_CHAN_MAXVALUE * 2];
	char enc[TLUA_CHAN_MAXVALUE * 3 + 1], frame[TLUA_CHAN_MAXVALUE * 3 + 64];
	size_t n;
	int sent = 0;
	if (m->realm == &tlua_server)
	{
		// channel:send(player | nil for everyone, value)
		int target = lua_isnoneornil(L, 2) ? 0 : (int)luaL_checkinteger(L, 2);
		luaL_checkany(L, 3);
		n = TLua_JsonEncode(L, 3, json, sizeof(json));
		if (!n || n > TLUA_CHAN_MAXVALUE)
			return luaL_error(L, "channel:send: the value is over %d bytes as JSON", TLUA_CHAN_MAXVALUE);
		if (!tlua_chan_encode(json, n, enc, sizeof(enc)))
			return luaL_error(L, "channel:send: too long");
		dpsnprintf(frame, sizeof(frame), "//xt%dm %s %s\n", TLUA_CHAN_VERSION, name, enc);
		if (target)
			sent = tlua_sv_frame_to(target, frame, true);
		else
		{
			int i;
			for (i = 1; i <= svs.maxclients && i <= MAX_SCOREBOARD; i++)
				sent += tlua_sv_frame_to(i, frame, true);
		}
		lua_pushinteger(L, sent);
		return 1;
	}
	// a client mod: channel:send(value) to the server
	luaL_checkany(L, 2);
	n = TLua_JsonEncode(L, 2, json, sizeof(json));
	if (!n || n > TLUA_CHAN_MAXVALUE)
		return luaL_error(L, "channel:send: the value is over %d bytes as JSON", TLUA_CHAN_MAXVALUE);
	if (!tlua_cl_capable || !cl_lua_channels.integer || tlua_cl_is_blocked(name) || !tlua_take(&tlua_cl_out, &tlua_cl_outt, 0, 5, 10))
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	if (!tlua_chan_encode(json, n, enc, sizeof(enc)))
		return luaL_error(L, "channel:send: too long");
	dpsnprintf(frame, sizeof(frame), "xtmsg %s %s", name, enc);
	tlua_cl_send(frame);
	lua_pushboolean(L, 1);
	return 1;
}

static int tlua_chan_ready(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	(void)tlua_chan_self(L);
	lua_pushboolean(L, m->realm == &tlua_server ? 1 : tlua_cl_capable && cl_lua_channels.integer);
	return 1;
}

int TLuaAPI_channel(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	if (!tlua_chan_name_ok(name, 32))
		return luaL_error(L, "xt.channel: a name is a-z 0-9 _ . @, up to 32 (\"mymod\", \"mymod.hud@1\")");
	lua_createtable(L, 0, 1);
	lua_pushstring(L, name);
	lua_setfield(L, -2, "name");
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_chanmt) != LUA_TTABLE)
	{
		lua_pop(L, 1);
		lua_newtable(L);
		lua_newtable(L);
		TLua_SetField(L, "send", tlua_chan_send);
		TLua_SetField(L, "on", tlua_chan_on);
		TLua_SetField(L, "ready", tlua_chan_ready);
		lua_setfield(L, -2, "__index");
		lua_pushliteral(L, "channel");
		lua_setfield(L, -2, "__metatable");
		lua_pushvalue(L, -1);
		lua_rawsetp(L, LUA_REGISTRYINDEX, &tlua_key_chanmt);
	}
	lua_setmetatable(L, -2);
	return 1;
}

// ---------------------------------------------------------------------------
// lua_channel_block / unblock / list

static void tlua_channel_block_f(cmd_state_t *cmd)
{
	char dir[1024], line[200], text[sizeof(tlua_cl_blocked) + 256];
	const char *srv, *ch;
	qbool block = !strcmp(Cmd_Argv(cmd, 0), "lua_channel_block");
	if (Cmd_Argc(cmd) < 2)
	{
		if (!tlua_cl_blocked_loaded)
			tlua_cl_blocked_load();
		Con_Printf("usage: %s <server address|current|*> [channel|pages|*]\nblocked now:\n%s", Cmd_Argv(cmd, 0), tlua_cl_blocked[0] ? tlua_cl_blocked : "  nothing\n");
		return;
	}
	if (TLua_RemoteRefused(cmd))
		return;
	srv = !strcmp(Cmd_Argv(cmd, 1), "current") ? tlua_cl_server() : Cmd_Argv(cmd, 1);
	ch = Cmd_Argc(cmd) >= 3 ? Cmd_Argv(cmd, 2) : "*";
	if (!srv[0] || strpbrk(srv, " \t\r\n\"") || strlen(srv) > 120 || (strcmp(ch, "*") && strcmp(ch, "pages") && !tlua_chan_name_ok(ch, 32)))
	{
		Con_Printf("%s: bad server or channel\n", Cmd_Argv(cmd, 0));
		return;
	}
	if (!tlua_cl_blocked_loaded)
		tlua_cl_blocked_load();
	dpsnprintf(line, sizeof(line), "%s %s\n", srv, ch);
	text[0] = 0;
	{
		// rebuild without the line, then add it when blocking
		const char *p = tlua_cl_blocked;
		while (*p)
		{
			const char *e = strchr(p, '\n');
			size_t n = e ? (size_t)(e - p) + 1 : strlen(p);
			if ((strncmp(p, line, n) || n != strlen(line)) && strlen(text) + n < sizeof(text))
			{
				size_t t = strlen(text);
				memcpy(text + t, p, n);
				text[t + n] = 0;
			}
			p += n;
		}
	}
	if (block)
		dp_strlcat(text, line, sizeof(text));
	if (strlen(text) >= sizeof(tlua_cl_blocked))
	{
		Con_Printf("%s: the list is full\n", Cmd_Argv(cmd, 0));
		return;
	}
	tlua_cl_blocked_path(dir, sizeof(dir));
	if (!TLua_WriteState(dir, "channels_blocked.txt", text, true))
	{
		Con_Printf("%s: could not save\n", Cmd_Argv(cmd, 0));
		return;
	}
	dp_strlcpy(tlua_cl_blocked, text, sizeof(tlua_cl_blocked));
	Con_Printf("%s %s %s\n", block ? "blocked" : "unblocked", srv, ch);
}

// ---------------------------------------------------------------------------

void TLua_Chan_Frame(void)
{
	if (cls.state != ca_connected)
	{
		tlua_cl_capable = false;
		tlua_cl_page_id[0] = 0;
	}
}

void TLua_Chan_ModStopped(tlua_mod_t *m)
{
	int i;
	for (i = 0; i < TLUA_CHAN_MAXSUBS; i++)
		if (tlua_subs[i].used && tlua_subs[i].realm == m->realm && !strcmp(tlua_subs[i].mod, m->name))
		{
			TLua_Unref(m, tlua_subs[i].ref);
			tlua_subs[i].used = false;
		}
}

void TLua_Chan_Setup(void)
{
	if (cls.state != ca_dedicated)
	{
		TLua_RegisterCvar(&cl_lua_channels);
		TLua_RegisterCvar(&cl_lua_server_pages);
		TLua_Guard(&cl_lua_channels);
		TLua_Guard(&cl_lua_server_pages);
		Cmd_AddCommand(CF_CLIENT, "lua_channel_block", tlua_channel_block_f, "refuse a server's mod messages to your mods: lua_channel_block <address|current|*> [channel|pages|*]");
		Cmd_AddCommand(CF_CLIENT, "lua_channel_unblock", tlua_channel_block_f, "undo lua_channel_block");
	}
	Cmd_AddCommand(CF_SERVER_FROM_CLIENT, "xthello", tlua_xthello_f, "a client's mods can talk to this server's mods (sent by the game)");
	Cmd_AddCommand(CF_SERVER_FROM_CLIENT, "xtmsg", tlua_xtmsg_f, "a message from a client mod to the server's mods (sent by the game)");
	Cmd_AddCommand(CF_SERVER_FROM_CLIENT, "xtpage", tlua_xtpage_f, "an answer to a page a server mod showed (sent by the game)");
}
