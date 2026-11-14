/*
Godnotic: why a join failed, as pure functions (no engine headers) shared by
touch_joinfail.c and tests/joinfail/joinfail_test.c.

The engine ends a join in a handful of ways and says so only in
cl_connect_status and the console:

  "Connect: failed, no reply"       ten getchallenge packets, no accept
  "Connect: failed, no network"     no socket for that address family
  "Connect: rejected, <text>"       the server's reject
  "Disconnect: <text>"              dropped, or a Host_Error, during the signon
  "Disconnected"                    the player's own cancel

TouchJoinFail_Classify turns that, plus what the join saw on the way (did the
server answer a challenge, was the connection ever up), into a kind and two
sentences a player can act on.
*/
#ifndef TOUCH_JOINFAIL_LOGIC_H
#define TOUCH_JOINFAIL_LOGIC_H

#include <stdio.h>
#include <string.h>

// the engine forbids snprintf (common.h); the tests have no dpsnprintf
#ifndef TOUCH_SNPRINTF
#ifdef snprintf
#define TOUCH_SNPRINTF dpsnprintf
#else
#define TOUCH_SNPRINTF snprintf
#endif
#endif

#define TOUCHJOINFAIL_NONE      0   // not a failure (cancelled, password prompt, still going)
#define TOUCHJOINFAIL_TIMEOUT   1   // the server never answered
#define TOUCHJOINFAIL_BUSY      2   // it answered the challenge and then ignored the connect
#define TOUCHJOINFAIL_REFUSED   3   // reject <text>
#define TOUCHJOINFAIL_NETWORK   4   // no socket / no route for the address family
#define TOUCHJOINFAIL_DROPPED   5   // the server closed the connection during the signon
#define TOUCHJOINFAIL_FILES     6   // a map or pack this client could not get
#define TOUCHJOINFAIL_ERROR     7   // a Host_Error while joining

// A stock DarkPlaces server ignores every connect from an IP address (any
// port) for net_connectfloodblockingtimeout (5 s) after a connect from that
// address, and each ignored connect renews the block. A client that keeps
// asking once a second is never let in. After this many challenges answered
// with a connect and no accept, the client stays quiet for a little longer
// than the block, then asks again.
#define TOUCHJOIN_BACKOFF_AFTER    3
#define TOUCHJOIN_BACKOFF_SECONDS  5.6
#define TOUCHJOIN_BACKOFF_TRIES    5   // tries left after the pause

static inline const char *TouchJoinFail_KindName(int kind)
{
	switch (kind)
	{
	case TOUCHJOINFAIL_TIMEOUT: return "timeout";
	case TOUCHJOINFAIL_BUSY: return "busy";
	case TOUCHJOINFAIL_REFUSED: return "refused";
	case TOUCHJOINFAIL_NETWORK: return "network";
	case TOUCHJOINFAIL_DROPPED: return "dropped";
	case TOUCHJOINFAIL_FILES: return "files";
	case TOUCHJOINFAIL_ERROR: return "error";
	default: return "";
	}
}

// Server text into something a label and a cvar can carry: one line, no
// quotes, no control characters, at most size - 1 bytes.
static inline void TouchJoinFail_Clean(char *out, size_t size, const char *in)
{
	size_t n = 0;
	if (!size)
		return;
	for (; in && *in && n + 1 < size; in++)
	{
		unsigned char c = (unsigned char)*in;
		if (c == '\n' || c == '\r' || c == '\t')
			c = ' ';
		if (c < 32 || c == '"' || c == ';' || c == '$' || c == '\\')
			continue;
		if (c == ' ' && (n == 0 || out[n - 1] == ' '))
			continue;
		out[n++] = (char)c;
	}
	while (n && out[n - 1] == ' ')
		n--;
	out[n] = 0;
}

/*
status      cl_connect_status when the join ended
nchallenges challenges this join answered with a connect
connected   the connection was up (accept received) before it ended
ipv6        the address is an IPv6 one
text        the headline, detail the second line (the server's words, or what to do)
Returns the kind.
*/
static inline int TouchJoinFail_Classify(const char *status, int nchallenges, int connected, int ipv6,
	const char *address, char *text, size_t textsize, char *detail, size_t detailsize)
{
	char clean[200];
	text[0] = detail[0] = 0;
	if (!status)
		return TOUCHJOINFAIL_NONE;
	if (!strcmp(status, "Connect: wrong password") || !strcmp(status, "Connect: this game needs a password"))
		return TOUCHJOINFAIL_NONE; // the password prompt has it
	if (!strncmp(status, "Connect: failed, no network", 27) || !strcmp(status, "No network"))
	{
		TOUCH_SNPRINTF(text, textsize, "No network connection for %s", address);
		TOUCH_SNPRINTF(detail, detailsize, "%s", ipv6
			? "This is an IPv6 address and this device has no IPv6 connection right now."
			: "Check that Wi-Fi or mobile data is on, then try again.");
		return TOUCHJOINFAIL_NETWORK;
	}
	if (!strncmp(status, "Connect: failed", 15))
	{
		if (nchallenges > 0)
		{
			TOUCH_SNPRINTF(text, textsize, "%s answered but did not let you in", address);
			TOUCH_SNPRINTF(detail, detailsize, "It is busy, or it limits joins from your network (someone on the same connection just joined). Try again in a few seconds.");
			return TOUCHJOINFAIL_BUSY;
		}
		TOUCH_SNPRINTF(text, textsize, "No answer from %s", address);
		TOUCH_SNPRINTF(detail, detailsize, "The server is offline or restarting, or this network cannot reach it. Waited 10 seconds.");
		return TOUCHJOINFAIL_TIMEOUT;
	}
	if (!strncmp(status, "Connect: rejected, ", 19))
	{
		TouchJoinFail_Clean(clean, sizeof(clean), status + 19);
		TOUCH_SNPRINTF(text, textsize, "%s refused the join", address);
		TOUCH_SNPRINTF(detail, detailsize, "The server says: %s", clean[0] ? clean : "(no reason given)");
		return TOUCHJOINFAIL_REFUSED;
	}
	if (connected && !strncmp(status, "Disconnect: ", 12))
	{
		const char *r = status + 12;
		if (!strncmp(r, "Host_Error: ", 12))
		{
			const char *map = strstr(r, "cannot load map ");
			if (map)
			{
				char name[64];
				size_t n = strcspn(map + 16, ". ");
				if (n >= sizeof(name)) n = sizeof(name) - 1;
				memcpy(name, map + 16, n);
				name[n] = 0;
				TouchJoinFail_Clean(clean, sizeof(clean), name);
				TOUCH_SNPRINTF(text, textsize, "Missing files: could not get the map %s", clean);
				TOUCH_SNPRINTF(detail, detailsize, "The download from the server did not finish. Check the connection and retry; the part already downloaded is kept.");
				return TOUCHJOINFAIL_FILES;
			}
			TouchJoinFail_Clean(clean, sizeof(clean), r + 12);
			TOUCH_SNPRINTF(text, textsize, "The join stopped with an error");
			TOUCH_SNPRINTF(detail, detailsize, "%s", clean);
			return TOUCHJOINFAIL_ERROR;
		}
		TouchJoinFail_Clean(clean, sizeof(clean), r);
		TOUCH_SNPRINTF(text, textsize, "%s closed the connection while joining", address);
		TOUCH_SNPRINTF(detail, detailsize, "The server says: %s", clean[0] ? clean : "(no reason given)");
		return TOUCHJOINFAIL_DROPPED;
	}
	return TOUCHJOINFAIL_NONE; // "Disconnected": the player's cancel
}

// The back-off: 1 when the client should now stay quiet for
// TOUCHJOIN_BACKOFF_SECONDS (once per join).
static inline int TouchJoin_ShouldBackOff(int nchallenges, int already)
{
	return !already && nchallenges >= TOUCHJOIN_BACKOFF_AFTER;
}

#endif
