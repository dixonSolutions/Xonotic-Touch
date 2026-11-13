// The pure parts of the LAN code (engine/darkplaces/touch_lan_logic.h):
// directed broadcasts, what counts as a LAN address, the lock key in a
// server's qcstatus, the password reject markers and the port list.
// The engine end to end (hosting, discovery, password) is scripts/test-lan.sh.
#include <stdio.h>
#include "touch_lan_logic.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t ip(unsigned a, unsigned b, unsigned c, unsigned d)
{
	return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
}

static void test_broadcast(void)
{
	CHECK(TouchLAN_DirectedBroadcast(ip(192, 168, 1, 23), ip(255, 255, 255, 0)) == ip(192, 168, 1, 255), "/24");
	CHECK(TouchLAN_DirectedBroadcast(ip(10, 1, 2, 3), ip(255, 0, 0, 0)) == ip(10, 255, 255, 255), "/8");
	CHECK(TouchLAN_DirectedBroadcast(ip(172, 20, 10, 2), ip(255, 255, 255, 240)) == ip(172, 20, 10, 15), "/28 (iPhone hotspot)");
	CHECK(TouchLAN_DirectedBroadcast(ip(192, 168, 43, 1), ip(255, 255, 255, 0)) == ip(192, 168, 43, 255), "Android hotspot");
	CHECK(TouchLAN_DirectedBroadcast(ip(127, 0, 0, 1), ip(255, 0, 0, 0)) == 0, "loopback");
	CHECK(TouchLAN_DirectedBroadcast(ip(10, 0, 0, 1), 0xffffffffu) == 0, "/32");
	CHECK(TouchLAN_DirectedBroadcast(ip(10, 0, 0, 1), 0xfffffffeu) == 0, "/31");
	CHECK(TouchLAN_DirectedBroadcast(0, ip(255, 255, 255, 0)) == 0, "no address");
}

static void test_lan_address(void)
{
	CHECK(TouchLAN_IsLanIPv4(ip(192, 168, 0, 5)), "192.168/16");
	CHECK(TouchLAN_IsLanIPv4(ip(10, 200, 1, 1)), "10/8");
	CHECK(TouchLAN_IsLanIPv4(ip(172, 16, 0, 1)) && TouchLAN_IsLanIPv4(ip(172, 31, 255, 254)), "172.16/12");
	CHECK(!TouchLAN_IsLanIPv4(ip(172, 32, 0, 1)) && !TouchLAN_IsLanIPv4(ip(172, 15, 0, 1)), "outside 172.16/12");
	CHECK(TouchLAN_IsLanIPv4(ip(169, 254, 3, 4)), "link-local");
	CHECK(TouchLAN_IsLanIPv4(ip(100, 64, 0, 1)) && TouchLAN_IsLanIPv4(ip(100, 127, 0, 1)), "CGNAT shared");
	CHECK(!TouchLAN_IsLanIPv4(ip(100, 128, 0, 1)), "outside 100.64/10");
	CHECK(TouchLAN_IsLanIPv4(ip(127, 0, 0, 1)), "loopback");
	CHECK(!TouchLAN_IsLanIPv4(ip(8, 8, 8, 8)) && !TouchLAN_IsLanIPv4(ip(5, 9, 1, 2)), "internet");
}

static void test_qcstatus(void)
{
	char s[64];
	strcpy(s, "dm:0.8.6:P0:S5:F2:MXonotic::score!!");
	TouchLAN_InsertQcstatusLock(s, sizeof(s));
	CHECK(!strcmp(s, "dm:0.8.6:P0:S5:F2:MXonotic:L1::score!!"), "before the score part: %s", s);
	TouchLAN_InsertQcstatusLock(s, sizeof(s));
	CHECK(!strcmp(s, "dm:0.8.6:P0:S5:F2:MXonotic:L1::score!!"), "only once: %s", s);
	strcpy(s, "dm:0.8.6:P0");
	TouchLAN_InsertQcstatusLock(s, sizeof(s));
	CHECK(!strcmp(s, "dm:0.8.6:P0:L1"), "no score part: %s", s);
	TouchLAN_InsertQcstatusLock(s, sizeof(s));
	CHECK(!strcmp(s, "dm:0.8.6:P0:L1"), "no score part, only once: %s", s);
	strcpy(s, "");
	TouchLAN_InsertQcstatusLock(s, sizeof(s));
	CHECK(!strcmp(s, ""), "empty stays empty");
	{
		char small[14] = "dm:0.8.6:P0:";
		TouchLAN_InsertQcstatusLock(small, sizeof(small));
		CHECK(!strcmp(small, "dm:0.8.6:P0:"), "does not overflow: %s", small);
	}
}

static void test_reject(void)
{
	const char *r = TOUCHLAN_REJECT_REQUIRED "This game needs a password.";
	const char *w = TOUCHLAN_REJECT_WRONG "Wrong password.";
	CHECK(TouchLAN_RejectKind(r, strlen(r)) == 1, "required");
	CHECK(TouchLAN_RejectKind(w, strlen(w)) == 2, "wrong");
	CHECK(TouchLAN_RejectKind("Server is full.", 15) == 0, "other");
	CHECK(TouchLAN_RejectKind(r, 3) == 0, "cut short");
}

static void test_ports(void)
{
	int p[16], n;
	n = TouchLAN_ParsePorts("26000-26004", p, 16);
	CHECK(n == 5 && p[0] == 26000 && p[4] == 26004, "range: %d", n);
	n = TouchLAN_ParsePorts(" 26000, 27500 26000 x 26001-26000 70000", p, 16);
	CHECK(n == 3 && p[0] == 26000 && p[1] == 27500 && p[2] == 26001, "list, duplicate, junk, reversed, out of range: %d", n);
	n = TouchLAN_ParsePorts("1-100000", p, 16);
	CHECK(n == 1 && p[0] == 1, "huge range is one port: %d", n);
	n = TouchLAN_ParsePorts("26000-26010", p, 4);
	CHECK(n == 4, "capped at max: %d", n);
	n = TouchLAN_ParsePorts("", p, 16);
	CHECK(n == 0, "empty");
}

static void test_interfaces(void)
{
	const int up = TOUCHLAN_IF_UP | TOUCHLAN_IF_BROADCAST, p2p = TOUCHLAN_IF_UP | TOUCHLAN_IF_POINTOPOINT;
	char label[64];
	CHECK(TouchLAN_InterfaceRank("wlan0", up) == 4, "Wi-Fi");
	CHECK(TouchLAN_InterfaceRank("swlan0", up) == 4, "Samsung hotspot");
	CHECK(TouchLAN_InterfaceRank("ap0", up) == 4 && TouchLAN_InterfaceRank("softap0", up) == 4, "hotspot");
	CHECK(TouchLAN_InterfaceRank("eth0", up) == 4 && TouchLAN_InterfaceRank("enp3s0", up) == 4 && TouchLAN_InterfaceRank("wlp2s0", up) == 4, "desktop names");
	CHECK(TouchLAN_InterfaceRank("rndis0", up) == 4 && TouchLAN_InterfaceRank("bt-pan", up) == 4, "tethering");
	CHECK(TouchLAN_InterfaceRank("rmnet0", up) == 0 && TouchLAN_InterfaceRank("rmnet_data1", up) == 0, "Qualcomm/Exynos modem");
	CHECK(TouchLAN_InterfaceRank("ccmni0", up) == 0, "MediaTek modem");
	CHECK(TouchLAN_InterfaceRank("v4-rmnet_data0", up) == 0 && TouchLAN_InterfaceRank("clat4", up) == 0, "464XLAT");
	CHECK(TouchLAN_InterfaceRank("rmnet0", p2p) == 0, "a point-to-point modem stays hidden");
	CHECK(TouchLAN_InterfaceRank("lo", up | TOUCHLAN_IF_LOOPBACK) == 0, "loopback");
	CHECK(TouchLAN_InterfaceRank("wlan0", 0) == 0, "down");
	CHECK(TouchLAN_InterfaceRank("sit0", up) == 0 && TouchLAN_InterfaceRank("gre0", up) == 0, "non-VPN tunnels");
	CHECK(TouchLAN_InterfaceRank("br0", up) == 1 && TouchLAN_InterfaceRank("docker0", up) == 1, "other: listed last");
	// VPNs are listed, after the LAN; which ones get broadcast
	CHECK(TouchLAN_InterfaceKind("tailscale0", p2p) == TOUCHLAN_KIND_VPN_L3, "Tailscale: routed");
	CHECK(TouchLAN_InterfaceKind("tun0", p2p) == TOUCHLAN_KIND_VPN_L3, "Android VPN app tun0: routed");
	CHECK(TouchLAN_InterfaceKind("wg0", TOUCHLAN_IF_UP) == TOUCHLAN_KIND_VPN_L3, "WireGuard: routed");
	CHECK(TouchLAN_InterfaceKind("nebula1", up) == TOUCHLAN_KIND_VPN_L3, "Nebula by name");
	CHECK(TouchLAN_InterfaceKind("wlan0", p2p) == TOUCHLAN_KIND_VPN_L3, "any point-to-point link: unicast");
	CHECK(TouchLAN_InterfaceKind("ztabcdef12", up) == TOUCHLAN_KIND_VPN_L2, "ZeroTier: broadcast");
	CHECK(TouchLAN_InterfaceKind("tap0", up) == TOUCHLAN_KIND_VPN_L2 && TouchLAN_InterfaceKind("ham0", up) == TOUCHLAN_KIND_VPN_L2, "tap, Hamachi: broadcast");
	CHECK(TouchLAN_InterfaceKind("zt0", TOUCHLAN_IF_UP) == TOUCHLAN_KIND_VPN_L3, "no broadcast flag: unicast");
	CHECK(TouchLAN_InterfaceRank("tailscale0", p2p) == 2 && TouchLAN_InterfaceRank("zt0", up) == 3, "VPN ranks");
	TouchLAN_InterfaceLabel("wlan0", TOUCHLAN_KIND_LOCAL, label, sizeof(label));
	CHECK(!strcmp(label, "Wi-Fi"), "label Wi-Fi");
	TouchLAN_InterfaceLabel("swlan0", TOUCHLAN_KIND_LOCAL, label, sizeof(label));
	CHECK(!strcmp(label, "Hotspot"), "label hotspot");
	TouchLAN_InterfaceLabel("enp3s0", TOUCHLAN_KIND_LOCAL, label, sizeof(label));
	CHECK(!strcmp(label, "Ethernet"), "label Ethernet");
	TouchLAN_InterfaceLabel("tailscale0", TOUCHLAN_KIND_VPN_L3, label, sizeof(label));
	CHECK(!strcmp(label, "VPN (tailscale0)"), "label VPN");
}

static void test_sweep(void)
{
	uint32_t first;
	int count;
	CHECK(TouchLAN_SweepHosts(ip(10, 66, 0, 7), 0xffffff00u, &first, &count) == 254 && first == ip(10, 66, 0, 1), "/24: 254 hosts");
	CHECK(TouchLAN_SweepHosts(ip(10, 66, 4, 7), 0xfffffc00u, &first, &count) == 1022 && first == ip(10, 66, 4, 1), "/22: 1022 hosts");
	CHECK(TouchLAN_SweepHosts(ip(10, 66, 0, 7), 0xfffff800u, &first, &count) == 0, "/21: too big");
	CHECK(TouchLAN_SweepHosts(ip(100, 115, 126, 72), 0xffffffffu, &first, &count) == 0, "Tailscale /32: none");
	CHECK(TouchLAN_SweepHosts(ip(10, 0, 0, 1), 0xfffffffeu, &first, &count) == 0, "/31: none");
	CHECK(TouchLAN_SweepHosts(ip(10, 0, 0, 1), 0xff00ff00u, &first, &count) == 0, "not a prefix");
}

static void test_join_string(void)
{
	char out[128];
	CHECK(TouchLAN_ParseJoin("godnotic://join/100.101.102.103:26000", out, sizeof(out)) && !strcmp(out, "100.101.102.103:26000"), "link");
	CHECK(TouchLAN_ParseJoin("  GODNOTIC://JOIN/10.0.0.2:26001/ ", out, sizeof(out)) && !strcmp(out, "10.0.0.2:26001"), "link, any case, slash, spaces");
	CHECK(TouchLAN_ParseJoin("192.168.1.20", out, sizeof(out)) && !strcmp(out, "192.168.1.20"), "plain address");
	CHECK(TouchLAN_ParseJoin("[fd7a:115c:a1e0::ff28:7e49]:26000", out, sizeof(out)) && !strcmp(out, "[fd7a:115c:a1e0::ff28:7e49]:26000"), "IPv6");
	CHECK(TouchLAN_ParseJoin("host.tailnet.ts.net:26000", out, sizeof(out)), "a name");
	CHECK(!TouchLAN_ParseJoin("10.0.0.2; quit", out, sizeof(out)), "no commands");
	CHECK(!TouchLAN_ParseJoin("\"x\"", out, sizeof(out)) && !TouchLAN_ParseJoin("", out, sizeof(out)), "junk");
	CHECK(TouchLAN_IsLanIPv4(ip(25, 1, 2, 3)) && TouchLAN_IsLanIPv4(ip(26, 1, 2, 3)), "Hamachi, Radmin");
	{
		const unsigned char ts[16] = {0xfd, 0x7a, 0x11, 0x5c, 0xa1, 0xe0}, ll[16] = {0xfe, 0x80}, g[16] = {0x2a, 0x01};
		CHECK(TouchLAN_UsableIPv6(ts) && TouchLAN_UsableIPv6(g) && !TouchLAN_UsableIPv6(ll), "IPv6 kinds");
	}
}

static void test_background(void)
{
	// local game: (local_server, lan_hosting, other_humans, remote_busy)
	CHECK(!TouchLAN_KeepRunningInBackground(1, 0, 0, 0), "bot match from Create pauses");
	CHECK(!TouchLAN_KeepRunningInBackground(1, 0, 0, 1), "bot match pauses even while something downloads");
	CHECK(TouchLAN_KeepRunningInBackground(1, 1, 0, 0), "Host screen game keeps running with nobody else in it");
	CHECK(TouchLAN_KeepRunningInBackground(1, 0, 1, 0), "a Create game another human joined keeps running");
	CHECK(TouchLAN_KeepRunningInBackground(0, 0, 0, 1), "connected, joining or downloading keeps going");
	CHECK(!TouchLAN_KeepRunningInBackground(0, 0, 0, 0), "idle menu waits");
}

int main(void)
{
	test_interfaces();
	test_sweep();
	test_join_string();
	test_background();
	test_broadcast();
	test_lan_address();
	test_qcstatus();
	test_reject();
	test_ports();
	if (failures)
	{
		printf("%d LAN logic checks failed\n", failures);
		return 1;
	}
	printf("LAN logic: all checks passed\n");
	return 0;
}
