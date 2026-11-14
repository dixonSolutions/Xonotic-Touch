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
#include "touch_nativefe.h"
#include "touch_lan_logic.h"
#include "touch_net.h"
#include "touch_joinfail.h"
#include "touch_srvlist.h"

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define TOUCHLAN_SOCKETS 1
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <unistd.h>
#include <sys/un.h>
#include <fcntl.h>
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
static cvar_t touch_lan_address_labels = {CF_CLIENT | CF_READONLY, "touch_lan_address_labels", "",
	"while hosting: each address others can join with what it is on, \"label|address\" separated by ';' (\"Wi-Fi|192.168.1.20:26000;VPN (tailscale0)|100.101.102.103:26000\")"};
static cvar_t touch_lan_join_link = {CF_CLIENT | CF_READONLY, "touch_lan_join_link", "",
	"while hosting: a link to share (godnotic://join/ADDRESS:PORT); lan_join and the Join by address box take it, or its ADDRESS:PORT"};
static cvar_t touch_lan_hosts = {CF_CLIENT | CF_READONLY, "touch_lan_hosts", "",
	"remembered LAN and VPN games (joined before or added with lan_addhost), newest first, space separated; every LAN search asks them"};
static cvar_t net_slist_lan_sweeps = {CF_CLIENT | CF_ARCHIVE, "net_slist_lan_sweeps", "2",
	"passes of unicast queries over a small routed VPN subnet (/22 to /30) per LAN search; 0 = none"};
static cvar_t net_slist_lan_sweeprate = {CF_CLIENT | CF_ARCHIVE, "net_slist_lan_sweeprate", "2000",
	"unicast LAN search queries sent per second at most"};
static cvar_t net_slist_lan_tailscale = {CF_CLIENT | CF_ARCHIVE, "net_slist_lan_tailscale", "1",
	"ask the Tailscale peers the local tailscaled lists (read only, desktop Linux) in a LAN search"};
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
static cvar_t touch_lan_private = {CF_SHARED | CF_ARCHIVE, "touch_lan_private", "1",
	"1: a game started on this device opens a network socket only when it is hosted on the LAN on purpose (Create with LAN ticked, the Host screen, touch_lan_arm); a local game then has the loopback socket alone and nobody else can find or join it. 0: every game listens on its port, as stock DarkPlaces does"};
static cvar_t touch_lan_popup_autoclose = {CF_CLIENT | CF_ARCHIVE, "touch_lan_popup_autoclose", "1",
	"the popup with the join addresses a LAN host sees closes by itself when the first player joins"};
static cvar_t touch_lan_ended = {CF_CLIENT, "_touch_lan_ended", "0",
	"1 after the host of the LAN game this client was in ended it; the menu shows \"The host ended the game\" and sets it back to 0"};
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
static qbool lan_host_armed;      // touch_lan_host_password_apply ran: the Host screen's map comes next
static qbool lan_host_deliberate; // the running server was started from the Host screen

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

// The up IPv4 interfaces others can reach this device on
// (TouchLAN_InterfaceKind): Wi-Fi, Ethernet and hotspot first, then the
// VPNs, then anything else; never loopback or a cellular modem's.
typedef struct lan_iface_s
{
	uint32_t addr, mask, bcast; // host order; bcast 0 = send no broadcast
	int kind;                   // TOUCHLAN_KIND_*
	int rank;
	int mtu;
	char name[16];
}
lan_iface_t;

#ifdef TOUCHLAN_SOCKETS
static void add_iface(lan_iface_t *list, int *n, const char *name, uint32_t a, uint32_t m, int flags, int mtu)
{
	lan_iface_t e;
	int at = *n;
	memset(&e, 0, sizeof(e));
	e.kind = TouchLAN_InterfaceKind(name, flags);
	e.rank = TouchLAN_InterfaceRank(name, flags);
	if (!e.rank || !a)
		return;
	e.addr = a;
	e.mask = m;
	// a routed VPN carries no broadcast: its peers get unicast probes
	e.bcast = e.kind == TOUCHLAN_KIND_VPN_L3 ? 0 : TouchLAN_DirectedBroadcast(a, m);
	e.mtu = mtu;
	dp_strlcpy(e.name, name ? name : "", sizeof(e.name));
	while (at > 0 && list[at - 1].rank < e.rank)
	{
		list[at] = list[at - 1];
		at--;
	}
	list[at] = e;
	(*n)++;
}
#endif

static int lan_interfaces(lan_iface_t *list, int max)
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
	for (i = 0; i < count && n < max && n < MAX_IFACES; i++)
	{
		struct ifreq r;
		uint32_t a, m;
		int flags, mtu = 0;
		if (reqs[i].ifr_addr.sa_family != AF_INET)
			continue;
		a = ntohl(((struct sockaddr_in *)&reqs[i].ifr_addr)->sin_addr.s_addr);
		memset(&r, 0, sizeof(r));
		memcpy(r.ifr_name, reqs[i].ifr_name, sizeof(r.ifr_name));
		if (ioctl(fd, SIOCGIFFLAGS, &r) < 0)
			continue;
		flags = ((r.ifr_flags & IFF_UP) ? TOUCHLAN_IF_UP : 0) | ((r.ifr_flags & IFF_LOOPBACK) ? TOUCHLAN_IF_LOOPBACK : 0)
			| ((r.ifr_flags & IFF_POINTOPOINT) ? TOUCHLAN_IF_POINTOPOINT : 0) | ((r.ifr_flags & IFF_BROADCAST) ? TOUCHLAN_IF_BROADCAST : 0);
		if (ioctl(fd, SIOCGIFNETMASK, &r) < 0)
			continue;
		m = ntohl(((struct sockaddr_in *)&r.ifr_netmask)->sin_addr.s_addr);
		if (ioctl(fd, SIOCGIFMTU, &r) == 0)
			mtu = r.ifr_mtu;
		add_iface(list, &n, reqs[i].ifr_name, a, m, flags, mtu);
	}
	close(fd);
#elif defined(TOUCHLAN_SOCKETS)
	struct ifaddrs *all, *it;
	if (getifaddrs(&all) < 0)
		return 0;
	for (it = all; it && n < max && n < MAX_IFACES; it = it->ifa_next)
	{
		int flags;
		if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET || !it->ifa_netmask)
			continue;
		flags = ((it->ifa_flags & IFF_UP) ? TOUCHLAN_IF_UP : 0) | ((it->ifa_flags & IFF_LOOPBACK) ? TOUCHLAN_IF_LOOPBACK : 0)
			| ((it->ifa_flags & IFF_POINTOPOINT) ? TOUCHLAN_IF_POINTOPOINT : 0) | ((it->ifa_flags & IFF_BROADCAST) ? TOUCHLAN_IF_BROADCAST : 0);
		add_iface(list, &n, it->ifa_name, ntohl(((struct sockaddr_in *)it->ifa_addr)->sin_addr.s_addr),
			ntohl(((struct sockaddr_in *)it->ifa_netmask)->sin_addr.s_addr), flags, 0);
	}
	freeifaddrs(all);
#else
	(void)list; (void)max;
#endif
	return n;
}

// The VPN interfaces' IPv6 addresses others can join (unique local or
// global, never link-local), as "[addr]" strings with the interface's name.
// Linux and Android: /proc/net/if_inet6; elsewhere none.
static int lan_vpn_ipv6(char (*addr)[48], char (*name)[16], int max)
{
	int n = 0;
#if defined(TOUCHLAN_SOCKETS) && defined(__linux__)
	FILE *f = fopen("/proc/net/if_inet6", "r");
	char hex[40], ifname[32], line[256];
	unsigned idx, plen, scope, flags;
	lan_iface_t v4[MAX_IFACES];
	int nv4 = lan_interfaces(v4, MAX_IFACES), i, j, kind;
	if (!f)
		return 0;
	while (n < max && fgets(line, sizeof(line), f))
	{
		unsigned char a[16];
		char text[INET6_ADDRSTRLEN];
		if (sscanf(line, "%32s %x %x %x %x %31s", hex, &idx, &plen, &scope, &flags, ifname) != 6 || strlen(hex) != 32)
			continue;
		for (i = 0; i < 16; i++)
		{
			unsigned b;
			if (sscanf(hex + i * 2, "%2x", &b) != 1)
				break;
			a[i] = (unsigned char)b;
		}
		if (i < 16 || !TouchLAN_UsableIPv6(a))
			continue;
		// only on a VPN interface (a LAN's global IPv6 is often not
		// reachable from others, and its IPv4 is listed anyway)
		kind = TouchLAN_InterfaceKind(ifname, TOUCHLAN_IF_UP);
		for (j = 0; j < nv4; j++)
			if (!strcmp(v4[j].name, ifname))
				kind = v4[j].kind;
		if (kind != TOUCHLAN_KIND_VPN_L2 && kind != TOUCHLAN_KIND_VPN_L3)
			continue;
		if (!inet_ntop(AF_INET6, a, text, sizeof(text)))
			continue;
		dpsnprintf(addr[n], sizeof(addr[n]), "[%s]", text);
		dp_strlcpy(name[n], ifname, sizeof(name[n]));
		n++;
	}
	fclose(f);
#else
	(void)addr; (void)name; (void)max;
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
	if (TouchNativeFE_PasswordRejected(kind == 2, addr)) return; // Touch: the desktop app asks for the password
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

// Unicast sweeps of small routed VPN subnets, at most net_slist_lan_sweeprate
// packets a second, net_slist_lan_sweeps passes. They go out of a socket of
// their own: most swept addresses have nobody behind them, and on an
// Ethernet-like link each of those packets waits for an ARP answer that never
// comes, holding send-buffer memory for seconds; from the game's socket that
// stalls the game's own packets (a connect sent then was lost). A host that
// answers is asked again from the game's socket, so the server list sees an
// ordinary reply.
#define MAX_SWEEPS 8
typedef struct lan_sweep_s
{
	lhnetsocket_t *sock;
	uint32_t first;
	int count, cursor, passes;
}
lan_sweep_t;
static lan_sweep_t sweeps[MAX_SWEEPS];
static int numsweeps;
static double sweep_time, sweep_close_time;
static int sweep_fd = -1;
static lhnetsocket_t *sweep_answer_sock;
// games that answered a sweep, asked again from the game's socket a little
// later (a server answers one query per second per address)
#define MAX_ANSWERED 16
static struct { lhnetaddress_t addr; double when; } answered[MAX_ANSWERED];
static int numanswered;
// hosts that answered this round: later passes skip them (the server would
// count the query against its flood guard and drop the game socket's)
static uint32_t found[MAX_ANSWERED];
static int numfound;

#ifdef TOUCHLAN_SOCKETS
static void sweep_start(lhnetsocket_t *s, const lan_iface_t *e)
{
	uint32_t first;
	int count, i;
	if (!TouchLAN_SweepHosts(e->addr, e->mask, &first, &count) || net_slist_lan_sweeps.integer < 1)
		return;
	for (i = 0; i < numsweeps; i++)
		if (sweeps[i].first == first)
			return; // already going
	if (numsweeps >= MAX_SWEEPS)
		return;
	if (sweep_fd < 0)
	{
		sweep_fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (sweep_fd < 0)
			return;
		fcntl(sweep_fd, F_SETFL, fcntl(sweep_fd, F_GETFL, 0) | O_NONBLOCK);
	}
	sweeps[numsweeps].sock = s;
	sweeps[numsweeps].first = first;
	sweeps[numsweeps].count = count;
	sweeps[numsweeps].cursor = 0;
	sweeps[numsweeps].passes = bound(1, net_slist_lan_sweeps.integer, 4);
	numsweeps++;
	Con_DPrintf("LAN sweep: %d hosts on %s\n", count, e->name);
	if (!sweep_time)
	{
		sweep_time = host.realtime;
		numfound = 0;
	}
}

static void sweep_frame(void)
{
	int ports[MAX_PORTS], nports, budget, i, j, n;
	char query[64], reply[64], str[32];
	struct sockaddr_in to, from;
	socklen_t fromlen;
	lhnetaddress_t addr;
	if (sweep_fd < 0)
		return;
	TouchLAN_BuildQuery(query, sizeof(query), host.realtime + (Sys_DirtyTime() - host.dirtytime));
	// answers: ask that game again from the game's socket
	for (;;)
	{
		fromlen = sizeof(from);
		n = (int)recvfrom(sweep_fd, reply, sizeof(reply), 0, (struct sockaddr *)&from, &fromlen);
		if (n <= 0)
			break;
		// the query is the server list's (getstatus or getinfo)
		if (n < 16 || (memcmp(reply, "\377\377\377\377infoResponse", 16) && memcmp(reply, "\377\377\377\377statusResponse", min(n, 18)))
			|| !sweep_answer_sock || !socket_alive(sweep_answer_sock))
			continue;
		dpsnprintf(str, sizeof(str), "%s:%d", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
		Con_DPrintf("LAN sweep: a game answered at %s\n", str);
		if (numfound < MAX_ANSWERED)
			found[numfound++] = ntohl(from.sin_addr.s_addr);
		if (numanswered < MAX_ANSWERED && LHNETADDRESS_FromString(&addr, str, 26000))
		{
			answered[numanswered].addr = addr;
			answered[numanswered].when = host.realtime + 1.1;
			numanswered++;
		}
	}
	for (i = 0; i < numanswered; )
	{
		if (host.realtime < answered[i].when)
		{
			i++;
			continue;
		}
		if (sweep_answer_sock && socket_alive(sweep_answer_sock))
			NetConn_WriteString(sweep_answer_sock, query, &answered[i].addr);
		answered[i] = answered[--numanswered];
	}
	if (!numsweeps)
	{
		sweep_time = 0;
		// a while for late answers, then the socket goes
		if (host.realtime > sweep_close_time && !numanswered)
		{
			close(sweep_fd);
			sweep_fd = -1;
		}
		return;
	}
	budget = (int)((host.realtime - sweep_time) * bound(100, net_slist_lan_sweeprate.integer, 20000));
	if (budget < 1)
		return;
	budget = min(budget, 1000);
	sweep_time = host.realtime;
	nports = TouchLAN_ParsePorts(net_slist_lan_ports.string, ports, MAX_PORTS);
	if (!nports)
		ports[nports++] = 26000;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	for (i = 0; i < numsweeps && budget > 0; )
	{
		lan_sweep_t *w = &sweeps[i];
		sweep_answer_sock = w->sock;
		while (w->passes > 0 && budget > 0)
		{
			uint32_t ip = w->first + (uint32_t)w->cursor;
			int k;
			for (k = 0; k < numfound && found[k] != ip; k++)
				;
			to.sin_addr.s_addr = htonl(ip);
			for (j = 0; j < nports && k == numfound; j++)
			{
				to.sin_port = htons((unsigned short)ports[j]);
				sendto(sweep_fd, query, strlen(query), 0, (struct sockaddr *)&to, sizeof(to));
			}
			budget -= nports;
			if (++w->cursor >= w->count)
			{
				w->cursor = 0;
				w->passes--;
			}
		}
		if (w->passes <= 0)
			sweeps[i] = sweeps[--numsweeps];
		else
			i++;
	}
	sweep_close_time = host.realtime + 3;
}
#else
// No BSD sockets here (web, Windows): the unicast sweep is left out.
static void sweep_start(lhnetsocket_t *s, const lan_iface_t *e) {}
static void sweep_frame(void) {}
#endif

// ---- remembered hosts: every game this device joined on a LAN or VPN
// address, and the ones the player added; asked directly on every LAN search
#define HOSTS_FILE "touch_lan_hosts.txt"
#define MAX_HOSTS 32
static char lan_hosts[MAX_HOSTS][64];
static int num_lan_hosts = -1; // -1: not read yet

static void hosts_cvar(void)
{
	char all[MAX_HOSTS * 65];
	int i;
	all[0] = 0;
	for (i = 0; i < num_lan_hosts; i++)
	{
		if (i)
			dp_strlcat(all, " ", sizeof(all));
		dp_strlcat(all, lan_hosts[i], sizeof(all));
	}
	if (strcmp(touch_lan_hosts.string, all))
		Cvar_SetQuick(&touch_lan_hosts, all);
}

static void hosts_read(void)
{
	qfile_t *f;
	char buf[MAX_HOSTS * 65], *line, *next, one[64];
	fs_offset_t len;
	if (num_lan_hosts >= 0)
		return;
	num_lan_hosts = 0;
	f = FS_OpenRealFile(HOSTS_FILE, "rb", true);
	if (f)
	{
		len = FS_Read(f, buf, sizeof(buf) - 1);
		FS_Close(f);
		buf[len > 0 ? len : 0] = 0;
		for (line = buf; line && *line && num_lan_hosts < MAX_HOSTS; line = next)
		{
			next = strchr(line, '\n');
			if (next)
				*next++ = 0;
			if (TouchLAN_ParseJoin(line, one, sizeof(one)))
				dp_strlcpy(lan_hosts[num_lan_hosts++], one, sizeof(lan_hosts[0]));
		}
	}
	hosts_cvar();
}

static void hosts_write(void)
{
	qfile_t *f = FS_OpenRealFile(HOSTS_FILE, "wb", true);
	int i;
	if (!f)
		return;
	for (i = 0; i < num_lan_hosts; i++)
		FS_Printf(f, "%s\n", lan_hosts[i]);
	FS_Close(f);
	hosts_cvar();
}

// newest first; an address already there moves to the front
static qbool hosts_add(const char *text)
{
	char one[64];
	int i;
	if (!TouchLAN_ParseJoin(text, one, sizeof(one)))
		return false;
	hosts_read();
	for (i = 0; i < num_lan_hosts && strcmp(lan_hosts[i], one); i++)
		;
	if (i == num_lan_hosts)
		i = min(num_lan_hosts, MAX_HOSTS - 1);
	else if (i == 0)
		return true;
	if (i == num_lan_hosts)
		num_lan_hosts++;
	memmove(lan_hosts[1], lan_hosts[0], i * sizeof(lan_hosts[0]));
	dp_strlcpy(lan_hosts[0], one, sizeof(lan_hosts[0]));
	hosts_write();
	return true;
}

static qbool hosts_remove(const char *text)
{
	char one[64];
	int i;
	if (!TouchLAN_ParseJoin(text, one, sizeof(one)))
		return false;
	hosts_read();
	for (i = 0; i < num_lan_hosts; i++)
	{
		if (!strcmp(lan_hosts[i], one))
		{
			memmove(lan_hosts[i], lan_hosts[i + 1], (num_lan_hosts - i - 1) * sizeof(lan_hosts[0]));
			num_lan_hosts--;
			hosts_write();
			return true;
		}
	}
	return false;
}

static void hosts_query(lhnetsocket_t *s, const char *query)
{
	lhnetaddress_t to;
	int i, j, ports[MAX_PORTS], nports;
	hosts_read();
	nports = TouchLAN_ParsePorts(net_slist_lan_ports.string, ports, MAX_PORTS);
	for (i = 0; i < num_lan_hosts; i++)
	{
		// with a port: that one; without: the LAN ports
		qbool hasport = lan_hosts[i][0] == '[' ? strstr(lan_hosts[i], "]:") != NULL : strchr(lan_hosts[i], ':') != NULL;
		for (j = 0; j < (hasport ? 1 : nports); j++)
		{
			if (!LHNETADDRESS_FromString(&to, lan_hosts[i], hasport ? 26000 : ports[j]))
				break;
			if (LHNETADDRESS_GetAddressType(&to) == LHNETADDRESS_GetAddressType(LHNET_AddressFromSocket(s)))
				NetConn_WriteString(s, query, &to);
		}
	}
}

// ---- the VPN's own peer list, when it can be read without asking anyone:
// Tailscale's LocalAPI (tailscaled's socket, readable by the desktop user
// tailscaled runs for; not in the Flatpak sandbox or on Android). Read only.
static int tailscale_peers(uint32_t *out, int max)
{
	int n = 0;
#if defined(TOUCHLAN_SOCKETS) && defined(__linux__) && !defined(__ANDROID__)
	static const char *const paths[] = {"/run/tailscale/tailscaled.sock", "/var/run/tailscale/tailscaled.sock", NULL};
	static const char req[] = "GET /localapi/v0/status HTTP/1.0\r\nHost: local-tailscaled.sock\r\n\r\n";
	struct sockaddr_un sun;
	struct timeval tv = {0, 300000};
	char *buf, *p;
	int fd = -1, i, len = 0, got;
	const int size = 1 << 20;
	if (!net_slist_lan_tailscale.integer)
		return 0;
	for (i = 0; paths[i] && fd < 0; i++)
	{
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0)
			return 0;
		memset(&sun, 0, sizeof(sun));
		sun.sun_family = AF_UNIX;
		dp_strlcpy(sun.sun_path, paths[i], sizeof(sun.sun_path));
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0)
		{
			close(fd);
			fd = -1;
		}
	}
	if (fd < 0)
		return 0;
	buf = (char *)Mem_Alloc(tempmempool, size + 1);
	if (write(fd, req, sizeof(req) - 1) == (ssize_t)(sizeof(req) - 1))
		while (len < size && (got = (int)read(fd, buf + len, size - len)) > 0)
			len += got;
	close(fd);
	buf[len] = 0;
	// every "TailscaleIPs":["100.x.y.z", ...] (the node's own, too: harmless)
	for (p = buf; n < max && (p = strstr(p, "\"TailscaleIPs\":[\"")); )
	{
		unsigned a, b, c, d;
		p += strlen("\"TailscaleIPs\":[\"");
		if (sscanf(p, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a < 256 && b < 256 && c < 256 && d < 256)
			out[n++] = (a << 24) | (b << 16) | (c << 8) | d;
	}
	Mem_Free(buf);
#else
	(void)out; (void)max;
#endif
	return n;
}

static void lan_query(lhnetsocket_t *s, qbool first)
{
	lan_iface_t ifs[MAX_IFACES];
	uint32_t peers[256];
	int ports[MAX_PORTS], nports, nifaces, npeers = 0, i, j;
	char query[64];
	lhnetaddresstype_t type = LHNETADDRESS_GetAddressType(LHNET_AddressFromSocket(s));
	TouchLAN_BuildQuery(query, sizeof(query), host.realtime + (Sys_DirtyTime() - host.dirtytime));
	// remembered and added hosts, IPv4 or IPv6 as the socket is
	hosts_query(s, query);
	if (type != LHNETADDRESSTYPE_INET4)
		return;
	nports = TouchLAN_ParsePorts(net_slist_lan_ports.string, ports, MAX_PORTS);
	if (!nports)
		ports[nports++] = 26000;
	nifaces = lan_interfaces(ifs, MAX_IFACES);
	if (first)
		npeers = tailscale_peers(peers, (int)(sizeof(peers) / sizeof(peers[0])));
	for (j = 0; j < nports; j++)
	{
		// netconn.c sent 255.255.255.255:26000 itself the first time.
		if (!first || ports[j] != 26000)
			send_to(s, query, 0xffffffffu, ports[j]);
		for (i = 0; i < nifaces; i++)
			if (ifs[i].bcast)
				send_to(s, query, ifs[i].bcast, ports[j]);
		for (i = 0; i < npeers; i++)
			send_to(s, query, peers[i], ports[j]);
	}
	// routed VPNs carry no broadcast: sweep the small ones
	if (first)
		for (i = 0; i < nifaces; i++)
			if (ifs[i].kind == TOUCHLAN_KIND_VPN_L3)
				sweep_start(s, &ifs[i]);
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
	if (!cache_queried && !TouchSrvList_Fast()) // the fast list asks the saved servers itself
		cache_query(sock);
	repeats_left = bound(0, net_slist_lan_repeats.integer, 10);
	next_repeat = host.realtime + bound(0.05, net_slist_lan_repeatdelay.value, 5);
}

static void repeat_frame(void)
{
	int i;
	sweep_frame();
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
	char addrs[1024], labels[1536], link[128], players[1024], one[160], label[48];
	char v6[8][48], v6name[8][16];
	lan_iface_t ifs[MAX_IFACES];
	int port = hosted_port(), n, i, humans = 0;
	addrs[0] = labels[0] = link[0] = players[0] = 0;
	if (port)
	{
		// "Join at": every LAN and VPN address, labelled for the UIs
		// (touch_lan_address_labels: "label|address;label|address")
		n = lan_interfaces(ifs, MAX_IFACES);
		for (i = 0; i < n; i++)
		{
			dpsnprintf(one, sizeof(one), "%u.%u.%u.%u:%d", ifs[i].addr >> 24, (ifs[i].addr >> 16) & 255, (ifs[i].addr >> 8) & 255, ifs[i].addr & 255, port);
			if (!link[0])
				dpsnprintf(link, sizeof(link), "godnotic://join/%s", one);
			if (addrs[0])
				dp_strlcat(addrs, " ", sizeof(addrs));
			dp_strlcat(addrs, one, sizeof(addrs));
			TouchLAN_InterfaceLabel(ifs[i].name, ifs[i].kind, label, sizeof(label));
			if (labels[0])
				dp_strlcat(labels, ";", sizeof(labels));
			dp_strlcat(labels, label, sizeof(labels));
			dp_strlcat(labels, "|", sizeof(labels));
			dp_strlcat(labels, one, sizeof(labels));
		}
		n = lan_vpn_ipv6(v6, v6name, 8);
		for (i = 0; i < n; i++)
		{
			dpsnprintf(one, sizeof(one), "%s:%d", v6[i], port);
			if (addrs[0])
				dp_strlcat(addrs, " ", sizeof(addrs));
			dp_strlcat(addrs, one, sizeof(addrs));
			dpsnprintf(label, sizeof(label), "VPN (%s)", v6name[i]);
			if (labels[0])
				dp_strlcat(labels, ";", sizeof(labels));
			dp_strlcat(labels, label, sizeof(labels));
			dp_strlcat(labels, "|", sizeof(labels));
			dp_strlcat(labels, one, sizeof(labels));
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
	if (strcmp(touch_lan_address_labels.string, labels))
		Cvar_SetQuick(&touch_lan_address_labels, labels);
	if (strcmp(touch_lan_join_link.string, link))
		Cvar_SetQuick(&touch_lan_join_link, link);
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
	TouchJoinFail_Frame();
	TouchSrvList_Frame();
	if (!host_pw_ready)
	{
		TouchSrvList_FirstFrame();
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
	if (touch_lan_address_labels.string[0])
	{
		// one line per address: "Wi-Fi 192.168.1.20:26000", "VPN (tailscale0) 100.x.y.z:26000"
		char list[1536], *p, *bar, *next;
		dp_strlcpy(list, touch_lan_address_labels.string, sizeof(list));
		for (p = list; p && *p; p = next)
		{
			next = strchr(p, ';');
			if (next)
				*next++ = 0;
			bar = strchr(p, '|');
			if (bar)
				*bar++ = 0;
			Con_Printf("  %s %s\n", p, bar ? bar : "");
		}
		Con_Printf("Join link: %s\n", touch_lan_join_link.string);
	}
	Con_Printf("Players: %s\n", touch_lan_players.string[0] ? touch_lan_players.string : "none");
}

// touch_lan_host_password_apply: the Host screen's password becomes the
// server's, right before its map command. The menu cannot read either (both
// are CF_PRIVATE, so a server's stuffcmd cannot $-expand them either).
static void TouchLAN_ApplyPassword_f(cmd_state_t *cmd)
{
	Cvar_SetQuick(&sv_touch_lan_password, touch_lan_host_password.string);
	// the Host screen runs this right before its map command
	lan_host_armed = true;
}

// NetConn_OpenServerPorts: a new server (a map command; a changelevel keeps
// the old one). It is a LAN game the player hosts on purpose when the Host
// screen or Create with LAN ticked started it (touch_lan_host_password_apply
// or touch_lan_arm right before the map command); Singleplayer, the campaign,
// Create without LAN or a console map is not.
void TouchLAN_ServerPortsOpened(void)
{
	lan_host_deliberate = lan_host_armed;
	lan_host_armed = false;
}

// NetConn_OpenServerPorts, after TouchLAN_ServerPortsOpened: whether this
// server opens its UDP sockets. A game nobody chose to host has the loopback
// socket only: no port is bound, so no query, challenge or connect from
// another device can reach it, whatever sv_public says.
int TouchLAN_OpenNetPorts(int opennetports)
{
	if (cls.state == ca_dedicated || !touch_lan_private.integer)
		return opennetports;
	if (opennetports && !lan_host_deliberate)
		Con_DPrintf("Local game: no network socket (not hosted on the LAN)\n");
	return opennetports && lan_host_deliberate;
}

static void TouchLAN_Arm_f(cmd_state_t *cmd)
{
	lan_host_armed = true;
}

// SV_Shutdown, before the clients are dropped: the host left or quit. The
// stock protocol's disconnect carries no reason, so the other devices are told
// out of band why it comes; they show "The host ended the game" and do not
// try to join again (touch_net.c).
#define HOSTENDED "touchlan hostended"
void TouchLAN_ServerClosing(void)
{
	int i, k;
	if (cls.state == ca_dedicated || !sv.active)
		return;
	for (i = 0; i < svs.maxclients; i++)
	{
		client_t *c = &svs.clients[i];
		if (!c->active || !c->netconnection || !c->netconnection->mysocket
			|| LHNETADDRESS_GetAddressType(&c->netconnection->peeraddress) == LHNETADDRESSTYPE_LOOP)
			continue;
		for (k = 0; k < 2; k++)
			NetConn_Write(c->netconnection->mysocket, "\377\377\377\377" HOSTENDED, 4 + (int)strlen(HOSTENDED), &c->netconnection->peeraddress);
	}
}

static double host_ended_time;
// Client, a connectionless packet: the marker above from the server we are on.
void TouchLAN_ClientConnectionless(const char *string, int length, const lhnetaddress_t *peer)
{
	if (length != (int)strlen(HOSTENDED) || memcmp(string, HOSTENDED, length) || !cls.netcon || sv.active)
		return;
	if (LHNETADDRESS_Compare(peer, &cls.netcon->peeraddress))
		return;
	host_ended_time = host.realtime;
}

// touch_net.c, when the client disconnects: true when the host just said it
// ended the game (then nothing is joined again and the menu says so).
qbool TouchLAN_HostEnded(void)
{
	if (!host_ended_time || host.realtime - host_ended_time > 5)
		return false;
	host_ended_time = 0;
	Cvar_SetQuick(&touch_lan_ended, "1");
	return true;
}

qbool TouchLAN_HostingOnPurpose(void)
{
	return lan_host_deliberate && sv.active && svs.maxclients > 1;
}

// Humans in the local game other than this device's player.
int TouchLAN_OtherHumans(void)
{
	int i, n = 0;
	if (!sv.active)
		return 0;
	for (i = 0; i < svs.maxclients; i++)
	{
		client_t *c = &svs.clients[i];
		if (c->active && c->netconnection && LHNETADDRESS_GetAddressType(&c->netconnection->peeraddress) != LHNETADDRESSTYPE_LOOP)
			n++;
	}
	return n;
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

#ifdef CONFIG_MENU
// lan_addhost ADDRESS[:PORT] | godnotic://join/...: remember a game to ask
// on every LAN search (a friend's VPN address, say)
static void TouchLAN_AddHost_f(cmd_state_t *cmd)
{
	if (Cmd_Argc(cmd) < 2 || !hosts_add(Cmd_Argv(cmd, 1)))
		Con_Printf("usage: lan_addhost ADDRESS[:PORT] (or a godnotic://join/ link)\n");
}

// lan_forgethost ADDRESS[:PORT]: no longer ask it; without one, forget all
static void TouchLAN_ForgetHost_f(cmd_state_t *cmd)
{
	if (Cmd_Argc(cmd) < 2)
	{
		hosts_read();
		num_lan_hosts = 0;
		hosts_write();
		return;
	}
	if (!hosts_remove(Cmd_Argv(cmd, 1)))
		Con_Printf("%s is not a remembered host\n", Cmd_Argv(cmd, 1));
}

static void TouchLAN_Hosts_f(cmd_state_t *cmd)
{
	int i;
	hosts_read();
	for (i = 0; i < num_lan_hosts; i++)
		Con_Printf("%s\n", lan_hosts[i]);
	if (!num_lan_hosts)
		Con_Printf("No remembered LAN or VPN games.\n");
}

// lan_join LINK-OR-ADDRESS: join from a shared join string
static void TouchLAN_Join_f(cmd_state_t *cmd)
{
	char one[64], line[96];
	if (Cmd_Argc(cmd) < 2 || !TouchLAN_ParseJoin(Cmd_Args(cmd), one, sizeof(one)))
	{
		Con_Printf("usage: lan_join godnotic://join/ADDRESS:PORT (or ADDRESS[:PORT])\n");
		return;
	}
	dpsnprintf(line, sizeof(line), "connect \"%s\"\n", one);
	Cbuf_AddText(cmd, line);
}
#endif

// touch_net.c: a connection was accepted. A game on a LAN or VPN address is
// remembered, so the next LAN search asks it even where no broadcast reaches.
void TouchLAN_Joined(const lhnetaddress_t *peer)
{
#ifdef CONFIG_MENU
	char text[128];
	lhnetaddresstype_t type = LHNETADDRESS_GetAddressType(peer);
	if (type != LHNETADDRESSTYPE_INET4 && type != LHNETADDRESSTYPE_INET6)
		return;
	if (!LHNETADDRESS_ToString(peer, text, sizeof(text), true))
		return;
	if (type == LHNETADDRESSTYPE_INET4)
	{
		unsigned a, b, c, d;
		if (sscanf(text, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a == 127
			|| !TouchLAN_IsLanIPv4((a << 24) | (b << 16) | (c << 8) | d))
			return;
	}
	else if (!(text[0] == '[' && (text[1] == 'f' || text[1] == 'F') && (text[2] == 'c' || text[2] == 'd' || text[2] == 'C' || text[2] == 'D')))
		return; // IPv6: unique local addresses (VPNs) only
	hosts_add(text);
#else
	(void)peer;
#endif
}

void TouchLAN_Init(void)
{
	cvar_t *v;
	TouchNet_Init(); // joins over real networks: touch_net.c
	TouchJoinFail_Init(); // a failed join says why: touch_joinfail.c
	TouchSrvList_Init(); // the fast server list: touch_srvlist.c
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
	Cvar_RegisterVariable(&touch_lan_private);
	Cvar_RegisterVariable(&touch_lan_popup_autoclose);
	Cvar_RegisterVariable(&touch_lan_ended);
	Cvar_RegisterCallback(&touch_lan_host_password, HostPassword_c);
	Cvar_RegisterCallback(&touch_lan_host_password_remember, HostPassword_c);
	Cmd_AddCommand(CF_SHARED, "touch_lan_status", TouchLAN_Status_f, "show the addresses and players of the game this one hosts");
	Cmd_AddCommand(CF_CLIENT, "touch_lan_host_password_apply", TouchLAN_ApplyPassword_f, "make the Host screen's password (touch_lan_host_password) the server's join password");
	Cmd_AddCommand(CF_SHARED, "touch_lan_arm", TouchLAN_Arm_f, "the next map command hosts its game on the LAN (opens the network port); without it a game started on this device is local only while touch_lan_private is 1");
	Cmd_AddCommand(CF_CLIENT, "touch_lan_forget", TouchLAN_Forget_f, "forget remembered join passwords: all, or the one for an address");
	Cvar_RegisterVariable(&touch_lan_address_labels);
	Cvar_RegisterVariable(&touch_lan_join_link);
	Cvar_RegisterVariable(&touch_lan_hosts);
	Cvar_RegisterVariable(&net_slist_lan_sweeps);
	Cvar_RegisterVariable(&net_slist_lan_sweeprate);
	Cvar_RegisterVariable(&net_slist_lan_tailscale);
#ifdef CONFIG_MENU
	Cmd_AddCommand(CF_CLIENT, "lan_addhost", TouchLAN_AddHost_f, "remember a LAN or VPN game (ADDRESS[:PORT] or a godnotic://join/ link) for every LAN search");
	Cmd_AddCommand(CF_CLIENT, "lan_forgethost", TouchLAN_ForgetHost_f, "stop asking a remembered LAN or VPN game; without an address, all of them");
	Cmd_AddCommand(CF_CLIENT, "lan_hosts", TouchLAN_Hosts_f, "list the remembered LAN and VPN games");
	Cmd_AddCommand(CF_CLIENT, "lan_join", TouchLAN_Join_f, "join from a shared join string (godnotic://join/ADDRESS:PORT or ADDRESS[:PORT])");
#endif

	// The master servers' answer is in long before 4 s, and DarkPlaces servers
	// echo a timestamp for the ping, so the stock wait before the list's own
	// queries only delays the list. Neither is archived.
	if ((v = Cvar_FindVar(&cvars_all, "net_slist_timeout", ~0)) && !strcmp(v->string, "4"))
		Cvar_SetQuick(v, "1.5");
	if ((v = Cvar_FindVar(&cvars_all, "net_slist_queriesperframe", ~0)) && !strcmp(v->string, "2"))
		Cvar_SetQuick(v, "8");
}
