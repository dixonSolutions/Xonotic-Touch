/*
Godnotic: the timeline of one join or local start (touch_jointime.h,
docs/JOIN_SPEED.md). scripts/bench-join.sh reads what this prints.
*/

#include "quakedef.h"
#include "touch_jointime.h"
#include "touch_loadtime.h"
#include "libcurl.h"

static cvar_t touch_jointime = {CF_CLIENT, "touch_jointime", "0", "print the timeline of each join and local start, phase by phase (Godnotic): 1 = at its first game frame and once it is playable"};
static cvar_t touch_jointime_last = {CF_CLIENT | CF_READONLY, "_touch_jointime", "", "the last join's timeline, \"event=ms ...\" (for scripts)"};
static cvar_t touch_jointime_serial = {CF_CLIENT | CF_READONLY, "_touch_jointime_serial", "0", "counts joins whose timeline is complete (for scripts that wait for one)"};

static const char *tjt_names[TJT_COUNT] =
{
	"tap",
	"spawn_begin",
	"spawn_end",
	"challenge_tx",
	"challenge_rx",
	"connect_tx",
	"accept",
	"signon1",
	"curl_begin",
	"curl_end",
	"csprogs_begin",
	"csprogs_end",
	"precache_end",
	"signon2",
	"signon3",
	"csqc_begin",
	"csqc_end",
	"signon4",
	"first_frame",
	"playable",
};

#define TJT_SMOOTH_FRAMES 10
#define TJT_SMOOTH_MS 0.05
#define TJT_PLAYABLE_GIVEUP 15.0
#define TJT_ABANDON 900.0

static struct
{
	qbool active;
	qbool first_printed;
	const char *started_by;
	double t0;
	double stamp[TJT_COUNT];
	char csprogs[16];
	long long csprogs_bytes;
	double last_frame;
	double smooth_start;
	int smooth;
	qbool curl_seen;
}
tjt;

double TouchJoinTime_Since(void)
{
	return tjt.active ? (Sys_DirtyTime() - tjt.t0) * 1000.0 : -1;
}

double TouchJoinTime_RTT(void)
{
	if (!tjt.active || tjt.stamp[TJT_CHALLENGE_TX] < 0 || tjt.stamp[TJT_CHALLENGE_RX] < 0)
		return -1;
	return (tjt.stamp[TJT_CHALLENGE_RX] - tjt.stamp[TJT_CHALLENGE_TX]) * 0.001;
}

static void TJT_Start(const char *by, double now)
{
	int i;
	memset(&tjt, 0, sizeof(tjt));
	tjt.active = true;
	tjt.started_by = by;
	tjt.t0 = now;
	for (i = 0; i < TJT_COUNT; i++)
		tjt.stamp[i] = -1;
}

static void TJT_MarkAt(int event, double now)
{
	if (!tjt.active || event < 0 || event >= TJT_COUNT || tjt.stamp[event] >= 0)
		return;
	tjt.stamp[event] = (now - tjt.t0) * 1000.0;
}

void TouchJoinTime_Mark(int event)
{
	if (cls.state == ca_dedicated)
		return;
	TJT_MarkAt(event, Sys_DirtyTime());
}

void TouchJoinTime_Signon(int stage)
{
	static const int ev[SIGNONS + 1] = {-1, TJT_SIGNON1, TJT_SIGNON2, TJT_SIGNON3, TJT_SIGNON4};
	if (stage > 0 && stage <= SIGNONS)
		TouchJoinTime_Mark(ev[stage]);
	// signon 2 answers the prespawn the client sent when its precache was done
	if (stage >= 2)
		TouchJoinTime_Mark(TJT_PRECACHE_END);
}

void TouchJoinTime_Tap(void)
{
	double now = Sys_DirtyTime();
	if (cls.state == ca_dedicated)
		return;
	TJT_Start("tap", now);
	TJT_MarkAt(TJT_TAP, now);
}

// a tap that has not got its server's accept yet owns the connect it causes
static qbool TJT_Fresh(double now)
{
	return tjt.active && tjt.stamp[TJT_ACCEPT] < 0 && now - tjt.t0 < 10;
}

void TouchJoinTime_Connect(void)
{
	double now = Sys_DirtyTime();
	if (cls.state == ca_dedicated || TJT_Fresh(now))
		return;
	TJT_Start("connect", now);
}

void TouchJoinTime_CSProgsSource(const char *source, long long bytes)
{
	if (!tjt.active)
		return;
	dp_strlcpy(tjt.csprogs, source, sizeof(tjt.csprogs));
	tjt.csprogs_bytes = bytes;
}

void TouchJoinTime_Phase(int phase, qbool begin)
{
	double now;
	if (cls.state == ca_dedicated)
		return;
	now = Sys_DirtyTime();
	if (phase == TLT_SPAWN)
	{
		// a map command from the console or a menu: the start
		if (begin && !TJT_Fresh(now) && !(tjt.active && tjt.stamp[TJT_SPAWN_BEGIN] < 0 && tjt.stamp[TJT_ACCEPT] < 0))
			TJT_Start("spawn", now);
		TJT_MarkAt(begin ? TJT_SPAWN_BEGIN : TJT_SPAWN_END, now);
	}
	else if (phase == TLT_CSQC)
		TJT_MarkAt(begin ? TJT_CSQC_BEGIN : TJT_CSQC_END, now);
}

void TouchJoinTime_Packet(const void *data, int length, int sent)
{
	const char *s = (const char *)data;
	double now;
	if (!tjt.active || tjt.stamp[TJT_ACCEPT] >= 0 || cls.state == ca_dedicated || length < 10)
		return;
	if ((unsigned char)s[0] != 255 || (unsigned char)s[1] != 255 || (unsigned char)s[2] != 255 || (unsigned char)s[3] != 255)
		return;
	s += 4;
	length -= 4;
	now = Sys_DirtyTime();
	if (sent)
	{
		if (length >= 12 && !strncmp(s, "getchallenge", 12))
			TJT_MarkAt(TJT_CHALLENGE_TX, now);
		else if (length >= 7 && !strncmp(s, "connect", 7))
			TJT_MarkAt(TJT_CONNECT_TX, now);
	}
	else
	{
		if (length >= 9 && !strncmp(s, "challenge", 9))
			TJT_MarkAt(TJT_CHALLENGE_RX, now);
		else if (length >= 6 && !strncmp(s, "accept", 6))
			TJT_MarkAt(TJT_ACCEPT, now);
	}
}

static void TJT_Report(qbool final)
{
	char line[1536];
	size_t n = 0;
	int i;
	const char *src = tjt.csprogs[0] ? tjt.csprogs : "none";
	for (i = 0; i < TJT_COUNT; i++)
		if (tjt.stamp[i] >= 0)
			n += dpsnprintf(line + n, sizeof(line) - n, "%s%s=%.0f", n ? " " : "", tjt_names[i], tjt.stamp[i]);
	dpsnprintf(line + n, sizeof(line) - n, " csprogs=%s csprogs_bytes=%lld by=%s map=%s", src, tjt.csprogs_bytes, tjt.started_by ? tjt.started_by : "?", cl.worldbasename);
	if (final)
	{
		Cvar_SetQuick(&touch_jointime_last, line);
		Cvar_SetValueQuick(&touch_jointime_serial, touch_jointime_serial.integer + 1);
	}
	if (!touch_jointime.integer)
		return;
	Con_Printf("JOINTIME %s first_frame=%.0f playable=%.0f csprogs=%s by=%s map=%s\n", final ? "done" : "first_frame",
		tjt.stamp[TJT_FIRST_FRAME], tjt.stamp[TJT_PLAYABLE], src, tjt.started_by ? tjt.started_by : "?", cl.worldbasename);
	if (final)
		for (i = 0; i < TJT_COUNT; i++)
			if (tjt.stamp[i] >= 0)
				Con_Printf("JOINTIME   %-14s %9.1f ms\n", tjt_names[i], tjt.stamp[i]);
	if (final)
		Con_Printf("JOINTIME line %s\n", line);
}

void TouchJoinTime_Frame(double now)
{
	double dt;
	if (!tjt.active || cls.state == ca_dedicated)
		return;
	if (now - tjt.t0 > TJT_ABANDON || (cls.state == ca_disconnected && !cls.connect_trying && tjt.stamp[TJT_ACCEPT] >= 0 && !sv.active))
	{
		// failed or abandoned: nothing to report
		tjt.active = false;
		return;
	}
	if (cls.state == ca_connected)
	{
		if (cls.signon >= 1)
			TJT_MarkAt(TJT_SIGNON1, now);
		if (cls.signon >= 2)
			TJT_MarkAt(TJT_SIGNON2, now);
		if (cls.signon >= 3)
			TJT_MarkAt(TJT_SIGNON3, now);
		if (cls.signon >= SIGNONS)
			TJT_MarkAt(TJT_SIGNON4, now);
		if (cls.signon >= 1 && cl.loadfinished)
			TJT_MarkAt(TJT_PRECACHE_END, now);
		// the server's packs (curl --forthismap), not other HTTP traffic
		if (Curl_Have_forthismap())
		{
			tjt.curl_seen = true;
			TJT_MarkAt(TJT_CURL_BEGIN, now);
		}
		else if (tjt.curl_seen)
			TJT_MarkAt(TJT_CURL_END, now);
		if (cls.qw_downloadname[0] && !strcmp(cls.qw_downloadname, csqc_progname.string))
		{
			// the server's own download (also after an HTTP fetch failed)
			TJT_MarkAt(TJT_CSPROGS_BEGIN, now);
			TouchJoinTime_CSProgsSource("udp", cls.qw_downloadmemorymaxsize);
		}
		else if (tjt.stamp[TJT_CSPROGS_BEGIN] >= 0 && !strcmp(tjt.csprogs, "udp"))
			TJT_MarkAt(TJT_CSPROGS_END, now);
	}
	if (tjt.stamp[TJT_FIRST_FRAME] < 0)
	{
		if (cls.state == ca_connected && cls.signon == SIGNONS && cl.worldmodel && !scr_loading)
		{
			TJT_MarkAt(TJT_FIRST_FRAME, now);
			if (!tjt.csprogs[0])
				TouchJoinTime_CSProgsSource(cl_touch_csqc_active ? "local" : "cache", 0);
			tjt.last_frame = now;
			tjt.smooth = 0;
			tjt.smooth_start = now;
			TJT_Report(false);
		}
		return;
	}
	// playable: the first frame after which TJT_SMOOTH_FRAMES frames in a
	// row took under 50 ms (the first frames load what they draw first)
	dt = now - tjt.last_frame;
	if (dt >= TJT_SMOOTH_MS)
	{
		tjt.smooth = 0;
		tjt.smooth_start = now;
	}
	else if (++tjt.smooth >= TJT_SMOOTH_FRAMES || now - tjt.t0 - tjt.stamp[TJT_FIRST_FRAME] / 1000.0 > TJT_PLAYABLE_GIVEUP)
	{
		TJT_MarkAt(TJT_PLAYABLE, tjt.smooth_start);
		TJT_Report(true);
		tjt.active = false;
	}
	tjt.last_frame = now;
}

void TouchJoinTime_Init(void)
{
	Cvar_RegisterVariable(&touch_jointime);
	Cvar_RegisterVariable(&touch_jointime_last);
	Cvar_RegisterVariable(&touch_jointime_serial);
}
