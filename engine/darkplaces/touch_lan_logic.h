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

// Private, link-local, CGNAT-shared and loopback IPv4: what the server list
// marks as a LAN game (host byte order).
static inline int TouchLAN_IsLanIPv4(uint32_t a)
{
	return (a >> 24) == 10
		|| (a >> 24) == 127
		|| (a >> 20) == ((172u << 4) | 1u)         // 172.16.0.0/12
		|| (a >> 16) == ((192u << 8) | 168u)       // 192.168.0.0/16
		|| (a >> 16) == ((169u << 8) | 254u)       // 169.254.0.0/16
		|| (a >> 22) == ((100u << 2) | 1u);        // 100.64.0.0/10
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

// Which network interfaces others on the local network can reach this device
// on, for the Host screen's "Join at" list and the LAN search's broadcasts:
// 0 none (loopback, point-to-point, a cellular modem's interface: rmnet* and
// the like, whose addresses only the carrier sees, or a VPN tunnel),
// 2 Wi-Fi, Ethernet or a hotspot this device runs (wlan*, swlan*, ap*, eth*,
// en*, softap*, rndis*, the USB and Bluetooth tethering ones), 1 anything
// else (listed after those). flags: TOUCHLAN_IF_* below.
#define TOUCHLAN_IF_UP          1
#define TOUCHLAN_IF_LOOPBACK    2
#define TOUCHLAN_IF_POINTOPOINT 4
static inline int TouchLAN_InterfaceRank(const char *name, int flags)
{
	static const char *const cellular[] = {"rmnet", "ccmni", "pdp", "seth_", "v4-", "clat", "wwan", "umts", "ppp", "tun", "ipsec", "dummy", "ifb", "sit", "ip6", "gre", "erspan", "vti", NULL};
	static const char *const local[] = {"wlan", "swlan", "ap", "eth", "en", "wl", "softap", "rndis", "usb", "ncm", "bt-pan", "bnep", NULL};
	int i;
	if (!(flags & TOUCHLAN_IF_UP) || (flags & (TOUCHLAN_IF_LOOPBACK | TOUCHLAN_IF_POINTOPOINT)))
		return 0;
	if (!name)
		return 1;
	for (i = 0; cellular[i]; i++)
		if (!strncmp(name, cellular[i], strlen(cellular[i])))
			return 0;
	for (i = 0; local[i]; i++)
		if (!strncmp(name, local[i], strlen(local[i])))
			return 2;
	return 1;
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
