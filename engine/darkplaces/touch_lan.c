/*
Godnotic: LAN games. Hosting a listen server for the local network, finding
games there without a master server or a typed address, and an optional
join password. docs/LAN.md is the player's side of this.

Discovery. DarkPlaces asks the LAN with one getstatus to 255.255.255.255,
port 26000, once per refresh (ServerList_QueryList). That misses a lot:
a limited broadcast goes out of the default route's interface only, and fails
outright (ENETUNREACH) where there is no default route -- a phone on a Wi-Fi
with no internet, or one whose default route is mobile data; a game on any
port but 26000 is never found; and one lost Wi-Fi broadcast means an empty
list. TouchLAN_Broadcast adds, for each client socket:
  - a subnet-directed broadcast (192.168.1.255 and the like) on every up,
    non-loopback IPv4 interface, which the kernel routes out of that interface;
  - the ports in net_slist_lan_ports (26000-26004 by default: the host
    screen offers the next free port when 26000 is taken);
  - the same again net_slist_lan_repeats times, net_slist_lan_repeatdelay
    apart, each with a fresh timestamp so the ping stays right. A server's
    getstatus flood guard (one per second per address) drops the copies that
    arrive after one got through, so repeats cost the server nothing;
  - the servers that answered last time (net_slist_cache), so a known game
    is listed within one round trip instead of after the master servers'
    net_slist_timeout.
Replies are ordinary statusResponses: netconn.c lists any server that echoes
the query's timestamp, so nothing else changes on the receiving side. The
menu files a private-address server under the LAN category, first in the list.
net_slist_lan_only skips the master servers altogether.

IPv6 multicast is not used: the engine's address strings drop the scope of a
link-local address, so a game found at fe80::... could not be joined.

Hosting. The menu's Host screen (dialog_touch_lan.qc) starts an ordinary
listen server with sv_public 0: that answers getinfo/getstatus, which LAN
discovery needs (-1 would hide the game from it), and never registers with a
master server. Before it starts, _touch_lan_portcheck says whether the port is
free and which one is next. While hosting, touch_lan_addresses lists every
address others can join, touch_lan_players who is on, for the menu to show.

Password. Xonotic has no join password at all, so this adds one. The client
never sends the password: it answers the server's challenge (a random
string the server made for that client address, which DarkPlaces already
checks on connect) with base64(HMAC-MD4(key = password, challenge)), the
scheme of DarkPlaces' rcon_secure 2. The server compares it with its own, in
TouchLAN_RefuseJoin. A wrong or missing proof gets a reject whose reason starts
with a marker (touch_lan_logic.h) our client turns into the menu's password
prompt; a stock client prints the sentence after it. The client sends a proof
only to the server the password was typed for (_cl_touch_lan_password_server),
keeps it in a cvar that is never archived, never printed and never $-expanded
(CF_PRIVATE), and writes it to <userdir>/data/touch_lan_passwords.txt only
when the player ticks "remember".
*/

#include "quakedef.h"
#include "lhnet.h"
#include "hmac.h"
#include "mdfour.h"
#include "touch_lan.h"
#include "touch_lan_logic.h"

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define TOUCHLAN_SOCKETS 1
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/ioctl.h>
#else
#include <ifaddrs.h>
#endif
#endif

#ifdef __EMSCRIPTEN__
#define TOUCHLAN_AVAILABLE "0"
#else
#define TOUCHLAN_AVAILABLE "1"
#endif

// Server
static cvar_t sv_touch_lan_password = {CF_SERVER | CF_PRIVATE, "sv_touch_lan_password", "",
	"Password players need to join this server (Godnotic clients prompt for it); empty for none. Never archived. "
	"A shared secret: the client proves it knows it without sending it"};

// Client / menu
static cvar_t net_slist_lan_only = {CF_CLIENT | CF_ARCHIVE, "net_slist_lan_only", "0",
	"1: the server list looks for games on the local network only, without asking the master servers"};
static cvar_t net_slist_lan_ports = {CF_CLIENT | CF_ARCHIVE, "net_slist_lan_ports", "26000-26004",
	"ports the LAN search asks on (list or ranges, e.g. \"26000-26004 27500\")"};
static cvar_t net_slist_lan_repeats = {CF_CLIENT, "net_slist_lan_repeats", "2",
	"how many more times a LAN search is sent after the first (Wi-Fi drops broadcasts)"};
static cvar_t net_slist_lan_repeatdelay = {CF_CLIENT, "net_slist_lan_repeatdelay", "0.35",
	"seconds between the LAN search repeats"};
static cvar_t net_slist_cache = {CF_CLIENT | CF_ARCHIVE, "net_slist_cache", "1",
	"1: remember the servers that answered and ask them first on the next refresh, so they show at once"};
static cvar_t touch_lan_available = {CF_CLIENT | CF_READONLY, "touch_lan_available", TOUCHLAN_AVAILABLE,
	"1 when this build can host and find LAN games (0 in the browser: no UDP)"};
static cvar_t touch_lan_hosting = {CF_CLIENT | CF_READONLY, "touch_lan_hosting", "0",
	"1 while this game hosts a server others can join"};
static cvar_t touch_lan_port = {CF_CLIENT | CF_READONLY, "touch_lan_port", "0",
	"the UDP port the hosted game listens on"};
static cvar_t touch_lan_addresses = {CF_CLIENT | CF_READONLY, "touch_lan_addresses", "",
	"while hosting: the addresses (IPv4:port) others on the network can join"};
static cvar_t touch_lan_players = {CF_CLIENT | CF_READONLY, "touch_lan_players", "",
	"while hosting: who is in the game"};
static cvar_t touch_lan_playercount = {CF_CLIENT | CF_READONLY, "touch_lan_playercount", "0",
	"while hosting: humans in the game"};
static cvar_t touch_lan_portcheck = {CF_SHARED, "_touch_lan_portcheck", "0",
	"set to a port: _touch_lan_port_free and _touch_lan_port_suggest say at once whether it can be hosted on"};
static cvar_t touch_lan_port_free = {CF_SHARED | CF_READONLY, "_touch_lan_port_free", "1",
	"result of _touch_lan_portcheck: 1 the port is free (or already this game's)"};
static cvar_t touch_lan_port_suggest = {CF_SHARED | CF_READONLY, "_touch_lan_port_suggest", "26000",
	"result of _touch_lan_portcheck: the first free port from the one asked"};
static cvar_t touch_lan_haspw_query = {CF_CLIENT, "_touch_lan_haspw_query", "",
	"set to a server address: _touch_lan_haspw says at once whether a password is known for it"};
static cvar_t touch_lan_haspw = {CF_CLIENT | CF_READONLY, "_touch_lan_haspw", "0",
	"result of _touch_lan_haspw_query"};
static cvar_t cl_touch_lan_password = {CF_CLIENT | CF_PRIVATE, "_cl_touch_lan_password", "",
	"the password for _cl_touch_lan_password_server, typed in the join prompt; never archived"};
static cvar_t cl_touch_lan_password_server = {CF_CLIENT, "_cl_touch_lan_password_server", "",
	"the server _cl_touch_lan_password is for; no other server gets a proof of it"};
static cvar_t cl_touch_lan_password_remember = {CF_CLIENT | CF_ARCHIVE, "cl_touch_lan_password_remember", "0",
	"1: keep join passwords in data/touch_lan_passwords.txt (plain text) for next time"};

// The Host screen's settings (dialog_touch_lan.qc), with the defaults of the
// launcher's Host page (launcher/core/.../Launch.kt) so both start the same game.
static cvar_t touch_lan_hostname = {CF_CLIENT | CF_ARCHIVE, "touch_lan_hostname", "Godnotic server", "Host screen: the game's name in server lists"};
static cvar_t touch_lan_gametype = {CF_CLIENT | CF_ARCHIVE, "touch_lan_gametype", "dm", "Host screen: game mode (dm, tdm, ctf, ...)"};
static cvar_t touch_lan_map = {CF_CLIENT | CF_ARCHIVE, "touch_lan_map", "stormkeep", "Host screen: first map; empty for a random one, then the whole rotation"};
static cvar_t touch_lan_bots = {CF_CLIENT | CF_ARCHIVE, "touch_lan_bots", "4", "Host screen: bots"};
static cvar_t touch_lan_maxplayers = {CF_CLIENT | CF_ARCHIVE, "touch_lan_maxplayers", "8", "Host screen: player slots"};
static cvar_t touch_lan_host_port = {CF_CLIENT | CF_ARCHIVE, "touch_lan_host_port", "26000", "Host screen: UDP port"};
static cvar_t touch_lan_assist = {CF_CLIENT | CF_ARCHIVE, "touch_lan_assist", "7", "Host screen: the sv_touch_assist the game allows"};
static cvar_t touch_lan_host_password = {CF_CLIENT | CF_PRIVATE, "touch_lan_host_password", "",
	"Host screen: join password for the next hosted game; never archived (see touch_lan_host_password_remember)"};
static cvar_t touch_lan_host_password_remember = {CF_CLIENT | CF_ARCHIVE, "touch_lan_host_password_remember", "0",
	"1: keep the Host screen's password in data/touch_lan_passwords.txt (plain text)"};

static cvar_t touch_lan_host_password_set = {CF_CLIENT | CF_READONLY, "touch_lan_host_password_set", "0",
	"1 when the Host screen has a password (the menu cannot read the password itself)"};
static cvar_t touch_lan_locked = {CF_CLIENT | CF_READONLY, "touch_lan_locked", "0",
	"while hosting: 1 when joining needs the password"};

#define HOST_ENTRY "(host)" // the remembered host password's line; never a server address
#define PASSWORD_FILE "touch_lan_passwords.txt"
#define CACHE_FILE "touch_serverlist_cache.txt"
#define MAX_REMEMBERED 32
#define MAX_CACHED 128
#define MAX_PORTS 16
#define MAX_IFACES 16
#define PROOF_SIZE 32

typedef struct
{
	char address[128];
	char password[64];
}
remembered_t;
static remembered_t remembered[MAX_REMEMBERED];
static int numremembered = -1; // -1: not read yet
static qbool host_pw_ready; // past the config's own cvar sets (first frame)

// ---------------------------------------------------------------- helpers

static qbool same_server(const char *a, const lhnetaddress_t *peer)
{
	lhnetaddress_t addr;
	if (!a[0] || a[0] == '(' || !peer)
		return false;
	if (!LHNETADDRESS_FromString(&addr, a, 26000))
		return false;
	return !LHNETADDRESS_Compare(&addr, peer);
}

// base64(HMAC-MD4(password, challenge)), 22 characters.
static void password_proof(char *out, size_t size, const char *challenge, const char *password)
{
	unsigned char hash[24];
	memset(hash, 0, sizeof(hash));
	HMAC_MDFOUR_16BYTES(hash, (const unsigned char *)challenge, (int)strlen(challenge),
		(const unsigned char *)password, (int)strlen(password));
	base64_encode(hash, 16, sizeof(hash));
	dp_ustr2stp(out, size, (const char *)hash, 22);
}

static void read_remembered(void)
{
	qfile_t *f;
	char buf[MAX_REMEMBERED * 200], *line, *next;
	fs_offset_t len;
	if (numremembered >= 0)
		return;
	numremembered = 0;
	f = FS_OpenRealFile(PASSWORD_FILE, "rb", true);
	if (!f)
		return;
	len = FS_Read(f, buf, sizeof(buf) - 1);
	FS_Close(f);
	buf[len > 0 ? len : 0] = 0;
	for (line = buf; line && *line && numremembered < MAX_REMEMBERED; line = next)
	{
		char *tab;
		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		tab = strchr(line, '\t');
		if (!tab || tab == line)
			continue;
		*tab = 0;
		dp_strlcpy(remembered[numremembered].address, line, sizeof(remembered[0].address));
		dp_strlcpy(remembered[numremembered].password, tab + 1, sizeof(remembered[0].password));
		numremembered++;
	}
}

static void write_remembered(void)
{
	qfile_t *f;
	int i;
	f = FS_OpenRealFile(PASSWORD_FILE, "wb", true);
	if (!f)
		return;
	if (numremembered <= 0)
		FS_RemoveOnClose(f); // nothing left: no file at all
	for (i = 0; i < numremembered; i++)
		FS_Printf(f, "%s\t%s\n", remembered[i].address, remembered[i].password);
	FS_Close(f);
}

static remembered_t *find_remembered(const lhnetaddress_t *peer)
{
	int i;
	read_remembered();
	for (i = 0; i < numremembered; i++)
		if (same_server(remembered[i].address, peer))
			return &remembered[i];
	return NULL;
}

static void forget(const lhnetaddress_t *peer)
{
	remembered_t *r = find_remembered(peer);
	if (!r)
		return;
	*r = remembered[--numremembered];
	write_remembered();
}

static void remember(const char *address, const lhnetaddress_t *peer, const char *password)
{
	remembered_t *r = find_remembered(peer);
	if (!r)
	{
		if (numremembered >= MAX_REMEMBERED)
			memmove(&remembered[0], &remembered[1], sizeof(remembered[0]) * --numremembered);
		r = &remembered[numremembered++];
	}
	else if (!strcmp(r->password, password))
		return;
	dp_strlcpy(r->address, address, sizeof(r->address));
	dp_strlcpy(r->password, password, sizeof(r->password));
	write_remembered();
}

// The Host screen's password: kept in the password file only while
// touch_lan_host_password_remember is on.
static void HostPassword_c(cvar_t *var)
{
	int i;
	if (touch_lan_host_password_set.integer != (touch_lan_host_password.string[0] != 0))
		Cvar_SetValueQuick(&touch_lan_host_password_set, touch_lan_host_password.string[0] != 0);
	if (!host_pw_ready)
		return;
	read_remembered();
	for (i = 0; i < numremembered; i++)
		if (!strcmp(remembered[i].address, HOST_ENTRY))
			break;
	if (touch_lan_host_password_remember.integer && touch_lan_host_password.string[0])
	{
		if (i == numremembered)
		{
			if (numremembered >= MAX_REMEMBERED)
				return;
			numremembered++;
		}
		else if (!strcmp(remembered[i].password, touch_lan_host_password.string))
			return;
		dp_strlcpy(remembered[i].address, HOST_ENTRY, sizeof(remembered[i].address));
		dp_strlcpy(remembered[i].password, touch_lan_host_password.string, sizeof(remembered[i].password));
		write_remembered();
	}
	else if (i < numremembered)
	{
		remembered[i] = remembered[--numremembered];
		write_remembered();
	}
	(void)var;
}

static void load_host_password(void)
{
	int i;
	read_remembered();
	for (i = 0; i < numremembered; i++)
		if (!strcmp(remembered[i].address, HOST_ENTRY))
			Cvar_SetQuick(&touch_lan_host_password, remembered[i].password);
}

// The password for this server: the one just typed for it, else a remembered one.
static const char *password_for(const lhnetaddress_t *peer)
{
	remembered_t *r;
	if (cl_touch_lan_password.string[0] && same_server(cl_touch_lan_password_server.string, peer))
		return cl_touch_lan_password.string;
	r = find_remembered(peer);
	return r ? r->password : NULL;
}

// Up, non-loopback IPv4 interfaces: address and directed broadcast (host order).
static int lan_interfaces(uint32_t *addr, uint32_t *bcast, int max)
{
	int n = 0;
#if defined(TOUCHLAN_SOCKETS) && defined(__linux__)
	// SIOCGIFCONF rather than getifaddrs: the latter is Android API 24+, and
	// this needs no netlink socket (sandboxes and old Android are happy).
	struct ifconf ifc;
	struct ifreq reqs[32];
	int fd = socket(AF_INET, SOCK_DGRAM, 0), i, count;
	if (fd < 0)
		return 0;
	memset(&ifc, 0, sizeof(ifc));
	ifc.ifc_len = sizeof(reqs);
	ifc.ifc_req = reqs;
	if (ioctl(fd, SIOCGIFCONF, &ifc) < 0)
	{
		close(fd);
		return 0;
	}
	count = ifc.ifc_len / (int)sizeof(struct ifreq);
	for (i = 0; i < count && n < max; i++)
	{
		struct ifreq r;
		uint32_t a, m, b;
		if (reqs[i].ifr_addr.sa_family != AF_INET)
			continue;
		a = ntohl(((struct sockaddr_in *)&reqs[i].ifr_addr)->sin_addr.s_addr);
		memset(&r, 0, sizeof(r));
		memcpy(r.ifr_name, reqs[i].ifr_name, sizeof(r.ifr_name));
		if (ioctl(fd, SIOCGIFFLAGS, &r) < 0 || !(r.ifr_flags & IFF_UP) || (r.ifr_flags & IFF_LOOPBACK))
			continue;
		if (ioctl(fd, SIOCGIFNETMASK, &r) < 0)
			continue;
		m = ntohl(((struct sockaddr_in *)&r.ifr_netmask)->sin_addr.s_addr);
		b = TouchLAN_DirectedBroadcast(a, m);
		addr[n] = a;
		bcast[n] = b;
		n++;
	}
	close(fd);
#elif defined(TOUCHLAN_SOCKETS)
	struct ifaddrs *list, *it;
	if (getifaddrs(&list) < 0)
		return 0;
	for (it = list; it && n < max; it = it->ifa_next)
	{
		uint32_t a, m;
		if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET || !it->ifa_netmask)
			continue;
		if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK))
			continue;
		a = ntohl(((struct sockaddr_in *)it->ifa_addr)->sin_addr.s_addr);
		m = ntohl(((struct sockaddr_in *)it->ifa_netmask)->sin_addr.s_addr);
		addr[n] = a;
		bcast[n] = TouchLAN_DirectedBroadcast(a, m);
		n++;
	}
	freeifaddrs(list);
#else
	(void)addr; (void)bcast; (void)max;
#endif
	return n;
}

#ifdef TOUCHLAN_SOCKETS
// Whether a UDP port can be bound on all IPv4 interfaces.
static qbool port_bindable(int port)
{
	struct sockaddr_in sa;
	int fd = socket(AF_INET, SOCK_DGRAM, 0), ok;
	if (fd < 0)
		return true; // cannot tell: let the engine try
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short)port);
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	ok = bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0;
	close(fd);
	return ok;
}
#endif

// The port the hosted game listens on, 0 when it has no network socket.
static int hosted_port(void)
{
	lhnetaddress_t any;
	lhnetsocket_t *s;
	if (!sv.active || svs.maxclients < 2)
		return 0;
	LHNETADDRESS_FromPort(&any, LHNETADDRESSTYPE_INET4, 0);
	s = NetConn_ChooseServerSocketForAddress(&any);
	return s ? LHNETADDRESS_GetPort(LHNET_AddressFromSocket(s)) : 0;
}

static void PortCheck_c(cvar_t *var)
{
	int port = var->integer, p;
	qbool free_now;
	if (port < 1 || port > 65535)
	{
		Cvar_SetValueQuick(&touch_lan_port_free, 0);
		Cvar_SetValueQuick(&touch_lan_port_suggest, 26000);
		return;
	}
#ifdef TOUCHLAN_SOCKETS
	free_now = port == hosted_port() || port_bindable(port);
	for (p = port; p < port + 100 && p <= 65535; p++)
		if (p == hosted_port() || port_bindable(p))
			break;
	if (p > 65535 || p >= port + 100)
		p = port;
#else
	free_now = true;
	p = port;
#endif
	Cvar_SetValueQuick(&touch_lan_port_free, free_now);
	Cvar_SetValueQuick(&touch_lan_port_suggest, p);
}

static void HasPw_c(cvar_t *var)
{
	lhnetaddress_t addr;
	qbool has = var->string[0] && LHNETADDRESS_FromString(&addr, var->string, 26000) && password_for(&addr);
	Cvar_SetValueQuick(&touch_lan_haspw, has);
}

// ---------------------------------------------------------------- password

void TouchLAN_AppendConnect(char *connect, size_t size, const char *challenge, const lhnetaddress_t *peer)
{
	const char *pw;
	char proof[PROOF_SIZE];
	size_t len;
	if (!challenge || !challenge[0] || !(pw = password_for(peer)))
		return;
	password_proof(proof, sizeof(proof), challenge, pw);
	len = strlen(connect);
	if (len + strlen("\\touch_pw\\") + strlen(proof) + 1 > size)
		return;
	dpsnprintf(connect + len, size - len, "\\touch_pw\\%s", proof);
	// The player ticked "remember": keep it for next time. A wrong one is
	// forgotten again when the server says so.
	if (cl_touch_lan_password_remember.integer && pw == cl_touch_lan_password.string)
		remember(cl_touch_lan_password_server.string, peer, pw);
}

void TouchLAN_Rejected(const char *reason, int length, const lhnetaddress_t *peer)
{
	char addr[128], cmd[256];
	int kind = TouchLAN_RejectKind(reason, length > 0 ? (size_t)length : 0);
	if (!kind)
		return;
	LHNETADDRESS_ToString(peer, addr, sizeof(addr), true);
	if (kind == 2)
	{
		forget(peer);
		if (same_server(cl_touch_lan_password_server.string, peer))
			Cvar_SetQuick(&cl_touch_lan_password, "");
	}
	dp_strlcpy(cl_connect_status, kind == 2 ? "Connect: wrong password" : "Connect: this game needs a password", sizeof(cl_connect_status));
#ifdef CONFIG_MENU
	// The menu's password prompt (dialog_touch_lan.qc) takes it from here.
	dpsnprintf(cmd, sizeof(cmd), "\ntogglemenu 1; menu_cmd directmenu TouchLanPassword %s \"%s\"\n", kind == 2 ? "wrong" : "required", addr);
	Cbuf_AddText(cmd_local, cmd);
#else
	(void)cmd;
#endif
}

qbool TouchLAN_RefuseJoin(lhnetsocket_t *sock, lhnetaddress_t *peer, const char *connect)
{
	char challenge[128], proof[PROOF_SIZE], expect[PROOF_SIZE], reply[160], addr[128];
	if (!sv_touch_lan_password.string[0] || LHNETADDRESS_GetAddressType(peer) == LHNETADDRESSTYPE_LOOP)
		return false;
	if (!InfoString_GetValue(connect, "challenge", challenge, sizeof(challenge)) || !challenge[0])
		challenge[0] = 0;
	if (!InfoString_GetValue(connect, "touch_pw", proof, sizeof(proof)) || !proof[0])
	{
		dpsnprintf(reply, sizeof(reply), "\377\377\377\377reject " TOUCHLAN_REJECT_REQUIRED "This game needs a password.");
		NetConn_WriteString(sock, reply, peer);
		return true;
	}
	password_proof(expect, sizeof(expect), challenge, sv_touch_lan_password.string);
	if (!challenge[0] || strcmp(proof, expect))
	{
		LHNETADDRESS_ToString(peer, addr, sizeof(addr), true);
		Con_Printf("Refused %s: wrong join password\n", addr);
		dpsnprintf(reply, sizeof(reply), "\377\377\377\377reject " TOUCHLAN_REJECT_WRONG "Wrong password.");
		NetConn_WriteString(sock, reply, peer);
		return true;
	}
	return false;
}

void TouchLAN_StatusFlags(char *qcstatus, size_t size)
{
	if (sv_touch_lan_password.string[0])
		TouchLAN_InsertQcstatusLock(qcstatus, size);
}

// ---------------------------------------------------------------- discovery

#ifdef CONFIG_MENU
static lhnetsocket_t *query_sockets[16];
static int num_query_sockets;
static int repeats_left;
static double next_repeat;
static qbool cache_queried;
static unsigned cache_saved_count;
static double cache_save_time;

static qbool socket_alive(lhnetsocket_t *s)
{
	lhnetsocket_t *it;
	List_For_Each_Entry(it, &lhnet_socketlist.list, lhnetsocket_t, list)
		if (it == s)
			return true;
	return false;
}

static void send_to(lhnetsocket_t *s, const char *query, uint32_t ip, int port)
{
	lhnetaddress_t to;
	char str[32];
	dpsnprintf(str, sizeof(str), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
	if (LHNETADDRESS_FromString(&to, str, port))
		NetConn_WriteString(s, query, &to);
}

static void lan_query(lhnetsocket_t *s, qbool first)
{
	uint32_t addr[MAX_IFACES], bcast[MAX_IFACES];
	int ports[MAX_PORTS], nports, nifaces, i, j;
	char query[64];
	if (LHNETADDRESS_GetAddressType(LHNET_AddressFromSocket(s)) != LHNETADDRESSTYPE_INET4)
		return;
	TouchLAN_BuildQuery(query, sizeof(query), host.realtime + (Sys_DirtyTime() - host.dirtytime));
	nports = TouchLAN_ParsePorts(net_slist_lan_ports.string, ports, MAX_PORTS);
	if (!nports)
		ports[nports++] = 26000;
	nifaces = lan_interfaces(addr, bcast, MAX_IFACES);
	for (j = 0; j < nports; j++)
	{
		// netconn.c sent 255.255.255.255:26000 itself the first time.
		if (!first || ports[j] != 26000)
			send_to(s, query, 0xffffffffu, ports[j]);
		for (i = 0; i < nifaces; i++)
			if (bcast[i])
				send_to(s, query, bcast[i], ports[j]);
	}
}

static void cache_query(lhnetsocket_t *s)
{
	qfile_t *f;
	char buf[MAX_CACHED * 64], *line, *next, query[64];
	fs_offset_t len;
	int n = 0;
	lhnetaddress_t to;
	if (!net_slist_cache.integer)
		return;
	f = FS_OpenRealFile(CACHE_FILE, "rb", true);
	if (!f)
		return;
	len = FS_Read(f, buf, sizeof(buf) - 1);
	FS_Close(f);
	buf[len > 0 ? len : 0] = 0;
	TouchLAN_BuildQuery(query, sizeof(query), host.realtime + (Sys_DirtyTime() - host.dirtytime));
	for (line = buf; line && *line && n < MAX_CACHED; line = next)
	{
		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		if (!line[0] || !LHNETADDRESS_FromString(&to, line, 26000))
			continue;
		if (LHNETADDRESS_GetAddressType(&to) != LHNETADDRESS_GetAddressType(LHNET_AddressFromSocket(s)))
			continue;
		if (net_slist_lan_only.integer)
		{
			// LAN only: the cached LAN games, not the internet ones.
			unsigned a, b, c, d;
			if (sscanf(line, "%u.%u.%u.%u", &a, &b, &c, &d) != 4
				|| !TouchLAN_IsLanIPv4(((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d))
				continue;
		}
		NetConn_WriteString(s, query, &to);
		n++;
	}
}

// Keep the addresses of the servers that answered, for the next session.
static void cache_save(void)
{
	qfile_t *f;
	unsigned i, n = 0;
	if (!net_slist_cache.integer || serverlist_cachecount == cache_saved_count)
		return;
	f = FS_OpenRealFile(CACHE_FILE, "wb", true);
	if (!f)
		return;
	for (i = 0; i < serverlist_cachecount && n < MAX_CACHED; i++)
		if (serverlist_cache[i].info.ping > 0 && serverlist_cache[i].info.cname[0])
		{
			FS_Printf(f, "%s\n", serverlist_cache[i].info.cname);
			n++;
		}
	FS_Close(f);
	cache_saved_count = serverlist_cachecount;
}

void TouchLAN_Broadcast(lhnetsocket_t *sock)
{
	int i;
	for (i = 0; i < num_query_sockets; i++)
		if (query_sockets[i] == sock)
			break;
	if (i == num_query_sockets && num_query_sockets < (int)(sizeof(query_sockets) / sizeof(query_sockets[0])))
		query_sockets[num_query_sockets++] = sock;
	lan_query(sock, true);
	if (!cache_queried)
		cache_query(sock);
	repeats_left = bound(0, net_slist_lan_repeats.integer, 10);
	next_repeat = host.realtime + bound(0.05, net_slist_lan_repeatdelay.value, 5);
}

static void repeat_frame(void)
{
	int i;
	if (num_query_sockets && !repeats_left)
	{
		cache_queried = true; // every socket has had its turn
		num_query_sockets = 0;
	}
	if (!repeats_left || host.realtime < next_repeat)
		return;
	for (i = 0; i < num_query_sockets; i++)
		if (socket_alive(query_sockets[i]))
			lan_query(query_sockets[i], false);
	repeats_left--;
	next_repeat = host.realtime + bound(0.05, net_slist_lan_repeatdelay.value, 5);
}
#else
void TouchLAN_Broadcast(lhnetsocket_t *sock) { (void)sock; }
#endif

qbool TouchLAN_LanOnly(void)
{
	return net_slist_lan_only.integer != 0;
}

// ---------------------------------------------------------------- hosting

static void host_status(void)
{
	char addrs[512], players[1024], one[160];
	uint32_t addr[MAX_IFACES], bcast[MAX_IFACES];
	int port = hosted_port(), n, i, humans = 0;
	addrs[0] = players[0] = 0;
	if (port)
	{
		n = lan_interfaces(addr, bcast, MAX_IFACES);
		for (i = 0; i < n; i++)
		{
			dpsnprintf(one, sizeof(one), "%s%u.%u.%u.%u:%d", addrs[0] ? " " : "",
				addr[i] >> 24, (addr[i] >> 16) & 255, (addr[i] >> 8) & 255, addr[i] & 255, port);
			dp_strlcat(addrs, one, sizeof(addrs));
		}
		for (i = 0; i < svs.maxclients; i++)
		{
			client_t *c = &svs.clients[i];
			if (!c->active)
				continue;
			if (c->netconnection)
			{
				humans++;
				if (LHNETADDRESS_GetAddressType(&c->netconnection->peeraddress) == LHNETADDRESSTYPE_LOOP)
					dpsnprintf(one, sizeof(one), "%s%s (you)", players[0] ? ", " : "", c->name);
				else
					dpsnprintf(one, sizeof(one), "%s%s (%d ms)", players[0] ? ", " : "", c->name, (int)floor(c->ping * 1000 + 0.5));
			}
			else
				dpsnprintf(one, sizeof(one), "%s%s (bot)", players[0] ? ", " : "", c->name);
			dp_strlcat(players, one, sizeof(players));
		}
	}
	if (touch_lan_hosting.integer != (port != 0))
		Cvar_SetValueQuick(&touch_lan_hosting, port != 0);
	if (touch_lan_port.integer != port)
		Cvar_SetValueQuick(&touch_lan_port, port);
	if (strcmp(touch_lan_addresses.string, addrs))
		Cvar_SetQuick(&touch_lan_addresses, addrs);
	if (strcmp(touch_lan_players.string, players))
		Cvar_SetQuick(&touch_lan_players, players);
	if (touch_lan_playercount.integer != humans)
		Cvar_SetValueQuick(&touch_lan_playercount, humans);
	if (touch_lan_locked.integer != (port && sv_touch_lan_password.string[0]))
		Cvar_SetValueQuick(&touch_lan_locked, port && sv_touch_lan_password.string[0]);
}

void TouchLAN_Frame(void)
{
	static double next_status;
	if (!host_pw_ready)
	{
		// The file system and config.cfg are in by the first frame.
		if (touch_lan_host_password_remember.integer)
			load_host_password();
		host_pw_ready = true;
	}
#ifdef CONFIG_MENU
	repeat_frame();
	if (host.realtime > cache_save_time)
	{
		cache_save();
		cache_save_time = host.realtime + 5;
	}
#endif
	if (host.realtime > next_status || (touch_lan_hosting.integer && !sv.active))
	{
		host_status();
		next_status = host.realtime + 0.5;
	}
}

// touch_lan_status: what others need to join (never the password).
static void TouchLAN_Status_f(cmd_state_t *cmd)
{
	host_status();
	if (!touch_lan_hosting.integer)
	{
		Con_Printf("Not hosting a game others can join.\n");
		return;
	}
	Con_Printf("Hosting on UDP port %d%s\n", touch_lan_port.integer, sv_touch_lan_password.string[0] ? ", password protected" : "");
	Con_Printf("Join at: %s\n", touch_lan_addresses.string[0] ? touch_lan_addresses.string : "(no network interface is up)");
	Con_Printf("Players: %s\n", touch_lan_players.string[0] ? touch_lan_players.string : "none");
}

// touch_lan_host_password_apply: the Host screen's password becomes the
// server's, right before its map command. The menu cannot read either (both
// are CF_PRIVATE, so a server's stuffcmd cannot $-expand them either).
static void TouchLAN_ApplyPassword_f(cmd_state_t *cmd)
{
	Cvar_SetQuick(&sv_touch_lan_password, touch_lan_host_password.string);
}

// touch_lan_forget [address]: forget remembered join passwords.
static void TouchLAN_Forget_f(cmd_state_t *cmd)
{
	lhnetaddress_t addr;
	read_remembered();
	if (Cmd_Argc(cmd) < 2)
	{
		numremembered = 0;
		write_remembered();
		Con_Printf("Forgot every remembered join password.\n");
		return;
	}
	if (LHNETADDRESS_FromString(&addr, Cmd_Argv(cmd, 1), 26000) && find_remembered(&addr))
	{
		forget(&addr);
		Con_Printf("Forgot the join password for %s.\n", Cmd_Argv(cmd, 1));
	}
	else
		Con_Printf("No join password remembered for %s.\n", Cmd_Argv(cmd, 1));
}

void TouchLAN_Init(void)
{
	cvar_t *v;
	Cvar_RegisterVariable(&sv_touch_lan_password);
	Cvar_RegisterVariable(&net_slist_lan_only);
	Cvar_RegisterVariable(&net_slist_lan_ports);
	Cvar_RegisterVariable(&net_slist_lan_repeats);
	Cvar_RegisterVariable(&net_slist_lan_repeatdelay);
	Cvar_RegisterVariable(&net_slist_cache);
	Cvar_RegisterVariable(&touch_lan_available);
	Cvar_RegisterVariable(&touch_lan_hosting);
	Cvar_RegisterVariable(&touch_lan_port);
	Cvar_RegisterVariable(&touch_lan_addresses);
	Cvar_RegisterVariable(&touch_lan_players);
	Cvar_RegisterVariable(&touch_lan_playercount);
	Cvar_RegisterVariable(&touch_lan_portcheck);
	Cvar_RegisterCallback(&touch_lan_portcheck, PortCheck_c);
	Cvar_RegisterVariable(&touch_lan_port_free);
	Cvar_RegisterVariable(&touch_lan_port_suggest);
	Cvar_RegisterVariable(&touch_lan_haspw_query);
	Cvar_RegisterCallback(&touch_lan_haspw_query, HasPw_c);
	Cvar_RegisterVariable(&touch_lan_haspw);
	Cvar_RegisterVariable(&cl_touch_lan_password);
	Cvar_RegisterVariable(&cl_touch_lan_password_server);
	Cvar_RegisterVariable(&cl_touch_lan_password_remember);
	Cvar_RegisterVariable(&touch_lan_hostname);
	Cvar_RegisterVariable(&touch_lan_gametype);
	Cvar_RegisterVariable(&touch_lan_map);
	Cvar_RegisterVariable(&touch_lan_bots);
	Cvar_RegisterVariable(&touch_lan_maxplayers);
	Cvar_RegisterVariable(&touch_lan_host_port);
	Cvar_RegisterVariable(&touch_lan_assist);
	Cvar_RegisterVariable(&touch_lan_host_password);
	Cvar_RegisterVariable(&touch_lan_host_password_remember);
	Cvar_RegisterVariable(&touch_lan_host_password_set);
	Cvar_RegisterVariable(&touch_lan_locked);
	Cvar_RegisterCallback(&touch_lan_host_password, HostPassword_c);
	Cvar_RegisterCallback(&touch_lan_host_password_remember, HostPassword_c);
	Cmd_AddCommand(CF_SHARED, "touch_lan_status", TouchLAN_Status_f, "show the addresses and players of the game this one hosts");
	Cmd_AddCommand(CF_CLIENT, "touch_lan_host_password_apply", TouchLAN_ApplyPassword_f, "make the Host screen's password (touch_lan_host_password) the server's join password");
	Cmd_AddCommand(CF_CLIENT, "touch_lan_forget", TouchLAN_Forget_f, "forget remembered join passwords: all, or the one for an address");

	// The master servers' answer is in long before 4 s, and DarkPlaces servers
	// echo a timestamp for the ping, so the stock wait before the list's own
	// queries only delays the list. Neither is archived.
	if ((v = Cvar_FindVar(&cvars_all, "net_slist_timeout", ~0)) && !strcmp(v->string, "4"))
		Cvar_SetQuick(v, "1.5");
	if ((v = Cvar_FindVar(&cvars_all, "net_slist_queriesperframe", ~0)) && !strcmp(v->string, "2"))
		Cvar_SetQuick(v, "8");
}
