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

int main(void)
{
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
