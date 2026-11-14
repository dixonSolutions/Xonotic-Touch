/*
Godnotic: the name of the server a join is for (touch_joinname.h).

Every screen of a join named the server by its address ("Joining
10.1.1.239:26150"). This file keeps, for the server being joined or played on,
its name, and the label every join screen shows: "<name> (<address>)", or the
address alone until a name is known.

Where the name comes from, first that answers:
  - the server list (serverlist_cache), when the server is in it: a join from
    Multiplayer has its name at once;
  - the server itself: a getinfo with a challenge of our own, answered with an
    infoResponse that carries its hostname. A typed address or a link gets its
    name a moment after the join starts (and still while it is being refused,
    so the Could not join dialog can name it). The answer is ours alone:
    netconn.c hands it here and drops it, so the server list never learns of
    a server the player only typed.

The name keeps until another server is joined, so a reconnect, a failed join
and the pause page name the same server. A local game clears it.

No hook of its own in an upstream file besides the infoResponse one: the frame
comes from TouchJoinFail_Frame (touch_joinfail.c), Init from TouchJoinFail_Init.
*/
#include "quakedef.h"
#include "netconn.h"
#include "utf8lib.h"
#include "touch_joinname.h"

static cvar_t touch_join_address = {CF_CLIENT | CF_READONLY, "_touch_join_address", "", "Godnotic: the address of the server being joined or played on (kept after a failed join; empty in a local game)"};
static cvar_t touch_join_name = {CF_CLIENT | CF_READONLY, "_touch_join_name", "", "Godnotic: that server's name, colour codes out, cut to 32 characters; empty while unknown"};
static cvar_t touch_join_label = {CF_CLIENT | CF_READONLY, "_touch_join_label", "", "Godnotic: how the join screens name that server: \"name (address)\", or the address while no name is known"};

#define JN_ASKS 4           // getinfo tries (a server blocks a second getinfo from one address within 1 s)
#define JN_ASK_EVERY 1.25   // seconds between them

static struct
{
	char address[128];  // LHNETADDRESS_ToString of the server
	lhnetaddress_t peer;
	char name[128];
	char token[32];     // the challenge of our getinfo
	qbool replied;
	int asks;
	double next_ask, next_list;
} tjn;

void TouchJoinName_Clean(const char *in, char *out, size_t outsize)
{
	char tmp[512], *s, *d;
	size_t chars, bytes;
	const char *ellipsis;
	if (!outsize)
		return;
	out[0] = 0;
	if (!in || !*in)
		return;
	COM_StringDecolorize(in, 0, tmp, sizeof(tmp), false);
	// one line, single spaces, nothing at either end
	for (s = d = tmp; *s; s++)
	{
		unsigned char c = (unsigned char)*s;
		if (c < 32 || c == 127)
			c = ' ';
		if (c == ' ' && (d == tmp || d[-1] == ' '))
			continue;
		*d++ = (char)c;
	}
	while (d > tmp && d[-1] == ' ')
		d--;
	*d = 0;
	chars = u8_strlen(tmp);
	if (chars <= TOUCHJOINNAME_MAXCHARS)
	{
		dp_strlcpy(out, tmp, outsize);
		return;
	}
	ellipsis = utf8_enable.integer ? "\xe2\x80\xa6" : "...";
	bytes = u8_bytelen(tmp, TOUCHJOINNAME_MAXCHARS - (utf8_enable.integer ? 1 : 3));
	while (bytes > 0 && tmp[bytes - 1] == ' ')
		bytes--;
	tmp[bytes] = 0;
	dpsnprintf(out, outsize, "%s%s", tmp, ellipsis);
}

const char *TouchJoinName_For(const char *addr)
{
	if (!addr || !*addr || strcmp(addr, tjn.address))
		return "";
	return tjn.name;
}

const char *TouchJoinName_Label(const char *addr, char *buf, size_t size)
{
	const char *name = TouchJoinName_For(addr);
	if (*name)
		dpsnprintf(buf, size, "%s (%s)", name, addr);
	else
		dp_strlcpy(buf, addr ? addr : "", size);
	return buf;
}

static void publish(void)
{
	char label[256];
	TouchJoinName_Label(tjn.address, label, sizeof(label));
	if (strcmp(touch_join_address.string, tjn.address))
		Cvar_SetQuick(&touch_join_address, tjn.address);
	if (strcmp(touch_join_name.string, tjn.name))
		Cvar_SetQuick(&touch_join_name, tjn.name);
	if (strcmp(touch_join_label.string, label))
		Cvar_SetQuick(&touch_join_label, label);
}

static void forget(void)
{
	if (!tjn.address[0] && !touch_join_label.string[0])
		return;
	memset(&tjn, 0, sizeof(tjn));
	publish();
}

// the server list's name for addr, when it has one
static qbool from_list(void)
{
#ifdef CONFIG_MENU
	unsigned i;
	for (i = 0; i < serverlist_cachecount; i++)
	{
		serverlist_info_t *info = &serverlist_cache[i].info;
		if (!info->name[0] || strcmp(info->cname, tjn.address))
			continue;
		TouchJoinName_Clean(info->name, tjn.name, sizeof(tjn.name));
		if (tjn.name[0])
			return true;
	}
#endif
	return false;
}

void TouchJoinName_Frame(void)
{
	char addr[128], ask[64];
	lhnetaddress_t *peer = NULL;
	lhnetsocket_t *sock = NULL;
	if (cls.state == ca_dedicated)
		return;
	if (sv.active || cls.demoplayback)
	{
		// a local game (hosted on LAN or not) or a demo: no server to name
		forget();
		return;
	}
	if (cls.connect_trying)
	{
		peer = &cls.connect_address;
		sock = cls.connect_mysocket;
	}
	else if (cls.state == ca_connected && cls.netcon)
	{
		peer = &cls.netcon->peeraddress;
		sock = cls.netcon->mysocket;
	}
	// not joining: the last server keeps its name (Could not join, a reconnect)
	if (!peer)
		return;
	if (LHNETADDRESS_GetAddressType(peer) == LHNETADDRESSTYPE_LOOP)
	{
		forget();
		return;
	}
	LHNETADDRESS_ToString(peer, addr, sizeof(addr), true);
	if (strcmp(addr, tjn.address))
	{
		memset(&tjn, 0, sizeof(tjn));
		dp_strlcpy(tjn.address, addr, sizeof(tjn.address));
		tjn.peer = *peer;
		dpsnprintf(tjn.token, sizeof(tjn.token), "gnjoin%08x", (unsigned)(rand() ^ (int)(Sys_DirtyTime() * 1000.0)));
		tjn.next_ask = tjn.next_list = host.realtime;
		publish();
	}
	if (tjn.name[0] || tjn.replied)
		return;
	if (host.realtime >= tjn.next_list)
	{
		tjn.next_list = host.realtime + 0.5;
		if (from_list())
		{
			publish();
			return;
		}
	}
	if (sock && tjn.asks < JN_ASKS && host.realtime >= tjn.next_ask)
	{
		tjn.asks++;
		tjn.next_ask = host.realtime + JN_ASK_EVERY;
		dpsnprintf(ask, sizeof(ask), "\377\377\377\377getinfo %s", tjn.token);
		NetConn_WriteString(sock, ask, &tjn.peer);
	}
}

qbool TouchJoinName_Connectionless(const char *string, int length, lhnetaddress_t *peeraddress)
{
	char value[64], host_name[512];
	if (!tjn.token[0] || length < 13 || memcmp(string, "infoResponse\x0A", 13))
		return false;
	if (!InfoString_GetValue(string + 13, "challenge", value, sizeof(value)) || strcmp(value, tjn.token))
		return false;
	// ours: never the server list's business, wherever it came from
	if (!peeraddress || LHNETADDRESS_Compare(peeraddress, &tjn.peer))
		return true;
	tjn.replied = true;
	if (!tjn.name[0] && InfoString_GetValue(string + 13, "hostname", host_name, sizeof(host_name)))
	{
		TouchJoinName_Clean(host_name, tjn.name, sizeof(tjn.name));
		if (tjn.name[0])
			Con_DPrintf("Join: %s is \"%s\"\n", tjn.address, tjn.name);
	}
	publish();
	return true;
}

void TouchJoinName_Init(void)
{
	Cvar_RegisterVariable(&touch_join_address);
	Cvar_RegisterVariable(&touch_join_name);
	Cvar_RegisterVariable(&touch_join_label);
}
