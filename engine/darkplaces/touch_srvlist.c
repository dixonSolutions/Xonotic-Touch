/*
Godnotic: the fast public server list (docs/NETWORK.md, "The server list").

What the stock list does on Refresh: ask the masters, wait until every one
has sent its end-of-list (or net_slist_timeout, 4 s, when one is silent),
then ask 128 servers a second, two a frame at most, every server three times
whether it answered or not; a row appears when its server answers.

What this does (net_slist_fast 1):

  - The saved list. Every server that answered is kept with what its row
    shows (touch_serverlist_info.txt in the user directory). Opening the list
    puts those rows up before a single packet is answered, marked stale until
    their server answers; one that stays silent is taken out again.
  - No waiting for masters. A server is asked in the frame a master names it;
    favourites and the saved servers are asked before the masters answer,
    in the frame the list opens.
  - The seed. With nothing saved yet (the first open of a new install) the
    servers the build carries (touch_srvlist_seed.h, written by
    scripts/srvlist-seed.py) take the saved list's place: asked at once,
    first, before the masters have answered. They are not shown until they
    answer.
  - A budget on the clock. TOUCHSL_RATE_DEFAULT queries a second, at most
    TOUCHSL_BURST_DEFAULT at once (the whole first wave in one frame),
    whatever the frame rate; at most TOUCHSL_HOST_BURST at once to one IP
    address and TOUCHSL_HOST_RATE a second after that; nothing while a join
    runs. The sockets get buffers big enough for a first wave's answers.
  - Asking again only where it helps: the silent servers, 1.1 s later (a
    server ignores a second getstatus within 1 s), three times for a server
    that was there last time, twice for an unknown one, once for one that
    already failed.
  - The menu's periodic refresh (every 10 s in the QuakeC list) runs at the
    stock pace and leaves a server that answered alone for 20 s.

The protocol is the stock one, packet for packet: getserversExt to the
masters (netconn.c still sends those), getstatus with the timestamp challenge
to the servers. net_slist_fast 0 gives the stock scheduler back.

It also measures every refresh, fast or stock (net_slist_stats), publishes
"N servers · M players" (_slist_summary and friends) and holds the list still
while a finger is on it.
*/
#include "quakedef.h"
#include "netconn.h"
#include "touch_lan.h"
#include "touch_lan_logic.h"
#include "touch_srvlist.h"
#include "touch_srvlist_logic.h"
#include "touch_joinfail.h"
#include <time.h>

#ifdef CONFIG_MENU

#include "touch_srvlist_seed.h"
#if !defined(WIN32) && !defined(__EMSCRIPTEN__)
#include <sys/socket.h>
#define TOUCHSL_SETSOCKOPT 1
#endif

extern uint8_t serverlist_querystage;
#define SLIST_QUERYSTAGE_QWMASTERS 2
#define SLIST_QUERYSTAGE_SERVERS 4

#define INFO_FILE "touch_serverlist_info.txt"

static cvar_t net_slist_fast = {CF_CLIENT | CF_ARCHIVE, "net_slist_fast", "1", "Godnotic: 1 asks servers the moment a master names them, at net_slist_fast_rate, and shows the saved list at once; 0 is the stock DarkPlaces server list; 2 is a model of Insanotic's scheduler, for comparison (docs/NETWORK.md)"};
static cvar_t net_slist_fast_rate = {CF_CLIENT | CF_ARCHIVE, "net_slist_fast_rate", "4000", "Godnotic: server queries a second at most when the list is opened or refreshed by hand (bounded 20..8000)"};
static cvar_t net_slist_fast_burst = {CF_CLIENT | CF_ARCHIVE, "net_slist_fast_burst", "320", "Godnotic: server queries in one frame at most (bounded 1..512)"};
static cvar_t net_slist_seed = {CF_CLIENT, "net_slist_seed", "1", "Godnotic: with nothing saved yet, ask the public servers the build carries at once, before the masters answer (1); 0 not; a file name (in the user data directory, one address a line) uses that list instead (tests)"};
static cvar_t net_slist_saved = {CF_CLIENT | CF_ARCHIVE, "net_slist_saved", "1", "Godnotic: keep the server list between sessions and show it at once when the list opens (rows are marked stale until their server answers)"};
static cvar_t net_slist_hold = {CF_CLIENT | CF_ARCHIVE, "net_slist_hold", "1", "Godnotic: while a finger is on the menu the server list keeps its rows where they are (it catches up on release)"};
static cvar_t net_slist_stats_print = {CF_CLIENT, "net_slist_stats_print", "0", "Godnotic: print the net_slist_stats line when a refresh settles (tests)"};
static cvar_t slist_summary = {CF_CLIENT | CF_READONLY, "_slist_summary", "", "Godnotic: \"N servers · M players\" for the Multiplayer card; empty until something is known"};
static cvar_t slist_servers = {CF_CLIENT | CF_READONLY, "_slist_servers", "0", "Godnotic: servers in the list (answered, or saved and not yet refreshed)"};
static cvar_t slist_players = {CF_CLIENT | CF_READONLY, "_slist_players", "0", "Godnotic: human players on the servers in the list"};
static cvar_t slist_refreshing = {CF_CLIENT | CF_READONLY, "_slist_refreshing", "0", "Godnotic: 1 while a server list refresh is running"};
static cvar_t slist_saved = {CF_CLIENT | CF_READONLY, "_slist_saved", "0", "Godnotic: 1 while _slist_summary still counts rows of the saved list that have not answered yet"};

#define SF_STALE   1   // shown from the saved list, no answer yet this refresh
#define SF_DONE    2   // given up on for this refresh
#define SF_ANSWER  4   // answered this refresh
typedef struct
{
	unsigned char tries, maxtries, flags, known;
	unsigned iphash;
	double lastsend;     // realtime of the last query, 0 none
	double lastanswer;   // realtime of the last answer, 0 none
	float reply;         // seconds from the refresh to its first answer, 0 none
} side_t;

static side_t side[SERVERLIST_TOTALSIZE];
static unsigned nside;

// When each address was last asked, kept across resets of the list: a list
// that is reset twice in a second (the menu opening it while a refresh runs)
// must not ask anyone twice within the second a server ignores us for.
#define ASKED_SLOTS 2048
static struct { unsigned hash; double time; } asked[ASKED_SLOTS];

static unsigned name_hash(const char *cname)
{
	unsigned h = 2166136261u;
	for (; *cname; cname++)
		h = (h ^ (unsigned char)*cname) * 16777619u;
	return h ? h : 1;
}

static double asked_when(const char *cname)
{
	unsigned h = name_hash(cname);
	return asked[h % ASKED_SLOTS].hash == h ? asked[h % ASKED_SLOTS].time : 0;
}

static void asked_now(const char *cname, double now)
{
	unsigned h = name_hash(cname);
	asked[h % ASKED_SLOTS].hash = h;
	asked[h % ASKED_SLOTS].time = now;
}

static touchsl_record_t *recs;   // the saved list in memory
static int nrecs;
static qbool recs_loaded;

static struct
{
	qbool running, fast, background, preloaded_once, model;
	double t0;            // realtime the refresh began
	double settle_time;   // realtime it settled
	touchsl_bucket_t bucket;
	touchsl_hosts_t hosts; // queries per IP address
	unsigned serial;
	// stats of the running / last refresh
	int rows_at_open, sent, answers, removed, known_at_end, seeded, frames;
	float t_first_row, t_first_answer, t50, t95, t_settled;
	unsigned masters_asked, masters_replied;
	char mode[16];
	double next_summary;
	// the hold
	qbool finger, autopaused;
	int pause_before;
	double finger_up;
} sl;

static double now_real(void)
{
	return host.realtime + (Sys_DirtyTime() - host.dirtytime);
}

qbool TouchSrvList_Fast(void)
{
#ifdef __EMSCRIPTEN__
	return false;
#else
	return net_slist_fast.integer != 0;
#endif
}

// net_slist_fast 2: Insanotic's scheduler (its darkplaces/netconn.c,
// NetConn_QueryQueueFrame with net_slist_burst 1, net_slist_querywait 0,
// net_slist_queriesperframe 100, net_slist_timeout 4, net_slist_maxtries 3),
// run by this engine so that the two can be timed by the same clock on the
// same network. Nothing saved, no seed, no spacing: a model for measuring.
static qbool model_insanotic(void)
{
	return net_slist_fast.integer == 2;
}

unsigned TouchSrvList_Serial(void) { return sl.serial; }

qbool TouchSrvList_IsStale(unsigned n)
{
	return n < nside && (side[n].flags & SF_STALE) != 0;
}

qbool TouchSrvList_Hold(void)
{
	return net_slist_hold.integer && (sl.finger || (sl.finger_up && now_real() - sl.finger_up < 0.25));
}

// ---------------------------------------------------------------------------
// the saved list
// ---------------------------------------------------------------------------
static touchsl_record_t *rec_find(const char *cname)
{
	int i;
	for (i = 0; i < nrecs; i++)
		if (!strcmp(recs[i].cname, cname))
			return &recs[i];
	return NULL;
}

static void recs_load(void)
{
	qfile_t *f;
	char *buf, *line, *next;
	fs_offset_t size, len;
	long long now = (long long)time(NULL);
	if (recs_loaded)
		return;
	recs_loaded = true;
	if (!recs)
		recs = (touchsl_record_t *)Z_Malloc(sizeof(touchsl_record_t) * TOUCHSL_MAX_RECORDS);
	nrecs = 0;
	if (!net_slist_saved.integer)
		return;
	f = FS_OpenRealFile(INFO_FILE, "rb", true);
	if (!f)
		return;
	size = FS_FileSize(f);
	if (size <= 0 || size > 1024 * 1024)
	{
		FS_Close(f);
		return;
	}
	buf = (char *)Z_Malloc(size + 1);
	len = FS_Read(f, buf, size);
	FS_Close(f);
	buf[len > 0 ? len : 0] = 0;
	line = buf;
	next = strchr(line, '\n');
	if (next && !strncmp(line, TOUCHSL_FILE_VERSION, strlen(TOUCHSL_FILE_VERSION)))
	{
		for (line = next + 1; *line && nrecs < TOUCHSL_MAX_RECORDS; line = next)
		{
			next = strchr(line, '\n');
			if (next)
				*next++ = 0;
			else
				next = line + strlen(line);
			if (TouchSL_ParseRecord(line, &recs[nrecs]) && TouchSL_RecordKept(&recs[nrecs], now) && !rec_find(recs[nrecs].cname))
				nrecs++;
		}
	}
	Z_Free(buf);
}

static void recs_save(void)
{
	qfile_t *f;
	char line[700];
	int i, n;
	long long now = (long long)time(NULL);
	if (!net_slist_saved.integer || !recs)
		return;
	f = FS_OpenRealFile(INFO_FILE, "wb", true);
	if (!f)
		return;
	FS_Printf(f, "%s\n", TOUCHSL_FILE_VERSION);
	for (i = 0; i < nrecs; i++)
		if (TouchSL_RecordKept(&recs[i], now))
		{
			n = TouchSL_FormatRecord(line, sizeof(line), &recs[i]);
			if (n > 0 && n < (int)sizeof(line))
				FS_Write(f, line, n);
		}
	FS_Close(f);
}

// What this refresh learned goes into the saved list: answers refresh their
// record, a server that was asked and stayed silent has one more failure.
static void recs_merge(void)
{
	unsigned i;
	long long now = (long long)time(NULL);
	if (!recs)
		return;
	for (i = 0; i < serverlist_cachecount && i < nside; i++)
	{
		const serverlist_info_t *in = &serverlist_cache[i].info;
		touchsl_record_t *r = rec_find(in->cname);
		if (side[i].flags & SF_ANSWER)
		{
			if (!r)
			{
				if (nrecs >= TOUCHSL_MAX_RECORDS)
					continue;
				r = &recs[nrecs++];
				memset(r, 0, sizeof(*r));
				dp_strlcpy(r->cname, in->cname, sizeof(r->cname));
			}
			r->ping = in->ping;
			r->numplayers = in->numplayers;
			r->numbots = max(0, in->numbots);
			r->maxplayers = in->maxplayers;
			r->protocol = in->protocol;
			r->gameversion = in->gameversion;
			r->seen = now;
			r->fails = 0;
			r->failed_now = 0;
			dp_strlcpy(r->name, in->name, sizeof(r->name));
			dp_strlcpy(r->map, in->map, sizeof(r->map));
			dp_strlcpy(r->mod, in->mod, sizeof(r->mod));
			dp_strlcpy(r->game, in->game, sizeof(r->game));
			dp_strlcpy(r->qcstatus, in->qcstatus, sizeof(r->qcstatus));
		}
		else if (r && side[i].tries && (side[i].flags & SF_DONE))
		{
			// once per session: the menu refreshes every 10 s, and a server
			// that restarts must not be forgotten within half a minute
			if (!r->failed_now)
				r->fails++;
			r->failed_now = 1;
		}
		else if (!r && side[i].tries && (side[i].flags & SF_DONE) && nrecs < TOUCHSL_MAX_RECORDS)
		{
			// a master lists it and it is silent: remember that, so the next
			// refresh asks it once and last
			r = &recs[nrecs++];
			memset(r, 0, sizeof(*r));
			dp_strlcpy(r->cname, in->cname, sizeof(r->cname));
			r->seen = now;
			r->fails = 1;
			r->failed_now = 1;
		}
	}
}

static unsigned ip_hash(const char *cname)
{
	// the address without its port
	unsigned h = 2166136261u;
	const char *end = strrchr(cname, ':');
	const char *p;
	if (!end || (cname[0] == '[' && !strchr(cname, ']')))
		end = cname + strlen(cname);
	for (p = cname; p < end; p++)
		h = (h ^ (unsigned char)*p) * 16777619u;
	return h;
}

// side[] follows serverlist_cache: entries are only ever appended (masters,
// LAN answers, favourites) or dropped all at once (a reset)
static void sync_sides(void)
{
	if (nside > serverlist_cachecount)
		nside = 0;
	while (nside < serverlist_cachecount && nside < SERVERLIST_TOTALSIZE)
	{
		side_t *s = &side[nside];
		const serverlist_entry_t *e = &serverlist_cache[nside];
		const touchsl_record_t *r = recs ? rec_find(e->info.cname) : NULL;
		memset(s, 0, sizeof(*s));
		s->iphash = ip_hash(e->info.cname);
		s->lastsend = asked_when(e->info.cname);
		s->known = !r ? TOUCHSL_UNKNOWN : r->fails ? TOUCHSL_KNOWN_DEAD : TOUCHSL_KNOWN_ALIVE;
		if (e->info.isfavorite && s->known != TOUCHSL_KNOWN_ALIVE)
			s->known = TOUCHSL_KNOWN_ALIVE; // a favourite gets every try
		nside++;
		sl.serial++;
	}
}

static int find_entry(const char *cname)
{
	unsigned i;
	for (i = serverlist_cachecount; i-- > 0;)
		if (!strcmp(serverlist_cache[i].info.cname, cname))
			return (int)i;
	return -1;
}

static qbool lan_address(const char *cname)
{
	unsigned a, b, c, d;
	return sscanf(cname, "%u.%u.%u.%u", &a, &b, &c, &d) == 4
		&& TouchLAN_IsLanIPv4(((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d);
}

// The saved rows go up before anything is answered.
static int preload(void)
{
	int i, n, shown = 0;
	long long now = (long long)time(NULL);
	qbool lanonly = TouchLAN_LanOnly();
	recs_load();
	for (i = 0; i < nrecs; i++)
	{
		const touchsl_record_t *r = &recs[i];
		serverlist_entry_t *e;
		serverlist_info_t *in;
		if (!TouchSL_RecordShown(r, now) || (lanonly && !lan_address(r->cname)))
			continue;
		if (!TouchSrvList_AddServer(r->cname, false))
			break;
		n = find_entry(r->cname);
		if (n < 0)
			continue;
		e = &serverlist_cache[n];
		in = &e->info;
		if (in->ping)
			continue; // already a live row
		in->ping = r->ping;
		in->numplayers = r->numplayers;
		in->numbots = r->numbots;
		in->numhumans = r->numplayers - max(0, r->numbots);
		in->maxplayers = r->maxplayers;
		in->freeslots = r->maxplayers - r->numplayers;
		in->protocol = r->protocol;
		in->gameversion = r->gameversion;
		in->name_len = dp_strlcpy(in->name, r->name, sizeof(in->name));
		in->map_len = dp_strlcpy(in->map, r->map, sizeof(in->map));
		in->mod_len = dp_strlcpy(in->mod, r->mod, sizeof(in->mod));
		in->game_len = dp_strlcpy(in->game, r->game, sizeof(in->game));
		in->qcstatus_len = dp_strlcpy(in->qcstatus, r->qcstatus, sizeof(in->qcstatus));
		in->players_len = in->players[0] = 0;
		e->responded = true; // "show this row": front ends list what responded
		serverreplycount++;
		sync_sides();
		side[n].flags |= SF_STALE;
		side[n].known = TOUCHSL_KNOWN_ALIVE;
		TouchSrvList_ViewInsert(e);
		shown++;
	}
	return shown;
}

// ---------------------------------------------------------------------------
// the seed: the first open of a new install asks these at once
// ---------------------------------------------------------------------------
static void seed_add(const char *cname)
{
	int n;
	char clean[128];
	size_t len = strcspn(cname, " \t\r\n");
	if (!len || len >= sizeof(clean) || sl.seeded >= TOUCHSL_SEED_MAX)
		return;
	memcpy(clean, cname, len);
	clean[len] = 0;
	if (!TouchSrvList_AddServer(clean, false))
		return;
	sync_sides();
	n = find_entry(clean);
	if (n < 0 || (unsigned)n >= nside || side[n].known != TOUCHSL_UNKNOWN)
		return;
	side[n].known = TOUCHSL_KNOWN_SEED;
	sl.seeded++;
}

// The seed stands for what the masters would say: with no master set (tests,
// a LAN-only setup) there is nothing to stand in for.
static qbool masters_set(void)
{
	static const char *const names[] = {"sv_master1", "sv_master2", "sv_master3", "sv_master4", "sv_masterextra1", "sv_masterextra2", "sv_masterextra3"};
	size_t i;
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
	{
		cvar_t *c = Cvar_FindVar(&cvars_all, names[i], CF_CLIENT | CF_SERVER);
		if (c && c->string[0])
			return true;
	}
	return false;
}

static void seed(void)
{
	int i;
	sl.seeded = 0;
	if (!net_slist_seed.string[0] || !strcmp(net_slist_seed.string, "0") || TouchLAN_LanOnly() || !masters_set())
		return;
	recs_load();
	if (nrecs > 0)
		return; // the saved list knows better
	if (!strcmp(net_slist_seed.string, "1"))
	{
		for (i = 0; touchsl_seed[i]; i++)
			seed_add(touchsl_seed[i]);
	}
	else
	{
		// a list of our own (tests): one address a line
		char *buf = (char *)FS_LoadFile(net_slist_seed.string, tempmempool, true, NULL), *line, *next;
		if (!buf)
			return;
		for (line = buf; *line; line = next)
		{
			next = strchr(line, '\n');
			next = next ? next + 1 : line + strlen(line);
			if (*line != '#' && *line != '\n')
				seed_add(line);
		}
		Mem_Free(buf);
	}
}

// The first wave's answers come back within a few milliseconds of each other
// when many servers sit at one distance; a default receive buffer (some 200 kB
// of skb memory, 60-150 datagrams) overflows if a frame takes a while.
static void socket_buffers(void)
{
#ifdef TOUCHSL_SETSOCKOPT
	lhnetsocket_t *s;
	int rcv = TOUCHSL_SOCKET_RCVBUF, snd = TOUCHSL_SOCKET_SNDBUF;
	List_For_Each_Entry(s, &lhnet_socketlist.list, lhnetsocket_t, list)
	{
		if (s->address.addresstype != LHNETADDRESSTYPE_INET4 && s->address.addresstype != LHNETADDRESSTYPE_INET6)
			continue;
		// the kernel caps both at its rmem_max / wmem_max; failing is harmless
		setsockopt(s->inetsocket, SOL_SOCKET, SO_RCVBUF, (const char *)&rcv, sizeof(rcv));
		setsockopt(s->inetsocket, SOL_SOCKET, SO_SNDBUF, (const char *)&snd, sizeof(snd));
	}
#endif
}

// ---------------------------------------------------------------------------
// summary
// ---------------------------------------------------------------------------
static void summary_set(int servers, int players, qbool saved)
{
	char buf[64];
	if (servers != slist_servers.integer || players != slist_players.integer || !slist_summary.string[0])
	{
		Cvar_SetValueQuick(&slist_servers, servers);
		Cvar_SetValueQuick(&slist_players, players);
		if (servers > 0)
			TouchSL_Summary(buf, sizeof(buf), servers, players);
		else
			buf[0] = 0;
		Cvar_SetQuick(&slist_summary, buf);
	}
	if ((saved != 0) != (slist_saved.integer != 0))
		Cvar_SetValueQuick(&slist_saved, saved);
}

static void summary_update(qbool force)
{
	unsigned i;
	int servers = 0, players = 0, stale = 0;
	double now = now_real();
	if (!force && now < sl.next_summary)
		return;
	sl.next_summary = now + 0.25;
	if ((serverlist_querystage != 0) != (slist_refreshing.integer != 0))
		Cvar_SetValueQuick(&slist_refreshing, serverlist_querystage != 0);
	if (!serverlist_cachecount && !sl.running && sl.settle_time == 0)
		return; // nothing asked yet this session: the startup numbers of the saved list stand
	for (i = 0; i < serverlist_cachecount; i++)
	{
		const serverlist_info_t *in = &serverlist_cache[i].info;
		if (!in->ping)
			continue;
		servers++;
		players += max(0, in->numhumans);
		if (TouchSrvList_IsStale(i))
			stale++;
	}
	summary_set(servers, players, stale > 0);
}

// before the list was ever opened: what the saved list says
static void summary_from_saved(void)
{
	int i, servers = 0, players = 0;
	long long now = (long long)time(NULL);
	recs_load();
	for (i = 0; i < nrecs; i++)
		if (TouchSL_RecordShown(&recs[i], now))
		{
			servers++;
			players += max(0, recs[i].numplayers - recs[i].numbots);
		}
	if (servers)
		summary_set(servers, players, true);
}

// ---------------------------------------------------------------------------
// stats
// ---------------------------------------------------------------------------
static void stats_line(char *buf, size_t size)
{
	dpsnprintf(buf, size, "SLIST %s%s: rows_at_open=%d first_row=%.3f first_answer=%.3f t50=%.3f t95=%.3f settled=%.3f answers=%d listed=%u sent=%d removed=%d masters=%u/%u seeded=%d frames=%d",
		sl.mode, sl.background ? "-background" : "", sl.rows_at_open, sl.t_first_row, sl.t_first_answer, sl.t50, sl.t95, sl.t_settled,
		sl.answers, serverlist_cachecount, sl.sent, sl.removed, sl.masters_replied, sl.masters_asked, sl.seeded, sl.frames);
}

static void settle(double now)
{
	static float times[SERVERLIST_TOTALSIZE];
	unsigned i;
	int n = 0;
	char buf[512];
	for (i = 0; i < nside && i < serverlist_cachecount; i++)
		if (side[i].reply > 0)
			times[n++] = side[i].reply;
	sl.answers = n;
	sl.t50 = TouchSL_Percentile(times, n, 50);
	sl.t95 = TouchSL_Percentile(times, n, 95);
	sl.t_settled = (float)(now - sl.t0);
	sl.masters_asked = masterquerycount;
	sl.masters_replied = masterreplycount;
	sl.running = false;
	sl.settle_time = now;
	sl.serial++;
	if (sl.fast && !sl.model)
	{
		recs_load();
		recs_merge();
		recs_save();
	}
	summary_update(true);
	stats_line(buf, sizeof(buf));
	if (net_slist_stats_print.integer || developer.integer)
		Con_Printf("%s\n", buf);
}

static void TouchSrvList_Stats_f(cmd_state_t *cmd)
{
	char buf[512];
	if (!sl.t0)
	{
		Con_Print("net_slist_stats: no refresh yet\n");
		return;
	}
	stats_line(buf, sizeof(buf));
	Con_Printf("%s%s\n", buf, sl.running ? " (still running)" : "");
	Con_Print("times in seconds from the refresh: first_row = first row on screen (0 with saved rows), t50/t95 = half / 95% of the servers that answered had answered\n");
}

// ---------------------------------------------------------------------------
// hooks
// ---------------------------------------------------------------------------
void TouchSrvList_Refresh(qbool resetcache)
{
	unsigned i;
	double now = now_real();
	qbool had_rows = false;
	sync_sides();
	if (resetcache)
		nside = 0, sync_sides();
	for (i = 0; i < nside; i++)
		if (serverlist_cache[i].info.ping)
			had_rows = true;
	sl.fast = TouchSrvList_Fast() && !(serverlist_querystage & SLIST_QUERYSTAGE_QWMASTERS);
	sl.model = sl.fast && model_insanotic();
	// The list opened, a Refresh by hand (a reset), or the first refresh in a
	// while: full speed. The menu's timer on a list that is up: the stock pace.
	sl.background = sl.fast && !sl.model && !resetcache && had_rows && sl.settle_time > 0 && now - sl.settle_time < 60;
	dp_strlcpy(sl.mode, sl.model ? "insanotic-model" : sl.fast ? "fast" : "stock", sizeof(sl.mode));
	sl.running = true;
	sl.t0 = now;
	sl.sent = sl.answers = sl.removed = sl.frames = 0;
	sl.t_first_row = sl.t_first_answer = sl.t50 = sl.t95 = sl.t_settled = 0;
	memset(&sl.bucket, 0, sizeof(sl.bucket));
	for (i = 0; i < nside; i++)
	{
		side[i].tries = 0;
		side[i].reply = 0;
		side[i].flags &= SF_STALE;
		if (sl.fast && serverlist_cache[i].info.ping)
		{
			// the row stays up while it is asked again
			serverlist_cache[i].responded = true;
			if (sl.background && side[i].lastanswer && now - side[i].lastanswer < TOUCHSL_MIN_REQUERY)
				side[i].flags |= SF_DONE | SF_ANSWER; // answered a moment ago: left alone
		}
	}
	if (sl.fast && !sl.model && net_slist_saved.integer && (resetcache || !sl.preloaded_once))
	{
		sl.preloaded_once = true;
		preload();
	}
	sl.seeded = 0;
	if (sl.fast && !sl.model && !sl.background)
		seed();
	if (sl.fast && !sl.model)
		socket_buffers();
	sl.rows_at_open = 0;
	for (i = 0; i < serverlist_cachecount; i++)
		if (serverlist_cache[i].info.ping)
			sl.rows_at_open++;
	if (sl.rows_at_open)
		sl.t_first_row = 0.0001f; // in the frame the list opened
	sl.serial++;
	summary_update(true);
	// the first wave goes out now, in the frame the list opened, not the next
	if (sl.fast && !sl.model)
		TouchSrvList_QueryFrame();
}

void TouchSrvList_Replied(unsigned n)
{
	double now = now_real();
	sync_sides();
	if (n >= nside)
		return;
	side[n].lastanswer = now;
	if (side[n].flags & SF_STALE)
		sl.serial++;
	side[n].flags = (side[n].flags & ~SF_STALE) | SF_ANSWER;
	if (!sl.running)
		return;
	if (!side[n].reply)
	{
		side[n].reply = (float)max(0.0005, now - sl.t0);
		if (!sl.t_first_answer)
			sl.t_first_answer = side[n].reply;
		if (!sl.t_first_row)
			sl.t_first_row = side[n].reply;
		if (developer_extra.integer && sl.answers < 5)
			Con_DPrintf("slist: answer from %s at %.3f (asked at %.3f, frame %d)\n", serverlist_cache[n].info.cname, side[n].reply, side[n].lastsend - sl.t0, sl.frames);
		sl.answers++;
		sl.serial++;
	}
}

// 1 sent, 0 not now (its host had its share), -1 the socket is full: stop
// for this frame
static int send_query(unsigned i, const char *query, double now, qbool hostlimit)
{
	serverlist_entry_t *e = &serverlist_cache[i];
	side_t *s = &side[i];
	lhnetaddress_t address;
	lhnetsocket_t *sock;
	int ret;
	if (!LHNETADDRESS_FromString(&address, e->info.cname, 0) || !(sock = NetConn_ChooseClientSocketForAddress(&address)))
	{
		s->flags |= SF_DONE; // no socket for that address family
		return 0;
	}
	if (hostlimit && !TouchSL_HostTake(&sl.hosts, s->iphash, now))
		return 0; // other ports of this host were just asked
	ret = NetConn_WriteString(sock, query, &address);
	if (ret == 0)
		return -1; // EWOULDBLOCK: the send buffer is full, the rest next frame
	// sent, or failed for good (no route): either way one try
	asked_now(e->info.cname, now);
	s->lastsend = now;
	e->querytime = now;
	s->tries++;
	sl.sent++;
	return 1;
}

// Insanotic's NetConn_QueryQueueFrame: up to 100 queries every frame, in the
// order the masters listed the servers; a silent server is asked again after
// net_slist_timeout (4 s), three times, then given up.
static qbool model_frame(const char *query, double now)
{
	unsigned i;
	int queries = 0;
	qbool pending = false;
	for (i = 0; i < nside && i < serverlist_cachecount && queries < 100; i++)
	{
		side_t *s = &side[i];
		if (s->flags & (SF_DONE | SF_ANSWER))
			continue;
		pending = true;
		if (s->tries && now < s->lastsend + 4.0)
			continue;
		if (s->tries < 3)
		{
			if (send_query(i, query, now, false) < 0)
				break;
			queries++;
		}
		else
			s->flags |= SF_DONE;
	}
	for (; i < nside && i < serverlist_cachecount && !pending; i++)
		if (!(side[i].flags & (SF_DONE | SF_ANSWER)))
			pending = true;
	return pending;
}

qbool TouchSrvList_QueryFrame(void)
{
	char query[64];
	unsigned i;
	int budget, used = 0, pass, maxtries, ret;
	double now = now_real(), finalwait, rate, burst;
	qbool pending = false, masters_done, full = false;

	sync_sides();
	summary_update(false);
	if (sl.running)
		sl.frames++;

	if (sl.running && !sl.fast)
	{
		// the stock scheduler runs it; we only notice the end
		if (!serverlist_querystage)
			settle(now);
		return false;
	}
	if (!TouchSrvList_Fast() && !sl.running)
		return false;
	if (!sl.running)
	{
		// a master answered late: its servers are asked too
		qbool late = false;
		for (i = 0; i < nside && i < serverlist_cachecount && !late; i++)
			if (!side[i].tries && !(side[i].flags & (SF_DONE | SF_ANSWER)) && sl.settle_time > 0)
				late = true;
		if (!late || key_dest == key_game)
		{
			if (serverlist_querystage)
				serverlist_querystage = 0;
			return true;
		}
		sl.running = true;
		serverlist_querystage = SLIST_QUERYSTAGE_SERVERS;
	}
	if (TouchJoinFail_Joining())
	{
		// a join is on: not one packet that could count against us at a server
		sl.bucket.last = now;
		return true;
	}

	TouchLAN_BuildQuery(query, sizeof(query), now);
	if (sl.model)
	{
		pending = model_frame(query, now);
		masters_done = serverlist_querystage >= SLIST_QUERYSTAGE_SERVERS || now - sl.t0 > TOUCHSL_MASTER_WAIT;
		if (!pending && masters_done && now - sl.t0 > 0.25)
		{
			serverlist_querystage = 0;
			settle(now);
		}
		return true;
	}

	rate = sl.background ? TOUCHSL_RATE_BACKGROUND : bound(20, net_slist_fast_rate.value, 8000);
	burst = sl.background ? TOUCHSL_BURST_BACKGROUND : bound(1, net_slist_fast_burst.value, 512);
	budget = TouchSL_Budget(&sl.bucket, now, rate, burst);
	finalwait = TouchSL_FinalWait(Cvar_VariableValue(&cvars_all, "net_slist_maxping", CF_CLIENT));
	maxtries = Cvar_VariableValue(&cvars_all, "net_slist_maxtries", CF_CLIENT);

	// First asks: favourites and the servers we knew were up, then the rest
	// (a server that failed before goes last). Then the second and third asks.
	for (pass = 0; pass < 4; pass++)
	{
		for (i = 0; i < nside && i < serverlist_cachecount && !full; i++)
		{
			side_t *s = &side[i];
			serverlist_entry_t *e = &serverlist_cache[i];
			if (s->flags & (SF_DONE | SF_ANSWER))
				continue;
			if (!s->maxtries || !s->tries)
				s->maxtries = TouchSL_Tries(s->known, maxtries, sl.background);
			if (e->protocol == PROTOCOL_QUAKEWORLD)
			{
				s->flags |= SF_DONE;
				continue;
			}
			if (pass == 0)
			{
				// bookkeeping pass: who is finished, is anything still open
				if (s->tries >= s->maxtries)
				{
					if (now >= s->lastsend + finalwait)
					{
						s->flags |= SF_DONE;
						if (e->info.ping)
						{
							// it was listed and no longer answers
							TouchSrvList_ViewRemove(e);
							e->info.ping = 0;
							e->responded = false;
							if (serverreplycount)
								serverreplycount--;
							s->flags &= ~SF_STALE;
							sl.removed++;
							sl.serial++;
						}
					}
					else
						pending = true;
				}
				else
					pending = true;
				continue;
			}
			if (used >= budget || s->tries >= s->maxtries)
				continue;
			if (pass < 3 && s->lastsend && now < s->lastsend + TOUCHSL_RETRY_INTERVAL)
				continue; // asked a moment ago (before a reset): it would not answer yet
			if (pass == 1 && !(s->tries == 0 && TouchSL_FirstWave(s->known)))
				continue;
			if (pass == 2 && !(s->tries == 0 && !TouchSL_FirstWave(s->known)))
				continue;
			if (pass == 3 && !(s->tries > 0 && now >= s->lastsend + TOUCHSL_RETRY_INTERVAL))
				continue;
			ret = send_query(i, query, now, true);
			if (ret > 0)
				used++;
			else if (ret < 0)
				full = true;
		}
	}
	TouchSL_Spend(&sl.bucket, used);

	// Settled: every master has sent its end-of-list (netconn.c moves the
	// stage on) or had its time, and every server answered or was given up.
	masters_done = serverlist_querystage >= SLIST_QUERYSTAGE_SERVERS || now - sl.t0 > TOUCHSL_MASTER_WAIT;
	if (!pending && masters_done && now - sl.t0 > 0.25)
	{
		serverlist_querystage = 0;
		settle(now);
	}
	return true;
}

// The hold: with a finger on the menu the QuakeC list is paused the way its
// own Pause box does it, and let go on release. A tap on that box toggles the
// value we put there, so a 0 after the release means the player asked for 1.
static void hold_frame(void)
{
	int i, active = 0;
	float x, y;
	cvar_t *pause;
	double now = now_real();
	qbool finger = false;
	if (key_dest == key_menu || key_dest == key_menu_grabbed)
		for (i = 0; i < 10 && !finger; i++)
		{
			VID_GetTouchFinger(i, &active, &x, &y);
			if (active)
				finger = true;
		}
	if (finger == sl.finger)
	{
		if (!finger && sl.autopaused && now - sl.finger_up >= 0.25)
		{
			pause = Cvar_FindVar(&cvars_all, "net_slist_pause", CF_CLIENT);
			sl.autopaused = false;
			if (pause)
				Cvar_SetValueQuick(pause, pause->integer ? 0 : 1); // still ours: let go; toggled by the tap: the player's pause
			sl.serial++;
		}
		return;
	}
	sl.finger = finger;
	pause = Cvar_FindVar(&cvars_all, "net_slist_pause", CF_CLIENT);
	if (finger)
	{
		if (net_slist_hold.integer && pause && !pause->integer && !sl.autopaused)
		{
			sl.autopaused = true;
			Cvar_SetValueQuick(pause, 1);
		}
	}
	else
		sl.finger_up = now;
}

void TouchSrvList_Frame(void)
{
	hold_frame();
	summary_update(false);
}

// net_slist_open: what opening the server list does (a reset refresh, quiet).
static void TouchSrvList_Open_f(cmd_state_t *cmd)
{
	ServerList_QueryList(true, true, false, false);
}

void TouchSrvList_Init(void)
{
	Cvar_RegisterVariable(&net_slist_fast);
	Cvar_RegisterVariable(&net_slist_fast_rate);
	Cvar_RegisterVariable(&net_slist_fast_burst);
	Cvar_RegisterVariable(&net_slist_seed);
	Cvar_RegisterVariable(&net_slist_saved);
	Cvar_RegisterVariable(&net_slist_hold);
	Cvar_RegisterVariable(&net_slist_stats_print);
	Cvar_RegisterVariable(&slist_summary);
	Cvar_RegisterVariable(&slist_servers);
	Cvar_RegisterVariable(&slist_players);
	Cvar_RegisterVariable(&slist_refreshing);
	Cvar_RegisterVariable(&slist_saved);
	Cmd_AddCommand(CF_CLIENT, "net_slist_open", TouchSrvList_Open_f, "Godnotic: refresh the server list from scratch as opening it in the menu does (no console output)");
	Cmd_AddCommand(CF_CLIENT, "net_slist_stats", TouchSrvList_Stats_f, "Godnotic: timings of the last server list refresh (first row, 50%, 95%, settled)");
}

// the file system is up by the first frame
void TouchSrvList_FirstFrame(void)
{
	summary_from_saved();
}

#else // !CONFIG_MENU: the dedicated server has no server list

qbool TouchSrvList_Fast(void) { return false; }
qbool TouchSrvList_IsStale(unsigned n) { return false; }
qbool TouchSrvList_Hold(void) { return false; }
unsigned TouchSrvList_Serial(void) { return 0; }
void TouchSrvList_Init(void) {}
void TouchSrvList_Frame(void) {}
void TouchSrvList_FirstFrame(void) {}

#endif
