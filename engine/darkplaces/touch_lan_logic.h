#ifndef TOUCH_LAN_LOGIC_H
#define TOUCH_LAN_LOGIC_H

/*
Godnotic LAN games: the pure pieces of touch_lan.c, kept free of engine
state so tests/lan/lan_test.c can check them with a plain C compiler.
*/

#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// Marker at the start of a "reject" reason for a password problem. A stock
// client prints the whole reason, which still reads as a sentence; ours
// recognises the marker and asks for the password instead.
#define TOUCHLAN_REJECT_REQUIRED "[pw-required] "
#define TOUCHLAN_REJECT_WRONG    "[pw-wrong] "

// The qcstatus key a password-protected server adds (":L1"). The menu's
// qcstatus parsers skip keys they do not know.
#define TOUCHLAN_QCSTATUS_LOCK ":L1"

// The subnet-directed broadcast address of an interface (host byte order).
// 0 when there is none worth sending to: loopback, a /31 or /32 point-to-point
// link, or no address.
static inline uint32_t TouchLAN_DirectedBroadcast(uint32_t addr, uint32_t mask)
{
	if (!addr || (addr >> 24) == 127)
		return 0;
	if (mask == 0xffffffffu || mask == 0xfffffffeu)
		return 0;
	return (addr & mask) | ~mask;
}

// Private, link-local, CGNAT-shared (Tailscale's 100.64.0.0/10) and loopback
// IPv4, and the ranges Hamachi (25/8) and Radmin VPN (26/8) give out: what
// the server list marks as a LAN game (host byte order).
static inline int TouchLAN_IsLanIPv4(uint32_t a)
{
	return (a >> 24) == 10
		|| (a >> 24) == 127
		|| (a >> 20) == ((172u << 4) | 1u)         // 172.16.0.0/12
		|| (a >> 16) == ((192u << 8) | 168u)       // 192.168.0.0/16
		|| (a >> 16) == ((169u << 8) | 254u)       // 169.254.0.0/16
		|| (a >> 22) == ((100u << 2) | 1u)         // 100.64.0.0/10
		|| (a >> 24) == 25 || (a >> 24) == 26;     // Hamachi, Radmin VPN
}

// Insert the lock key into a server's qcstatus ("dm:0.8.6:P0:S5:F2:MXonotic::score...").
// The menu stops reading keys at the first empty field, so it goes before the
// "::" that starts the score part, or at the end when there is none. Nothing
// changes when it does not fit or is already there.
static inline void TouchLAN_InsertQcstatusLock(char *qcstatus, size_t size)
{
	size_t len = strlen(qcstatus), add = strlen(TOUCHLAN_QCSTATUS_LOCK), at;
	const char *dd;
	if (!len || strstr(qcstatus, TOUCHLAN_QCSTATUS_LOCK ":") || len + add + 1 > size)
		return;
	if (len >= add && !strcmp(qcstatus + len - add, TOUCHLAN_QCSTATUS_LOCK))
		return;
	dd = strstr(qcstatus, "::");
	at = dd ? (size_t)(dd - qcstatus) : len;
	memmove(qcstatus + at + add, qcstatus + at, len - at + 1);
	memcpy(qcstatus + at, TOUCHLAN_QCSTATUS_LOCK, add);
}

// What a reject reason says about the password: 0 nothing, 1 one is needed,
// 2 the one sent was wrong.
static inline int TouchLAN_RejectKind(const char *reason, size_t len)
{
	size_t r = strlen(TOUCHLAN_REJECT_REQUIRED), w = strlen(TOUCHLAN_REJECT_WRONG);
	if (len >= r && !memcmp(reason, TOUCHLAN_REJECT_REQUIRED, r))
		return 1;
	if (len >= w && !memcmp(reason, TOUCHLAN_REJECT_WRONG, w))
		return 2;
	return 0;
}

// Parse a list of ports ("26000 26001-26003") into out[], skipping junk and
// duplicates. Returns how many.
static inline int TouchLAN_ParsePorts(const char *s, int *out, int max)
{
	int n = 0;
	while (s && *s && n < max)
	{
		char *end;
		long a, b, p;
		while (*s == ' ' || *s == ',' || *s == '\t')
			s++;
		if (!*s)
			break;
		a = strtol(s, &end, 10);
		if (end == s)
		{
			s++;
			continue;
		}
		b = a;
		s = end;
		if (*s == '-')
		{
			b = strtol(s + 1, &end, 10);
			if (end == s + 1)
				b = a;
			s = end;
		}
		if (b < a || b - a > 64)
			b = a;
		for (p = a; p <= b && n < max; p++)
		{
			int i, dup = 0;
			if (p < 1 || p > 65535)
				continue;
			for (i = 0; i < n; i++)
				dup |= out[i] == (int)p;
			if (!dup)
				out[n++] = (int)p;
		}
	}
	return n;
}

// Which network interfaces others can reach this device on, for the Host
// screen's "Join at" list and the LAN search. flags: TOUCHLAN_IF_* below.
//   NONE    loopback, down, a cellular modem's interface (rmnet*, ccmni*,
//           clat*, v4-*, ...: addresses only the carrier sees), or a tunnel
//           that is not a VPN (sit, gre, ...)
//   LOCAL   Wi-Fi, Ethernet, a hotspot this device runs, USB/Bluetooth
//           tethering: broadcast reaches the others
//   VPN_L2  an Ethernet-like overlay (ZeroTier zt*, OpenVPN tap*, Hamachi
//           ham*, n2n edge*): broadcast reaches the peers
//   VPN_L3  a routed overlay (Tailscale, WireGuard wg*, tun*/utun*, Nebula,
//           NetBird wt*, IPsec), or any point-to-point or non-broadcast
//           link: no broadcast, so unicast probes (TouchLAN_SweepHosts and
//           the remembered hosts)
//   OTHER   anything else with broadcast (bridges, docker0): listed last
#define TOUCHLAN_IF_UP          1
#define TOUCHLAN_IF_LOOPBACK    2
#define TOUCHLAN_IF_POINTOPOINT 4
#define TOUCHLAN_IF_BROADCAST   8
#define TOUCHLAN_KIND_NONE   0
#define TOUCHLAN_KIND_LOCAL  1
#define TOUCHLAN_KIND_VPN_L2 2
#define TOUCHLAN_KIND_VPN_L3 3
#define TOUCHLAN_KIND_OTHER  4

static inline int TouchLAN_Prefix(const char *name, const char *const *list)
{
	int i;
	for (i = 0; list[i]; i++)
		if (!strncmp(name, list[i], strlen(list[i])))
			return 1;
	return 0;
}

static inline int TouchLAN_InterfaceKind(const char *name, int flags)
{
	static const char *const hidden[] = {"rmnet", "ccmni", "pdp", "seth_", "v4-", "clat", "wwan", "umts", "ppp",
		"dummy", "ifb", "sit", "ip6", "gre", "erspan", "lo", NULL};
	static const char *const vpn3[] = {"tailscale", "tun", "utun", "wg", "nebula", "wt", "ipsec", "vti", "nordlynx", "mullvad", "proton", NULL};
	static const char *const vpn2[] = {"zt", "tap", "ham", "edge", "vpn", NULL};
	static const char *const local[] = {"wlan", "swlan", "ap", "eth", "en", "wl", "softap", "rndis", "usb", "ncm", "bt-pan", "bnep", NULL};
	if (!(flags & TOUCHLAN_IF_UP) || (flags & TOUCHLAN_IF_LOOPBACK))
		return TOUCHLAN_KIND_NONE;
	if (!name)
		return TOUCHLAN_KIND_OTHER;
	if (TouchLAN_Prefix(name, hidden))
		return TOUCHLAN_KIND_NONE;
	if (TouchLAN_Prefix(name, vpn3) || (flags & TOUCHLAN_IF_POINTOPOINT) || !(flags & TOUCHLAN_IF_BROADCAST))
		return TOUCHLAN_KIND_VPN_L3;
	if (TouchLAN_Prefix(name, vpn2))
		return TOUCHLAN_KIND_VPN_L2;
	if (TouchLAN_Prefix(name, local))
		return TOUCHLAN_KIND_LOCAL;
	return TOUCHLAN_KIND_OTHER;
}

// Order of the "Join at" list: 0 not listed, higher first (LAN, then the
// VPNs, then anything else).
static inline int TouchLAN_InterfaceRank(const char *name, int flags)
{
	static const int rank[] = {0, 4, 3, 2, 1};
	return rank[TouchLAN_InterfaceKind(name, flags)];
}

// out = a b c, cut to size (the engine forbids snprintf)
static inline void TouchLAN_Join3(char *out, size_t size, const char *a, const char *b, const char *c)
{
	const char *parts[3];
	size_t n = 0, i;
	parts[0] = a; parts[1] = b; parts[2] = c;
	for (i = 0; i < 3; i++)
		for (; parts[i] && *parts[i] && n + 1 < size; parts[i]++)
			out[n++] = *parts[i];
	out[n] = 0;
}

// The words before an address in the "Join at" list.
static inline void TouchLAN_InterfaceLabel(const char *name, int kind, char *out, size_t size)
{
	static const char *const hotspot[] = {"swlan", "ap", "softap", NULL};
	static const char *const wifi[] = {"wlan", "wl", NULL};
	static const char *const usb[] = {"rndis", "usb", "ncm", NULL};
	static const char *const bt[] = {"bt-pan", "bnep", NULL};
	const char *what;
	if (!out || !size)
		return;
	if (kind == TOUCHLAN_KIND_VPN_L2 || kind == TOUCHLAN_KIND_VPN_L3)
	{
		TouchLAN_Join3(out, size, "VPN (", name ? name : "?", ")");
		return;
	}
	if (kind == TOUCHLAN_KIND_LOCAL && name)
	{
		what = TouchLAN_Prefix(name, hotspot) ? "Hotspot" : TouchLAN_Prefix(name, wifi) ? "Wi-Fi"
			: TouchLAN_Prefix(name, usb) ? "USB tethering" : TouchLAN_Prefix(name, bt) ? "Bluetooth" : "Ethernet";
		TouchLAN_Join3(out, size, what, NULL, NULL);
		return;
	}
	TouchLAN_Join3(out, size, "LAN (", name ? name : "?", ")");
}

// The hosts of a small routed subnet the LAN search sweeps with unicast
// queries (host byte order): from *first, *count of them. Only prefixes
// /22 to /30 (at most 1022 hosts); a /31, a /32 (Tailscale gives each node
// one) or anything bigger is not swept: its peers come from the remembered
// hosts and the VPN's own peer list. Returns *count.
static inline int TouchLAN_SweepHosts(uint32_t addr, uint32_t mask, uint32_t *first, int *count)
{
	int prefix = 0;
	uint32_t m = mask;
	while (m & 0x80000000u)
	{
		prefix++;
		m <<= 1;
	}
	*first = 0;
	*count = 0;
	if (m || prefix < 22 || prefix > 30 || !addr)
		return 0;
	*first = (addr & mask) + 1;
	*count = (int)((1u << (32 - prefix)) - 2);
	return *count;
}

// A join string as the Host screen shows it, or as a player types or pastes
// it: "godnotic://join/HOST:PORT", "HOST:PORT", "HOST", "[v6]:PORT", with
// spaces and a trailing slash tolerated. Writes "HOST:PORT" or "HOST" to
// out; 0 when it is not an address (anything but letters, digits and
// ".:-[]%_").
static inline int TouchLAN_ParseJoin(const char *in, char *out, size_t size)
{
	static const char prefix[] = "godnotic://join/";
	size_t n, i;
	if (!in || !out || size < 2)
		return 0;
	while (*in == ' ' || *in == '\t')
		in++;
	n = strlen(prefix);
	for (i = 0; i < n && in[i] && ((in[i] | 32) == prefix[i] || in[i] == prefix[i]); i++)
		;
	if (i == n)
		in += n;
	n = strlen(in);
	while (n && (in[n - 1] == ' ' || in[n - 1] == '\t' || in[n - 1] == '/' || in[n - 1] == '\n' || in[n - 1] == '\r'))
		n--;
	if (!n || n >= size)
		return 0;
	for (i = 0; i < n; i++)
	{
		char c = in[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == ':' || c == '-' || c == '[' || c == ']' || c == '%' || c == '_'))
			return 0;
	}
	memcpy(out, in, n);
	out[n] = 0;
	return 1;
}

// A VPN or LAN IPv6 address worth listing and probing: unique local
// (fc00::/7: Tailscale's fd7a:115c:a1e0::/48, ZeroTier's RFC 4193 and
// 6plane) or global unicast (2000::/3). Link-local (fe80::/10) is left out:
// the engine's address strings drop its scope, so it could not be joined.
static inline int TouchLAN_UsableIPv6(const unsigned char *a)
{
	return (a[0] & 0xfe) == 0xfc || (a[0] & 0xe0) == 0x20;
}

// Whether a game keeps running while the Android app is in the background
// (touch_android.c). A local game (this device runs the server) pauses unless
// another human is in it or the player deliberately hosts it for the LAN
// (started from the Host screen): a bot match from Create or Singleplayer
// pauses even with more than one player slot. A connection to another
// server, a join or a download keeps going, or the server would time the
// player out.
static inline int TouchLAN_KeepRunningInBackground(int local_server, int lan_hosting, int other_humans, int remote_busy)
{
	if (local_server)
		return lan_hosting || other_humans > 0;
	return remote_busy;
}

#endif
