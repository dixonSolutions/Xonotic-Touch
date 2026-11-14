/*
Godnotic: the fast server list's rules as pure functions (no engine headers),
shared by touch_srvlist.c and tests/srvlist/srvlist_logic_test.c.

  - the saved list: one server per line, what a row shows and how often the
    server failed to answer lately;
  - the query budget: a token bucket on the wall clock;
  - how many times a server is asked and how long it is waited for;
  - percentiles for net_slist_stats.
*/
#ifndef TOUCH_SRVLIST_LOGIC_H
#define TOUCH_SRVLIST_LOGIC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// the engine forbids snprintf (common.h); the tests have no dpsnprintf
#ifndef TOUCH_SNPRINTF
#ifdef snprintf
#define TOUCH_SNPRINTF dpsnprintf
#else
#define TOUCH_SNPRINTF snprintf
#endif
#endif

// ---------------------------------------------------------------------------
// Numbers (docs/NETWORK.md has the reasons)
// ---------------------------------------------------------------------------
#define TOUCHSL_RATE_DEFAULT      1500    // getstatus packets a second at most, first open and Refresh
#define TOUCHSL_BURST_DEFAULT     48      // and at most this many in one go (one frame)
#define TOUCHSL_RATE_BACKGROUND   120     // the menu's 10 s refresh timer: no faster than the stock list
#define TOUCHSL_BURST_BACKGROUND  8
#define TOUCHSL_SAME_HOST_GAP     0.05    // seconds between two queries to one IP address (hosts with many ports)
#define TOUCHSL_RETRY_INTERVAL    1.1     // a server ignores a second getstatus from one address for 1 s
#define TOUCHSL_MIN_REQUERY       20.0    // background: a server that answered is left alone this long
#define TOUCHSL_MASTER_WAIT       1.5     // a master that has said nothing by then holds nothing up
#define TOUCHSL_FILE_VERSION      "godnotic-serverlist 1"
#define TOUCHSL_MAX_RECORDS       1024
#define TOUCHSL_MAX_AGE           (7 * 86400)   // a server not seen for a week is forgotten
#define TOUCHSL_MAX_FAILS         3             // sessions in a row without an answer before that

// ---------------------------------------------------------------------------
// The saved list
// ---------------------------------------------------------------------------
typedef struct touchsl_record_s
{
	char cname[128];
	int ping, numplayers, numbots, maxplayers, protocol, gameversion;
	long long seen;      // unix time of the last answer
	int fails;           // sessions without an answer since
	int failed_now;      // not saved: this session already counted one
	char name[128], map[32], mod[32], game[32], qcstatus[128];
} touchsl_record_t;

// a field without the characters the line format uses
static inline void TouchSL_CleanField(char *out, size_t size, const char *in)
{
	size_t n = 0;
	for (; in && *in && n + 1 < size; in++)
	{
		unsigned char c = (unsigned char)*in;
		out[n++] = (c == '\t' || c == '\n' || c == '\r') ? ' ' : (char)c;
	}
	out[n] = 0;
}

static inline int TouchSL_FormatRecord(char *out, size_t size, const touchsl_record_t *r)
{
	char name[128], map[32], mod[32], game[32], qc[128];
	TouchSL_CleanField(name, sizeof(name), r->name);
	TouchSL_CleanField(map, sizeof(map), r->map);
	TouchSL_CleanField(mod, sizeof(mod), r->mod);
	TouchSL_CleanField(game, sizeof(game), r->game);
	TouchSL_CleanField(qc, sizeof(qc), r->qcstatus);
	return TOUCH_SNPRINTF(out, size, "%s\t%d\t%d\t%d\t%d\t%d\t%d\t%lld\t%d\t%s\t%s\t%s\t%s\t%s\n",
		r->cname, r->ping, r->numplayers, r->numbots, r->maxplayers, r->protocol, r->gameversion,
		r->seen, r->fails, name, map, mod, game, qc);
}

// One line (NUL-terminated, no newline) into a record; 0 when it is not one.
// The line is modified.
static inline int TouchSL_ParseRecord(char *line, touchsl_record_t *r)
{
	char *f[14];
	int n = 0;
	char *p = line;
	memset(r, 0, sizeof(*r));
	while (n < 14)
	{
		f[n++] = p;
		p = strchr(p, '\t');
		if (!p)
			break;
		*p++ = 0;
	}
	if (n != 14 || !f[0][0] || strlen(f[0]) >= sizeof(r->cname))
		return 0;
	// an address: digits, hex, dots, colons, brackets only
	for (p = f[0]; *p; p++)
		if (!strchr("0123456789abcdefABCDEF.:[]", *p))
			return 0;
	TOUCH_SNPRINTF(r->cname, sizeof(r->cname), "%s", f[0]);
	r->ping = atoi(f[1]);
	r->numplayers = atoi(f[2]);
	r->numbots = atoi(f[3]);
	r->maxplayers = atoi(f[4]);
	r->protocol = atoi(f[5]);
	r->gameversion = atoi(f[6]);
	r->seen = atoll(f[7]);
	r->fails = atoi(f[8]);
	TOUCH_SNPRINTF(r->name, sizeof(r->name), "%s", f[9]);
	TOUCH_SNPRINTF(r->map, sizeof(r->map), "%s", f[10]);
	TOUCH_SNPRINTF(r->mod, sizeof(r->mod), "%s", f[11]);
	TOUCH_SNPRINTF(r->game, sizeof(r->game), "%s", f[12]);
	TOUCH_SNPRINTF(r->qcstatus, sizeof(r->qcstatus), "%s", f[13]);
	if (r->ping < 0 || r->ping > 100000 || r->numplayers < 0 || r->numplayers > 4096 || r->maxplayers < 0 || r->maxplayers > 4096 || r->fails < 0)
		return 0;
	if (r->numbots > r->numplayers)
		r->numbots = r->numplayers;
	return 1;
}

// Shown at once when the list opens: answered at the last refresh, not too old.
static inline int TouchSL_RecordShown(const touchsl_record_t *r, long long now)
{
	return r->fails == 0 && r->ping > 0 && now - r->seen < TOUCHSL_MAX_AGE;
}

// Kept in the file at all.
static inline int TouchSL_RecordKept(const touchsl_record_t *r, long long now)
{
	return r->fails < TOUCHSL_MAX_FAILS && now - r->seen < TOUCHSL_MAX_AGE;
}

// ---------------------------------------------------------------------------
// The query budget
// ---------------------------------------------------------------------------
typedef struct touchsl_bucket_s
{
	double tokens, last;
} touchsl_bucket_t;

// How many queries may go out now. A stall (a long frame, a paused list) does
// not pile up more than one burst.
static inline int TouchSL_Budget(touchsl_bucket_t *b, double now, double rate, double burst)
{
	double dt = now - b->last;
	int n;
	if (rate < 1) rate = 1;
	if (burst < 1) burst = 1;
	if (b->last <= 0 || dt < 0)
		dt = 0, b->tokens = burst;   // a fresh bucket starts full
	b->last = now;
	b->tokens += dt * rate;
	if (b->tokens > burst)
		b->tokens = burst;
	n = (int)b->tokens;
	return n;
}

static inline void TouchSL_Spend(touchsl_bucket_t *b, int n)
{
	b->tokens -= n;
}

// ---------------------------------------------------------------------------
// Tries
// ---------------------------------------------------------------------------
#define TOUCHSL_KNOWN_ALIVE  1   // answered at the last refresh (from the saved list)
#define TOUCHSL_KNOWN_DEAD   2   // did not, once or more
#define TOUCHSL_UNKNOWN      0   // a master named it and that is all we know

// A server that was there a minute ago is worth three asks, one a master
// lists but nobody has heard from two, one that has already failed one.
static inline int TouchSL_Tries(int known, int maxtries, int background)
{
	int n;
	if (maxtries < 1) maxtries = 1;
	if (maxtries > 8) maxtries = 8;
	n = known == TOUCHSL_KNOWN_ALIVE ? maxtries : known == TOUCHSL_KNOWN_DEAD ? 1 : (maxtries < 2 ? maxtries : 2);
	if (background && n > 2)
		n = 2;
	return n;
}

// After the last ask a server has maxping (answers slower than that are not
// listed anyway) and a little for the frame it arrives in.
static inline double TouchSL_FinalWait(double maxping_ms)
{
	if (maxping_ms < 50) maxping_ms = 50;
	if (maxping_ms > 2000) maxping_ms = 2000;
	return maxping_ms / 1000.0 + 0.1;
}

// ---------------------------------------------------------------------------
// Percentiles: the time by which pct percent of the n answers were in.
// t is sorted in place.
// ---------------------------------------------------------------------------
static int touchsl_cmp_float(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;
	return x < y ? -1 : x > y;
}

static inline float TouchSL_Percentile(float *t, int n, int pct)
{
	int k;
	if (n <= 0)
		return 0;
	qsort(t, n, sizeof(float), touchsl_cmp_float);
	k = (n * pct + 99) / 100 - 1;
	if (k < 0) k = 0;
	if (k >= n) k = n - 1;
	return t[k];
}

// "12 servers · 5 players" (the dot is U+00B7)
static inline void TouchSL_Summary(char *out, size_t size, int servers, int players)
{
	TOUCH_SNPRINTF(out, size, "%d server%s \xC2\xB7 %d player%s", servers, servers == 1 ? "" : "s", players, players == 1 ? "" : "s");
}

#endif
