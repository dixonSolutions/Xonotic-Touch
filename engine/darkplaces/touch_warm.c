/*
Godnotic: prepare the next local game while the menu is idle (touch_warm).

A tap on Play loads the map's world model, then the server and CSQC
precache what they need and touch_preload loads what earlier games on the
map used late. With the game open on the menu and the player doing nothing
the device was idle all that time.

touch_warm 1: once the menu has had no input for touch_warm_idle seconds,
the map the next game most likely uses is prepared in the background, a
piece per frame within touch_warm_budget ms (the world model is one piece
and can take longer):
  - the world model, its textures, lightmaps and shaders, exactly as
    SV_SpawnServer's Mod_ForName would load them;
  - its touch_preload list (touch_preload/<map>.txt, or the built-in seed
    before the map's first game): models, pictures, GLSL permutations and
    sounds, as the replay under the loading screen would load them;
  - the persistent pictures earlier map loads spent over 2 ms on
    (_touch_warm_pics: CSQC's precaches, the two zoom reticles
    among them).
The map is touch_warm_map when a menu or front end sets it (the map its Play
button starts), else the last local map played (_touch_warm_last, saved).

Play then finds these already loaded: SV_SpawnServer's Mod_ForName returns
the world model, precaches mark the models used so the purge at the end of
the load keeps them, the replay finds nothing left to load. Nothing here
depends on the gametype, mutators or bots: a different map or settings only
means the prepared assets go unused and the end of that load frees them
(Mod_PurgeUnused, S_PurgeUnused) as it frees an earlier game's. A texture
quality change restarts the renderer, which reloads loaded models as before.

Never in a game, a demo, with the console open, the window hidden or
unfocused, on battery saver (touch_device_powersave 1) or when the device is
warm (touch_device_thermal >= touch_warm_maxthermal); the platform layer sets
those two (Android: PowerManager.isPowerSaveMode and getCurrentThermalStatus).
Any input stops it until the menu is idle again. It prepares a map once and
then does nothing until the target changes or a game is played. On a
low-memory warning (SDL_APP_LOWMEMORY, or the touch_warm_drop command) the
models and sounds no game uses are freed and nothing is prepared again until
the next game.
*/

#include "quakedef.h"
#include "touch_warm.h"
#include "touch_crc.h"
#include "touch_prvm.h"
#include "touch_loadtime.h"
#if defined(__has_include)
# if __has_include(<SDL.h>) && !defined(DEDICATED_SERVER)
#  include <SDL.h>
#  define TOUCH_WARM_SDL 1
# endif
#endif

#ifdef __EMSCRIPTEN__
// the web build fetches maps on demand: preparing one would download it
#define TOUCH_WARM_DEFAULT "0"
#else
#define TOUCH_WARM_DEFAULT "1"
#endif
static cvar_t touch_warm = {CF_CLIENT | CF_ARCHIVE, "touch_warm", TOUCH_WARM_DEFAULT, "while the menu is idle, load the next local game's map, models, sounds and shaders in the background so Play starts sooner (Godnotic)"};
static cvar_t touch_warm_idle = {CF_CLIENT, "touch_warm_idle", "2", "seconds without input on the menu before touch_warm starts"};
static cvar_t touch_warm_budget = {CF_CLIENT, "touch_warm_budget", "12", "milliseconds per frame touch_warm may spend (one model is never split: the world model takes as long as it takes)"};
static cvar_t touch_warm_map = {CF_CLIENT, "touch_warm_map", "", "map touch_warm prepares (a menu or front end sets it to what its Play button starts); empty = the last local map played"};
static cvar_t touch_warm_last = {CF_CLIENT | CF_ARCHIVE, "_touch_warm_last", "", "last local map played (touch_warm)"};
static cvar_t touch_warm_pics = {CF_CLIENT | CF_ARCHIVE, "_touch_warm_pics", "", "persistent pictures map loads spent time on (CSQC precaches), \"flags name\" separated by ';' (touch_warm)"};
static cvar_t touch_warm_maxthermal = {CF_CLIENT, "touch_warm_maxthermal", "2", "touch_warm does nothing while touch_device_thermal is at least this"};
static cvar_t touch_device_thermal = {CF_CLIENT, "touch_device_thermal", "0", "the device's thermal status as the platform reports it (Android: 0 none, 1 light, 2 moderate, 3 severe, ...); set by the platform layer"};
static cvar_t touch_device_powersave = {CF_CLIENT, "touch_device_powersave", "0", "1 while the device is in battery saver; set by the platform layer"};

#define TW_MAXITEMS 1024

typedef struct tw_item_s
{
	char kind; // 'w' world, 'm' model, 'p' picture ("flags name"), 'g' GLSL ("mode perm"), 's' sound
	char name[MAX_QPATH + 16];
}
tw_item_t;

static struct
{
	char map[MAX_QPATH];   // being prepared or prepared
	tw_item_t *items;
	int count, next;
	qbool done;
	qbool dropped;         // low memory: nothing until the next game
	double last_input;
	double started, spent;
	int loaded[5], already, missing;
	volatile int lowmemory;
	qbool in_game;
}
tw;

void TouchWarm_Input(void)
{
	tw.last_input = host.realtime;
}

static int TW_KindIndex(char kind)
{
	switch (kind)
	{
	case 'w': return 0;
	case 'm': return 1;
	case 'p': return 2;
	case 'g': return 3;
	default: return 4;
	}
}

static void TW_Add(char kind, const char *name, void *ctx)
{
	tw_item_t *it;
	(void)ctx;
	if (tw.count >= TW_MAXITEMS || strlen(name) >= sizeof(it->name))
		return;
	it = &tw.items[tw.count++];
	it->kind = kind;
	dp_strlcpy(it->name, name, sizeof(it->name));
}

#define TW_MAXPICS 16

// touch_preload.h: kept when it cost over 2 ms, at most TW_MAXPICS
void TouchWarm_PicLoaded(const char *name, unsigned int cachepicflags, double seconds)
{
	char entry[MAX_QPATH + 16], list[(MAX_QPATH + 16) * TW_MAXPICS];
	const char *p;
	int n = 0;
	if (seconds < 0.002 || !touch_warm_pics.name || !Cvar_FindVar(&cvars_all, touch_warm_pics.name, ~0) || strchr(name, ';') || strlen(name) >= MAX_QPATH)
		return;
	dpsnprintf(entry, sizeof(entry), "%u %s", cachepicflags, name);
	for (p = touch_warm_pics.string; *p; n++)
	{
		const char *e = strchr(p, ';');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		if (len == strlen(entry) && !strncmp(p, entry, len))
			return;
		if (!e)
			break;
		p = e + 1;
	}
	if (n + 1 >= TW_MAXPICS)
		return;
	dpsnprintf(list, sizeof(list), "%s%s%s", touch_warm_pics.string, touch_warm_pics.string[0] ? ";" : "", entry);
	Cvar_SetQuick(&touch_warm_pics, list);
}

static void TW_AddPics(void)
{
	char buf[(MAX_QPATH + 16) * TW_MAXPICS];
	char *p, *e;
	dp_strlcpy(buf, touch_warm_pics.string, sizeof(buf));
	for (p = buf; *p; p = e + 1)
	{
		e = strchr(p, ';');
		if (e)
			*e = 0;
		if (*p)
			TW_Add('p', p, NULL);
		if (!e)
			break;
	}
}

static void TW_Reset(void)
{
	tw.map[0] = 0;
	tw.count = tw.next = 0;
	tw.done = false;
	tw.spent = 0;
	memset(tw.loaded, 0, sizeof(tw.loaded));
	tw.already = tw.missing = 0;
}

// the replay's order: world, models, pictures, shaders, sounds
static void TW_Plan(const char *map)
{
	int pass, i, n;
	tw_item_t *sorted;
	char world[MAX_QPATH + 16];
	TW_Reset();
	dp_strlcpy(tw.map, map, sizeof(tw.map));
	if (!tw.items)
		tw.items = (tw_item_t *)Mem_Alloc(cls.permanentmempool, sizeof(tw_item_t) * TW_MAXITEMS);
	dpsnprintf(world, sizeof(world), "maps/%s.bsp", map);
	TW_Add('w', world, NULL);
	TouchPreload_ForEach(map, TW_Add, NULL);
	TW_AddPics();
	sorted = (tw_item_t *)Mem_Alloc(tempmempool, sizeof(tw_item_t) * (tw.count ? tw.count : 1));
	for (pass = 0, n = 0; pass < 5; pass++)
		for (i = 0; i < tw.count; i++)
			if (TW_KindIndex(tw.items[i].kind) == pass)
				sorted[n++] = tw.items[i];
	memcpy(tw.items, sorted, sizeof(tw_item_t) * n);
	Mem_Free(sorted);
	tw.started = host.realtime;
}

// one item
static void TW_Load(tw_item_t *it)
{
	int r = -1;
	switch (it->kind)
	{
	case 'w':
	{
		model_t *mod;
		if (!FS_FileExists(it->name))
			break;
		mod = Mod_FindName(it->name, NULL);
		if (mod->loaded)
			r = 0;
		else
		{
			mod = Mod_ForName(it->name, false, false, NULL);
			r = (mod && mod->loaded) ? 1 : -1;
		}
		break;
	}
	case 'm':
		r = TouchModel_WarmName(it->name);
		break;
	case 's':
		r = TouchSnd_WarmName(it->name);
		break;
	case 'p':
	{
		unsigned int flags;
		int len;
		if (sscanf(it->name, "%u %n", &flags, &len) == 1 && it->name[len])
		{
			cachepic_t *pic = Draw_CachePic_Flags(it->name + len, flags);
			r = (pic && Draw_IsPicLoaded(pic)) ? 1 : -1;
		}
		break;
	}
	case 'g':
	{
		unsigned int mode;
		unsigned long long perm;
		if (sscanf(it->name, "%u %llx", &mode, &perm) == 2)
			r = TouchGLSL_Precompile(mode, perm) ? 1 : 0;
		break;
	}
	}
	if (r > 0)
		tw.loaded[TW_KindIndex(it->kind)]++;
	else if (r == 0)
		tw.already++;
	else
		tw.missing++;
}

static const char *TW_Target(void)
{
	if (touch_warm_map.string[0])
		return touch_warm_map.string;
	return touch_warm_last.string;
}

static qbool TW_MayRun(void)
{
	if (!touch_warm.integer || tw.dropped)
		return false;
	if (cls.state != ca_disconnected || sv.active || cls.demoplayback || key_consoleactive)
		return false;
	if (vid_hidden || !vid_activewindow)
		return false;
	if (touch_device_powersave.integer || touch_device_thermal.integer >= touch_warm_maxthermal.integer)
		return false;
	if (host.realtime - tw.last_input < touch_warm_idle.value)
		return false;
	return true;
}

static void TW_Drop(const char *why)
{
	// only outside a game: nothing references a model or sound then
	if (cls.state != ca_disconnected || sv.active)
		return;
	Mod_ClearUsed();
	Mod_PurgeUnused();
	S_ClearUsed();
	S_PurgeUnused();
	TW_Reset();
	tw.dropped = true;
	Con_DPrintf("touch_warm: freed what no game uses (%s)\n", why);
}

static void TW_Frame(void)
{
	double start, budget;
	const char *target;

	if (tw.lowmemory)
	{
		tw.lowmemory = 0;
		TW_Drop("low memory");
	}

	// a local game: remember its map, prepare again after it
	if (cls.state == ca_connected && cls.signon == SIGNONS && sv.active && sv.worldbasename[0])
	{
		if (!tw.in_game)
		{
			tw.in_game = true;
			if (strcmp(touch_warm_last.string, sv.worldbasename))
				Cvar_SetQuick(&touch_warm_last, sv.worldbasename);
		}
		return;
	}
	if (tw.in_game && cls.state == ca_disconnected)
	{
		tw.in_game = false;
		tw.dropped = false;
		TW_Reset();
		tw.last_input = host.realtime;
	}

	if (!TW_MayRun())
		return;
	target = TW_Target();
	if (!target[0])
		return;
	if (strcmp(tw.map, target))
		TW_Plan(target);
	if (tw.done)
		return;

	start = Sys_DirtyTime();
	budget = touch_warm_budget.value * 0.001;
	while (tw.next < tw.count)
	{
		TW_Load(&tw.items[tw.next++]);
		// input is polled between frames: the next frame sees it
		if (Sys_DirtyTime() - start >= budget)
			break;
	}
	tw.spent += Sys_DirtyTime() - start;
	if (tw.next >= tw.count)
	{
		tw.done = true;
		if (developer_loading.integer || Cvar_VariableValue(&cvars_all, "touch_loadtime", ~0))
			Con_Printf("LOADTIME warm %s items=%d world=%d models=%d pics=%d shaders=%d sounds=%d already=%d missing=%d work=%.1f ms over %.1f s\n",
				tw.map, tw.count, tw.loaded[0], tw.loaded[1], tw.loaded[2], tw.loaded[3], tw.loaded[4], tw.already, tw.missing,
				tw.spent * 1000.0, host.realtime - tw.started);
	}
}

void TouchWarm_Frame(void)
{
	TW_Frame();
}

static void TouchWarm_Drop_f(cmd_state_t *cmd)
{
	TW_Drop("touch_warm_drop");
}

static void TouchWarm_Status_f(cmd_state_t *cmd)
{
	Con_Printf("touch_warm: target \"%s\", prepared \"%s\" %d/%d%s%s, %.1f ms of work; idle %.1f s\n",
		TW_Target(), tw.map, tw.next, tw.count, tw.done ? " done" : "", tw.dropped ? " (dropped: low memory)" : "",
		tw.spent * 1000.0, host.realtime - tw.last_input);
	// why it would not run now: every gate of TW_MayRun
	Con_Printf("touch_warm: gates: enabled %d dropped %d state %d server %d demo %d console %d hidden %d active %d powersave %d thermal %d/%d idle %.1f/%.1f -> %s\n",
		touch_warm.integer, tw.dropped, (int)cls.state, (int)sv.active, (int)cls.demoplayback, key_consoleactive, (int)vid_hidden, (int)vid_activewindow,
		touch_device_powersave.integer, touch_device_thermal.integer, touch_warm_maxthermal.integer,
		host.realtime - tw.last_input, touch_warm_idle.value, TW_MayRun() ? "runs" : "waits");
}

#ifdef TOUCH_WARM_SDL
// may run on another thread (Android's onLowMemory): only flags it
static int SDLCALL TW_EventWatch(void *userdata, SDL_Event *event)
{
	if (event->type == SDL_APP_LOWMEMORY)
		tw.lowmemory = 1;
	return 0;
}
#endif

void TouchWarm_Init(void)
{
	Cvar_RegisterVariable(&touch_warm);
	Cvar_RegisterVariable(&touch_warm_idle);
	Cvar_RegisterVariable(&touch_warm_budget);
	Cvar_RegisterVariable(&touch_warm_map);
	Cvar_RegisterVariable(&touch_warm_last);
	Cvar_RegisterVariable(&touch_warm_pics);
	Cvar_RegisterVariable(&touch_warm_maxthermal);
	Cvar_RegisterVariable(&touch_device_thermal);
	Cvar_RegisterVariable(&touch_device_powersave);
	Cmd_AddCommand(CF_CLIENT, "touch_warm_drop", TouchWarm_Drop_f, "free the models and sounds no game uses and stop touch_warm until the next game (low memory)");
	Cmd_AddCommand(CF_CLIENT, "touch_warm_status", TouchWarm_Status_f, "what touch_warm prepared");
	// load-time switches with no init hook of their own in an upstream file
	TouchCRC_Init();
	TouchPRVM_Init();
	Touch_FS_Init(); // fs_zstd_lazy (touch_assets_fs.h, in fs.c)
#ifdef TOUCH_WARM_SDL
	SDL_AddEventWatch(TW_EventWatch, NULL);
#endif
	tw.last_input = host.realtime;
}
