/* touch_joinfail_logic.h: how a failed join is classified and worded, and the
   connect back-off rule. Run by scripts/test-joinfail.sh. */
#include "touch_joinfail_logic.h"

static int fails;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL %s (line %d)\n", what, __LINE__); fails++; } } while (0)

static int classify(const char *status, int challenges, int connected, int ipv6, char *text, char *detail)
{
	return TouchJoinFail_Classify(status, challenges, connected, ipv6, ipv6 ? "[2001:db8::1]:26000" : "203.0.113.7:26000", text, 200, detail, 256);
}

int main(void)
{
	char text[200], detail[256], clean[32];

	CHECK(classify("Connect: failed, no reply", 0, 0, 0, text, detail) == TOUCHJOINFAIL_TIMEOUT, "silence is a timeout");
	CHECK(strstr(text, "No answer from 203.0.113.7:26000") != NULL, "timeout names the server");
	CHECK(classify("Connect: failed, no reply", 4, 0, 0, text, detail) == TOUCHJOINFAIL_BUSY, "challenges answered, no accept: busy");
	CHECK(strstr(text, "answered but did not let you in") != NULL, "busy says the server answered");
	CHECK(classify("Connect: rejected, Server is full.\n", 1, 0, 0, text, detail) == TOUCHJOINFAIL_REFUSED, "reject is refused");
	CHECK(!strcmp(detail, "The server says: Server is full."), "refused carries the server's text");
	CHECK(classify("Connect: rejected, ", 1, 0, 0, text, detail) == TOUCHJOINFAIL_REFUSED && strstr(detail, "no reason given"), "empty reject text");
	CHECK(classify("Connect: failed, no network", 0, 0, 1, text, detail) == TOUCHJOINFAIL_NETWORK, "no socket is network");
	CHECK(strstr(detail, "IPv6") != NULL, "an IPv6 address without IPv6 says so");
	CHECK(classify("Connect: failed, no network", 0, 0, 0, text, detail) == TOUCHJOINFAIL_NETWORK && !strstr(detail, "IPv6"), "IPv4 network text");
	CHECK(classify("Disconnect: Host_Error: Godnotic: cannot load map solarium. Enable server downloads and reconnect.", 1, 1, 0, text, detail) == TOUCHJOINFAIL_FILES, "a missing map is missing files");
	CHECK(strstr(text, "map solarium") != NULL, "missing files names the map");
	CHECK(classify("Disconnect: Host_Error: CL_ParseServerMessage: Illegible server message", 1, 1, 0, text, detail) == TOUCHJOINFAIL_ERROR, "another Host_Error is an error");
	CHECK(classify("Disconnect: Server disconnected", 1, 1, 0, text, detail) == TOUCHJOINFAIL_DROPPED, "a drop during the signon");
	CHECK(!strcmp(detail, "The server says: Server disconnected"), "dropped carries the reason");
	// not failures
	CHECK(classify("Disconnected", 1, 1, 0, text, detail) == TOUCHJOINFAIL_NONE, "the player's cancel is not a failure");
	CHECK(classify("Disconnect: kicked", 0, 0, 0, text, detail) == TOUCHJOINFAIL_NONE, "a disconnect that was never connected is not reported");
	CHECK(classify("Connect: wrong password", 1, 0, 0, text, detail) == TOUCHJOINFAIL_NONE, "the password prompt owns a wrong password");
	CHECK(classify("Connect: this game needs a password", 1, 0, 0, text, detail) == TOUCHJOINFAIL_NONE, "and a missing one");
	CHECK(classify("Connect: signon stage 2 of 4", 1, 1, 0, text, detail) == TOUCHJOINFAIL_NONE, "a status that is not an end");
	// server text cannot smuggle a command or a quote into a cvar or a label
	CHECK(classify("Connect: rejected, bye\"; quit; $rcon_password \\x\n\tnow", 1, 0, 0, text, detail) == TOUCHJOINFAIL_REFUSED, "hostile reject");
	CHECK(!strpbrk(detail, "\";$\\\n\t"), "hostile reject text is cleaned");
	TouchJoinFail_Clean(clean, sizeof(clean), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	CHECK(strlen(clean) == sizeof(clean) - 1, "clean respects the buffer");
	// names for the front ends
	CHECK(!strcmp(TouchJoinFail_KindName(TOUCHJOINFAIL_BUSY), "busy") && !strcmp(TouchJoinFail_KindName(TOUCHJOINFAIL_FILES), "files"), "kind names");
	// the back-off: once, after three ignored connects, and longer than the server's 5 s block
	CHECK(!TouchJoin_ShouldBackOff(2, 0), "two ignored connects: keep asking");
	CHECK(!TouchJoin_ShouldBackOff(3, 0), "three: keep asking, as fast as stock");
	CHECK(TouchJoin_ShouldBackOff(4, 0), "four: back off");
	CHECK(!TouchJoin_ShouldBackOff(9, 1), "only once per join");
	CHECK(TOUCHJOIN_BACKOFF_SECONDS > 5.0, "the pause outlasts net_connectfloodblockingtimeout");

	if (fails)
		return 1;
	printf("ok   joinfail logic\n");
	return 0;
}
