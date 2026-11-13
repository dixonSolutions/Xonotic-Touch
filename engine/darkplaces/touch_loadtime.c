/*
Godnotic: load-time accounting, and no frame limiter while a game loads.

Accounting: every phase in touch_loadtime.h is timed with one clock read at
each end, so it costs nothing worth measuring and needs no cvar to be armed.
touch_loadtime 1 prints, at the first frame of the main menu and at the first
game frame after a load:

  LOADTIME menu_ready proc=<ms since process start> init=<ms since Host_Init>
  LOADTIME game_ready load=<ms since the load began> proc=<ms> frames=<n>

followed by the phase table of that stretch, the file system counters and the
signon timeline. scripts/bench-load.sh parses these lines.

A load begins at touch_loadtime_start (what the bench issues just before its
map or connect command), else when the engine is seen starting one (server
spawn, a connection attempt, a signon below the last).

Restless loading: between a connection's first packet and its last signon a
local game needs about a dozen host frames to trade the handshake with its own
server, and each waited for the frame limiter (cl_maxfps, or the much lower
cl_maxidlefps when the window is not focused, as under the loading screen of a
phone that is not the top activity). touch_load_restless 1 runs the client
frame on every host frame and does not sleep while cls.signon < SIGNONS on a
local server; with a remote server it sleeps at most 1 ms so packets are taken
as they arrive. Neither applies in the menu or in the game.
*/

#include "quakedef.h"
#include "touch_loadtime.h"
#include "libcurl.h"
#include "touch_loadfs.h"
#include "touch_loadcvar.h"
#include "image.h"
#ifdef __linux__
#include <unistd.h>
#include <time.h>
#endif

static cvar_t touch_loadtime = {CF_CLIENT | CF_SERVER, "touch_loadtime", "0", "print where start-up and each map load spent their time (Godnotic): 1 = at the main menu's first frame and at the first game frame after a load"};
static cvar_t touch_load_restless = {CF_CLIENT, "touch_load_restless", "0", "do not wait for the frame limiter or sleep while a game is loading or the connection handshake runs (Godnotic); 0 = stock behaviour"};

static cvar_t touch_load_lazysbar = {CF_CLIENT, "touch_load_lazysbar", "1", "Xonotic: cache the engine status bar's Quake pictures when the engine HUD is first drawn instead of at every renderer start (Godnotic); 0 = at renderer start"};
static cvar_t touch_load_lazypics = {CF_CLIENT, "touch_load_lazypics", "1", "the menu's precache_pic only notes a picture that exists; it loads when first drawn (Godnotic); 0 = load at precache"};
static cvar_t touch_load_lazydeflate = {CF_SERVER, "touch_load_lazydeflate", "1", "deflate csprogs.dat when a client first downloads it instead of at every map start (Godnotic)"};
static cvar_t touch_loadtime_menu = {CF_CLIENT | CF_READONLY, "_touch_loadtime_menu", "0", "1 once the main menu has drawn its first frame (for scripts that wait for it)"};
static cvar_t touch_loadtime_game = {CF_CLIENT | CF_READONLY, "_touch_loadtime_game", "0", "counts loads that reached their first game frame (for scripts that wait for one)"};

static const char *tlt_names[TLT_COUNT] =
{
	"fs init (pk3 mount+index)",
	"startup configs",
	"video/GL init",
	"sound device init",
	"menu init (MR_Init)",
	"font load",
	"QC progs load",
	"server spawn",
	"client CSQC init",
	"csprogs deflate",
	"model load",
	"  q3bsp lightmaps",
	"  q3 shader scripts",
	"sound load/decode",
	"image read+decode",
	"dds/ktx texture load",
	"texture upload",
	"GLSL compile+link",
	"file read (FS_LoadFile)",
	"cvar list insert",
};

static const char *tlc_names[TLC_COUNT] =
{
	"fs lookups",
	"fs lookup misses",
	"loose dir stat()s",
	"files loaded",
};

typedef struct tlt_state_s
{
	double begin[TLT_COUNT];
	int depth[TLT_COUNT];
	double total[TLT_COUNT];
	int calls[TLT_COUNT];
	long long counters[TLC_COUNT];
	long long bytes;

	double init_time;            // Sys_DirtyTime at TouchLoadTime_Init... see first_time
	double first_time;           // first clock read of the process (first Begin)
	qbool menu_reported;
	double menu_time;

	qbool loading;               // a load is being timed
	double load_start;
	int load_frames;             // host frames since load_start
	int last_signon;
	double signon_time[SIGNONS + 1];
	int signon_frame[SIGNONS + 1];
	double ready_time;           // first game frame of the last load, 0 when not watching
	double last_hostframe;
	double worst_frame;          // longest host frame in the 5 s after it
	double worst_play;           // ... of those with no menu or dialog open
	int frames_after;

}
tlt_state_t;

static tlt_state_t tlt;

static double TLT_Now(void)
{
	double t = Sys_DirtyTime();
	if (!tlt.first_time)
		tlt.first_time = t;
	return t;
}

void TouchLoadTime_Begin(int phase)
{
	if (phase < 0 || phase >= TLT_COUNT)
		return;
	if (tlt.depth[phase]++ == 0)
		tlt.begin[phase] = TLT_Now();
}

void TouchLoadTime_End(int phase)
{
	if (phase < 0 || phase >= TLT_COUNT || tlt.depth[phase] <= 0)
		return;
	if (--tlt.depth[phase] == 0)
	{
		tlt.total[phase] += TLT_Now() - tlt.begin[phase];
		tlt.calls[phase]++;
	}
	if (phase == TLT_SPAWN && !tlt.loading && host.state == host_active)
	{
		// a map command from the console or the menu: the load started at
		// the spawn's begin
		tlt.loading = true;
		tlt.load_start = tlt.begin[phase];
		tlt.load_frames = 0;
		tlt.last_signon = -1;
	}
}

void TouchLoadTime_Count(int counter)
{
	if (counter >= 0 && counter < TLC_COUNT)
		tlt.counters[counter]++;
}

void TouchLoadTime_AddBytes(long long bytes)
{
	tlt.counters[TLC_FILES_LOADED]++;
	tlt.bytes += bytes;
}

// Milliseconds since the process started, from the kernel's record of it
// (Linux and Android: /proc/self/stat field 22, in clock ticks since boot);
// -1 elsewhere.
static double TLT_ProcessAgeMs(void)
{
#if defined(__linux__) && defined(CLOCK_BOOTTIME)
	char buf[1024], *p;
	unsigned long long start;
	long hz;
	int field;
	struct timespec ts;
	FILE *f = fopen("/proc/self/stat", "r");
	size_t n;
	if (!f)
		return -1;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	// the command name may hold spaces: count fields after its ')'
	p = strrchr(buf, ')');
	if (!p)
		return -1;
	for (field = 2; *p && field < 22; p++)
		if (*p == ' ')
			field++;
	if (sscanf(p, "%llu", &start) != 1)
		return -1;
	hz = sysconf(_SC_CLK_TCK);
	if (hz <= 0 || clock_gettime(CLOCK_BOOTTIME, &ts) != 0)
		return -1;
	return (ts.tv_sec + ts.tv_nsec * 1e-9 - (double)start / hz) * 1000.0;
#else
	return -1;
#endif
}

static void TLT_ResetStretch(void)
{
	int i;
	for (i = 0; i < TLT_COUNT; i++)
	{
		tlt.total[i] = 0;
		tlt.calls[i] = 0;
	}
	for (i = 0; i < TLC_COUNT; i++)
		tlt.counters[i] = 0;
	tlt.bytes = 0;
}

static void TLT_PrintStretch(void)
{
	int i;
	for (i = 0; i < TLT_COUNT; i++)
		if (tlt.calls[i] || tlt.depth[i])
			Con_Printf("LOADTIME   %-28s %8.1f ms %6d calls\n", tlt_names[i], tlt.total[i] * 1000.0, tlt.calls[i]);
	for (i = 0; i < TLC_COUNT; i++)
		Con_Printf("LOADTIME   %-28s %8lld\n", tlc_names[i], tlt.counters[i]);
	Con_Printf("LOADTIME   %-28s %8.1f MB\n", "bytes loaded", tlt.bytes / 1048576.0);
}

static void TLT_StartLoad(double when)
{
	int i;
	tlt.loading = true;
	tlt.load_start = when;
	tlt.load_frames = 0;
	tlt.last_signon = -1;
	for (i = 0; i <= SIGNONS; i++)
	{
		tlt.signon_time[i] = -1;
		tlt.signon_frame[i] = -1;
	}
	TLT_ResetStretch();
}

static void TouchLoadTime_Start_f(cmd_state_t *cmd)
{
	TLT_StartLoad(Sys_DirtyTime());
}

static void TouchLoadTime_Report_f(cmd_state_t *cmd)
{
	TLT_PrintStretch();
}

static void TLT_Signons(double now)
{
	if (!tlt.loading || cls.state != ca_connected)
		return;
	if (cls.signon != tlt.last_signon && cls.signon >= 0 && cls.signon <= SIGNONS)
	{
		if (tlt.signon_time[cls.signon] < 0)
		{
			tlt.signon_time[cls.signon] = now - tlt.load_start;
			tlt.signon_frame[cls.signon] = tlt.load_frames;
		}
		tlt.last_signon = cls.signon;
	}
}

void TouchLoadTime_Frame(void)
{
	double now = Sys_DirtyTime();
	int i;

	TLT_Signons(now);

	// font sizes r_font_lazysizes left for later: all of them under a load's
	// loading screen (the HUD draws most of them in its first frame), else
	// one per frame once the main menu has been up a moment
	if (scr_loading && (tlt.loading || cls.state == ca_connected))
		while (TouchFont_LoadPendingOne());
	else if (tlt.menu_reported && key_dest == key_menu && cls.state != ca_connected && now - tlt.menu_time > 0.5)
		TouchFont_LoadPendingOne();

	if (!tlt.menu_reported && host.state == host_active && !scr_loading && key_dest == key_menu)
	{
		tlt.menu_reported = true;
		tlt.menu_time = now;
		Cvar_SetValueQuick(&touch_loadtime_menu, 1);
		if (touch_loadtime.integer)
		{
			Con_Printf("LOADTIME menu_ready proc=%.1f init=%.1f\n", TLT_ProcessAgeMs(), (now - tlt.first_time) * 1000.0);
			TLT_PrintStretch();
		}
		TLT_ResetStretch();
	}

	if (tlt.loading && !scr_loading && cls.state == ca_connected && cls.signon == SIGNONS && cl.worldmodel)
	{
		tlt.loading = false;
		Cvar_SetValueQuick(&touch_loadtime_game, touch_loadtime_game.integer + 1);
		tlt.ready_time = now;
		tlt.worst_frame = 0;
		tlt.worst_play = 0;
		tlt.frames_after = 0;
		if (touch_loadtime.integer)
		{
			Con_Printf("LOADTIME game_ready load=%.1f proc=%.1f frames=%d map=%s\n", (now - tlt.load_start) * 1000.0,
				TLT_ProcessAgeMs(), tlt.load_frames, cl.worldbasename);
			for (i = 0; i <= SIGNONS; i++)
				if (tlt.signon_time[i] >= 0)
					Con_Printf("LOADTIME   signon %d at %8.1f ms, host frame %d\n", i, tlt.signon_time[i] * 1000.0, tlt.signon_frame[i]);
			TLT_PrintStretch();
		}
		TLT_ResetStretch();
	}
}

qbool TouchLoadTime_Restless(void)
{
	if (!touch_load_restless.integer || cls.state == ca_dedicated)
		return false;
	if (cls.demoplayback || cls.timedemo)
		return false;
	// the handshake, not a download: a download from an HTTP server is paced
	// by curl's own select
	if (cls.state == ca_connected && cls.signon < SIGNONS && !Curl_Running() && !cls.qw_downloadname[0])
		return true;
	return false;
}

static void (*tl_sbar_start)(void);
static qbool tl_sbar_deferred, tl_sbar_running;

qbool TouchLoad_SbarDefer(void (*start)(void))
{
	if (!touch_load_lazysbar.integer || gamemode != GAME_XONOTIC || tl_sbar_running)
		return false;
	tl_sbar_start = start;
	tl_sbar_deferred = true;
	return true;
}

void TouchLoad_SbarEnsure(void)
{
	if (!tl_sbar_deferred)
		return;
	tl_sbar_deferred = false;
	tl_sbar_running = true;
	tl_sbar_start();
	tl_sbar_running = false;
}

// lazily precached menu pictures: name -> the flags precache_pic asked for
#define TL_LAZYPICS 1024
typedef struct tl_lazypic_s { char name[MAX_QPATH]; unsigned int flags; } tl_lazypic_t;
static tl_lazypic_t *tl_lazypics;
static int tl_numlazypics;

static int TL_FindLazyPic(const char *name)
{
	int i;
	for (i = 0; i < tl_numlazypics; i++)
		if (!strcmp(tl_lazypics[i].name, name))
			return i;
	return -1;
}

qbool TouchLoad_LazyPrecachePic(prvm_prog_t *prog, const char *name, unsigned int flags)
{
	static const char *const forms[] = {"dds/%s.dds", "dds/%s.ktx", "%s.tga", "%s.png", "%s.jpg", "%s.pcx"};
	char base[MAX_QPATH], path[MAX_QPATH + 16];
	size_t i;

	if (!touch_load_lazypics.integer || prog != MVM_prog || cls.state == ca_dedicated || !name[0] || strlen(name) >= MAX_QPATH)
		return false;
	if (TL_FindLazyPic(name) >= 0)
		return true;
	if (tl_numlazypics >= TL_LAZYPICS)
		return false;
	Image_StripImageExtension(name, base, sizeof(base));
	// an existing file in a form every image loader search list tries;
	// anything else is precached now, so a missing picture still returns
	// "" to the QC as before
	for (i = 0; i < sizeof(forms) / sizeof(forms[0]); i++)
	{
		dpsnprintf(path, sizeof(path), forms[i], base);
		if (FS_FileExists(path))
			break;
	}
	if (i == sizeof(forms) / sizeof(forms[0]))
		return false;
	if (!tl_lazypics)
		tl_lazypics = (tl_lazypic_t *)Mem_Alloc(cls.permanentmempool, TL_LAZYPICS * sizeof(*tl_lazypics));
	dp_strlcpy(tl_lazypics[tl_numlazypics].name, name, sizeof(tl_lazypics[0].name));
	tl_lazypics[tl_numlazypics].flags = flags;
	tl_numlazypics++;
	return true;
}

unsigned int TouchLoad_PicFlags(const char *path, unsigned int flags)
{
	int i;
	if (!tl_numlazypics || (i = TL_FindLazyPic(path)) < 0)
		return flags;
	// first load of a precached picture: persistent, with precache's flags
	flags = (flags & ~(CACHEPICFLAG_NOTPERSISTENT | CACHEPICFLAG_FAILONMISSING)) | (tl_lazypics[i].flags & ~CACHEPICFLAG_FAILONMISSING);
	tl_lazypics[i] = tl_lazypics[--tl_numlazypics];
	return flags;
}

void (*touch_snd_loadsome)(void);
void (*touch_snd_poll)(void);
void (*touch_model_loadsome)(void);

qbool TouchModel_CSQCPrecache(prvm_prog_t *prog, model_t *m)
{
	int i;
	if (!TouchModel_IsDeferred(m))
		return false;
	// as VM_CL_precache_model does for a loaded model
	for (i = 0; i < MAX_MODELS; i++)
	{
		if (!cl.csqc_model_precache[i])
		{
			cl.csqc_model_precache[i] = m;
			PRVM_G_FLOAT(OFS_RETURN) = -(i + 1);
			return true;
		}
	}
	VM_Warning(prog, "VM_CL_precache_model: no free models\n");
	return true;
}

qbool TouchLoad_DeferCSQCDeflate(void)
{
	return touch_load_lazydeflate.integer != 0;
}

void TouchLoad_EnsureCSQCDeflated(void)
{
	size_t deflated_size = 0;
	if (!svs.csqc_progdata || svs.csqc_progdata_deflated || sv.csqc_progsize <= 0)
		return;
	TouchLoadTime_Begin(TLT_DEFLATE);
	svs.csqc_progdata_deflated = FS_Deflate(svs.csqc_progdata, sv.csqc_progsize, &deflated_size, -1, sv_mempool);
	TouchLoadTime_End(TLT_DEFLATE);
	svs.csqc_progsize_deflated = (int)deflated_size;
}

float TouchLoadTime_MaxFPS(float maxfps)
{
	// no frame limiter: CL_Frame then runs every host frame on the real
	// elapsed time, as for a timedemo, and asks for no sleep
	return TouchLoadTime_Restless() ? 0 : maxfps;
}

double TouchLoadTime_SleepTime(double sleeptime)
{
	double now = Sys_DirtyTime();

	if (tlt.loading)
		tlt.load_frames++;
	// what a load left for later shows up as long frames right after it
	if (tlt.ready_time > 0)
	{
		if (tlt.last_hostframe > tlt.ready_time && now - tlt.last_hostframe > tlt.worst_frame)
			tlt.worst_frame = now - tlt.last_hostframe;
		if (tlt.last_hostframe > tlt.ready_time && key_dest == key_game && now - tlt.last_hostframe > tlt.worst_play)
			tlt.worst_play = now - tlt.last_hostframe;
		if (developer_loading.integer && tlt.last_hostframe > tlt.ready_time && now - tlt.last_hostframe > 0.03)
			Con_Printf("LOADTIME long frame %.1f ms at %.0f ms after ready\n", (now - tlt.last_hostframe) * 1000.0, (now - tlt.ready_time) * 1000.0);
		tlt.frames_after++;
		if (now - tlt.ready_time >= 5)
		{
			if (touch_loadtime.integer)
				Con_Printf("LOADTIME after_ready worst_frame=%.1f worst_play=%.1f frames=%d (first 5 s after the load; play = no menu or dialog open)\n", tlt.worst_frame * 1000.0, tlt.worst_play * 1000.0, tlt.frames_after);
			tlt.ready_time = 0;
		}
	}
	tlt.last_hostframe = now;
	// a load the bench did not announce: a connection attempt, or a signon
	// starting over (changelevel)
	if (!tlt.loading && host.state == host_active && cls.state != ca_dedicated
	 && (cls.connect_trying || (cls.state == ca_connected && cls.signon < SIGNONS)))
		TLT_StartLoad(now);
	TLT_Signons(now);

	// background work left by the load, once the game runs
	if (touch_snd_poll)
		touch_snd_poll();
	// what the load left: decoded while a menu or dialog is open (the welcome
	// dialog after a join, the game menu); in play only what is used loads
	if (cls.state == ca_connected && cls.signon == SIGNONS && !scr_loading && key_dest != key_game)
	{
		if (touch_model_loadsome)
			touch_model_loadsome();
		if (touch_snd_loadsome)
			touch_snd_loadsome();
	}

	if (TouchLoadTime_Restless())
	{
		// our own server answers within the same host frame: no sleep;
		// a remote one: take its packets as they come, without spinning
		if (sv.active)
			sleeptime = 0;
		else
			sleeptime = 0.001;
	}
	return sleeptime;
}

void TouchLoadTime_Init(void)
{
	TouchLoadFS_Init();
	TouchCvar_Init();
	Cvar_RegisterVariable(&touch_loadtime);
	Cvar_RegisterVariable(&touch_load_restless);
	Cvar_RegisterVariable(&touch_load_lazysbar);
	Cvar_RegisterVariable(&touch_load_lazypics);
	Cvar_RegisterVariable(&touch_load_lazydeflate);
	Cvar_RegisterVariable(&touch_loadtime_menu);
	Cvar_RegisterVariable(&touch_loadtime_game);
	Cmd_AddCommand(CF_CLIENT | CF_SERVER, "touch_loadtime_start", TouchLoadTime_Start_f, "start timing a load now (put it before map or connect); touch_loadtime 1 prints the result at the first game frame");
	Cmd_AddCommand(CF_CLIENT | CF_SERVER, "touch_loadtime_report", TouchLoadTime_Report_f, "print the load phase totals gathered since the last report");
}
