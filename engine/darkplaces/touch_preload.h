// Godnotic: what a game loaded late, loaded again before its first frame
// (touch_preload). Included by touch_loadtime.c.
//
// mod_lazyload and snd_lazyload moved most of a map load into the game: on a
// Mali tablet (stormkeep, 4 bots) the frame where the bots joined took
// 240-270 ms, 190 ms of it the server loading their player models, LODs and
// the first weapon models through setmodel, and the first weapon pickups and
// HUD pictures cost 30-90 ms frames later on.
//
// Recording: every model, picture, sound and GLSL permutation a game loads on
// first use after its first frame is noted with what it cost
// (developer_loading 1 prints each one over 1 ms). The list is kept per map
// in the userdir, touch_preload/<map>.txt, merged with what earlier games on
// that map needed; an entry neither loaded late nor used after being
// preloaded in touch_preload_keep games of the map is dropped.
//
// Replay: at the end of the next load of that map, under the loading screen
// (once the client has its precache list and CSQC is up, before the first
// game frame), the entries are loaded in the order models, pictures, shaders,
// sounds, within touch_preload_budget ms. A first game on a map with no list
// uses the built-in seed below: every player model and LOD the bots pick
// from, the weapons a deathmatch gives or shows first, and what every match
// on the tablet showed in its first seconds.
//
// GLSL permutations are listed for completeness but cost about a
// millisecond each to compile and link on the Mali-G68 (Android keeps its own
// shader cache), so a program binary cache would buy nothing measurable there.

#include "touch_warm.h"

static cvar_t touch_preload = {CF_CLIENT, "touch_preload", "1", "load what earlier games on this map loaded late (models, sounds, GLSL shaders) under the loading screen before the first game frame, and record what this game loads late (Godnotic); 0 = neither"};
static cvar_t touch_preload_budget = {CF_CLIENT, "touch_preload_budget", "4000", "milliseconds the preload may add to a map load at most"};
static cvar_t touch_preload_keep = {CF_CLIENT, "touch_preload_keep", "4", "an entry of a map's preload list is dropped once that many games on the map did not need it"};
static cvar_t touch_preload_seed = {CF_CLIENT, "touch_preload_seed", "1", "with no preload list for a map yet, preload the built-in list of player models, weapons and shaders a game always needs"};

#define TP_MAX 1024
#define TP_NAMELEN MAX_QPATH

typedef struct tp_entry_s
{
	char kind;      // 'm' model, 'g' GLSL permutation ("mode perm"), 's' sound
	char name[TP_NAMELEN];
	int age;        // games of this map since it was last needed
	float cost;     // ms it took when loaded late
	qbool used;     // loaded late in this game
}
tp_entry_t;

static struct
{
	tp_entry_t list[TP_MAX];
	int count;
	char map[MAX_QPATH];    // map whose list is in memory
	qbool dirty;
	qbool replaying;        // inside the replay: loads are not late
	qbool done;             // replayed for this load
	double last_save;
	unsigned int glsl_mode; // permutation being compiled (TouchPreload_Shader)
	uint64_t glsl_perm;
	qbool glsl_pending;
	double glsl_begin;
	int late_count;
	double late_ms;
}
tp;

// the seed: names as the late list records them
static const char *tp_seed[] =
{
	// player models the bots (bots.txt) and the default player pick from
	"m models/player/erebus.iqm", "m models/player/erebus_lod1.iqm", "m models/player/erebus_lod2.iqm",
	"m models/player/megaerebus.iqm", "m models/player/megaerebus_lod1.iqm", "m models/player/megaerebus_lod2.iqm",
	"m models/player/gak.iqm", "m models/player/gak_lod1.iqm", "m models/player/gak_lod2.iqm",
	"m models/player/gakmasked.iqm", "m models/player/gakmasked_lod1.iqm", "m models/player/gakmasked_lod2.iqm",
	"m models/player/ignis.iqm", "m models/player/ignis_lod1.iqm", "m models/player/ignis_lod2.iqm",
	"m models/player/ignismasked.iqm", "m models/player/ignismasked_lod1.iqm", "m models/player/ignismasked_lod2.iqm",
	"m models/player/nyx.iqm", "m models/player/nyx_lod1.iqm", "m models/player/nyx_lod2.iqm",
	"m models/player/pyria.iqm", "m models/player/pyria_lod1.iqm", "m models/player/pyria_lod2.iqm",
	"m models/player/seraphina.iqm", "m models/player/seraphina_lod1.iqm", "m models/player/seraphina_lod2.iqm",
	"m models/player/seraphinamasked.iqm", "m models/player/seraphinamasked_lod1.iqm", "m models/player/seraphinamasked_lod2.iqm",
	"m models/player/umbra.iqm", "m models/player/umbra_lod1.iqm", "m models/player/umbra_lod2.iqm",
	// start weapons and the most common pickups: view, held and world models
	"m models/weapons/h_shotgun.iqm", "m models/weapons/v_shotgun.md3", "m models/weapons/g_shotgun.md3",
	"m models/weapons/h_laser.iqm", "m models/weapons/v_laser.md3", "m models/weapons/g_laser.md3",
	"m models/weapons/h_nex.iqm", "m models/weapons/v_nex.md3", "m models/weapons/g_nex.md3",
	"m models/weapons/h_rl.iqm", "m models/weapons/v_rl.md3",
	"m models/weapons/h_uzi.iqm", "m models/weapons/v_uzi.md3",
	"m models/weapons/h_gl.iqm", "m models/weapons/v_gl.md3",
	"m models/weapons/h_electro.iqm", "m models/weapons/v_electro.md3",
	"m models/weapons/h_crylink.iqm", "m models/weapons/v_crylink.md3",
	"m models/weapons/h_hagar.iqm", "m models/weapons/v_hagar.md3",
	"m models/weapons/h_ok_mg.iqm", "m models/weapons/v_ok_mg.md3",
	// what every match showed in its first seconds on the tablet
	"m models/relics/relic.md3", "m models/misc/chatbubble.spr", "m models/uziflash.md3", "m models/casing_shell.mdl",
	"m models/gibs/eye.md3", "m models/gibs/bloodyskull.md3", "m models/gibs/arm.md3", "m models/gibs/chest.md3",
	"m models/gibs/smallchest.md3", "m models/gibs/chunk.mdl",
	"s misc/spawn.ogg", "s weapons/weapon_switch.wav", "s weapons/weaponpickup.wav", "s weapons/shotgun_fire.wav",
	"s weapons/shotgun_melee.wav", "s weapons/lasergun_fire.wav", "s weapons/laserimpact.ogg",
	"s weapons/ric1.ogg", "s weapons/ric2.ogg", "s weapons/ric3.ogg",
	"s weapons/casings1.ogg", "s weapons/casings2.ogg", "s weapons/casings3.ogg",
	"s weapons/nexfire.wav", "s weapons/nexcharge.wav", "s weapons/neximpact.ogg",
	"s misc/itempickup.wav", "s misc/itemrespawncountdown.wav", "s misc/itemrespawn.wav", "s misc/minihealth.wav", "s misc/armor25.wav",
	"s misc/gib.ogg", "s misc/jumppad.wav",
	NULL
};

static tp_entry_t *TP_Find(char kind, const char *name)
{
	int i;
	for (i = 0; i < tp.count; i++)
		if (tp.list[i].kind == kind && !strcmp(tp.list[i].name, name))
			return &tp.list[i];
	return NULL;
}

static tp_entry_t *TP_Add(char kind, const char *name)
{
	tp_entry_t *e = TP_Find(kind, name);
	if (e || tp.count >= TP_MAX || strlen(name) >= TP_NAMELEN)
		return e;
	e = &tp.list[tp.count++];
	memset(e, 0, sizeof(*e));
	e->kind = kind;
	dp_strlcpy(e->name, name, sizeof(e->name));
	return e;
}

static void TP_Path(char *path, size_t size, const char *map)
{
	dpsnprintf(path, size, "touch_preload/%s.txt", map);
}

static void TP_Save(void)
{
	char path[MAX_QPATH + 32];
	qfile_t *f;
	int i;
	if (!tp.map[0] || !tp.dirty)
		return;
	TP_Path(path, sizeof(path), tp.map);
	if (!(f = FS_OpenRealFile(path, "wb", true)))
		return;
	FS_Printf(f, "// Godnotic touch_preload: kind age cost-ms name (m model, p picture: flags name, g GLSL mode+permutation, s sound)\n");
	for (i = 0; i < tp.count; i++)
	{
		tp_entry_t *e = &tp.list[i];
		// a GLSL permutation costs about a millisecond: kept
		if (e->kind == 'g' && e->age > 1)
			e->age = 1;
		if (e->age <= touch_preload_keep.integer)
			FS_Printf(f, "%c %d %.1f %s\n", e->kind, e->used ? 0 : e->age, e->cost, e->name);
	}
	FS_Close(f);
	tp.dirty = false;
	tp.last_save = Sys_DirtyTime();
}

// a new game on map: save the last one's list, read this one's (one more
// game of the map for every entry)
static qbool TP_Open(const char *map)
{
	char path[MAX_QPATH + 32];
	char *text, *p;
	qbool found = false;
	TP_Save();
	tp.count = 0;
	tp.dirty = false;
	dp_strlcpy(tp.map, map, sizeof(tp.map));
	TP_Path(path, sizeof(path), map);
	if ((text = (char *)FS_LoadFile(path, tempmempool, true, NULL)))
	{
		found = true;
		for (p = text; *p; )
		{
			char kind, name[TP_NAMELEN + 64];
			int age;
			float cost;
			char *eol = strchr(p, '\n');
			if (eol)
				*eol = 0;
			if (p[0] != '/' && sscanf(p, "%c %d %f %191[^\r\n]", &kind, &age, &cost, name) == 4 && (kind == 'm' || kind == 'g' || kind == 's' || kind == 'p'))
			{
				tp_entry_t *e = TP_Add(kind, name);
				if (e)
				{
					e->age = age + 1;
					e->cost = cost;
				}
			}
			if (!eol)
				break;
			p = eol + 1;
		}
		Mem_Free(text);
		// the aged list is what the next game reads if this one adds nothing
		tp.dirty = true;
	}
	return found;
}

// model_shared.c, snd_main.c, gl_rmain.c (through the helpers below)
void TouchPreload_Loaded(int kind, const char *name, double seconds)
{
	tp_entry_t *e;
	if (tp.replaying || tlt.loading || tlt.background)
		return;
	if (cls.state != ca_connected || cls.signon != SIGNONS || !cl.worldbasename[0])
		return;
	if (developer_loading.integer && seconds > 0.001)
		Con_Printf("LOADTIME late %s %s %.1f ms at %.0f ms in the game\n", kind == 'm' ? "model" : kind == 's' ? "sound" : kind == 'p' ? "pic" : "glsl", name, seconds * 1000.0, (Sys_DirtyTime() - tlt.game_time) * 1000.0);
	tp.late_count++;
	tp.late_ms += seconds * 1000.0;
	if (!touch_preload.integer)
		return;
	if (strcmp(tp.map, cl.worldbasename))
		TP_Open(cl.worldbasename);
	if (!(e = TP_Add((char)kind, name)))
		return;
	if (!e->used || seconds * 1000.0 > e->cost)
		e->cost = seconds * 1000.0;
	e->used = true;
	e->age = 0;
	tp.dirty = true;
}

void TouchPreload_Used(int kind, const char *name)
{
	tp_entry_t *e;
	if (!touch_preload.integer || tp.replaying || !(e = TP_Find((char)kind, name)) || e->used)
		return;
	e->used = true;
	e->age = 0;
	tp.dirty = true;
}

// gl_rmain.c R_GLSL_CompilePermutation: the permutation being compiled; its
// time is the GLSL phase that follows (TouchLoadTime_End)
void TouchPreload_Shader(unsigned int mode, unsigned long long permutation)
{
	tp.glsl_mode = mode;
	tp.glsl_perm = permutation;
	tp.glsl_pending = true;
	tp.glsl_begin = Sys_DirtyTime();
}

static void TP_ShaderDone(void)
{
	char name[64];
	if (!tp.glsl_pending)
		return;
	tp.glsl_pending = false;
	dpsnprintf(name, sizeof(name), "%u %llx", tp.glsl_mode, (unsigned long long)tp.glsl_perm);
	TouchPreload_Loaded('g', name, Sys_DirtyTime() - tp.glsl_begin);
}

// gl_draw.c: a picture loaded in the game (the HUD's first draw of it);
// recorded as "flags name"
static double tp_pic_begin;
static int tp_pic_depth, tp_pic_loads;
void TouchPreload_PicBegin(void)
{
	if (tp_pic_depth++ == 0)
		tp_pic_begin = Sys_DirtyTime();
}

void TouchPreload_PicEnd(const char *name, unsigned int cachepicflags)
{
	char entry[TP_NAMELEN + 16];
	if (tp_pic_depth <= 0 || --tp_pic_depth > 0)
		return;
	tp_pic_loads++;
	// a persistent picture a map load spent time on (CSQC's precaches):
	// touch_warm loads it while the menu is idle before the next game
	if (tlt.loading && !(cachepicflags & CACHEPICFLAG_NOTPERSISTENT))
		TouchWarm_PicLoaded(name, cachepicflags, Sys_DirtyTime() - tp_pic_begin);
	// the replay loads it persistent, so it is not unloaded before its use
	dpsnprintf(entry, sizeof(entry), "%u %s", cachepicflags & ~CACHEPICFLAG_NOTPERSISTENT, name);
	TouchPreload_Loaded('p', entry, Sys_DirtyTime() - tp_pic_begin);
}

static int TP_KindOrder(char kind)
{
	return kind == 'm' ? 0 : kind == 'p' ? 1 : kind == 'g' ? 2 : 3;
}

// under the loading screen, once per load: load the list
static void TP_Replay(void)
{
	double start = Sys_DirtyTime(), budget = touch_preload_budget.value * 0.001, t0;
	double spent[4] = {0, 0, 0, 0};
	int n[4] = {0, 0, 0, 0}, pass, i, skipped = 0;
	qbool have;
	if (!touch_preload.integer || !cl.worldbasename[0])
		return;
	have = TP_Open(cl.worldbasename);
	if (!have && touch_preload_seed.integer)
	{
		for (i = 0; tp_seed[i]; i++)
		{
			tp_entry_t *e = TP_Add(tp_seed[i][0], tp_seed[i] + 2);
			if (e)
				e->age = 1;
		}
	}
	tp.replaying = true;
	for (pass = 0; pass < 4; pass++)
	{
		for (i = 0; i < tp.count; i++)
		{
			tp_entry_t *e = &tp.list[i];
			qbool loaded = false;
			if (TP_KindOrder(e->kind) != pass)
				continue;
			if (Sys_DirtyTime() - start >= budget)
			{
				skipped++;
				continue;
			}
			t0 = Sys_DirtyTime();
			if (e->kind == 'm')
				loaded = TouchModel_PreloadName(e->name);
			else if (e->kind == 's')
				loaded = TouchSnd_PreloadName(e->name);
			else if (e->kind == 'p')
			{
				unsigned int flags;
				int len;
				if (sscanf(e->name, "%u %n", &flags, &len) == 1 && e->name[len])
				{
					int before = tp_pic_loads;
					Draw_CachePic_Flags(e->name + len, flags);
					loaded = tp_pic_loads != before;
				}
			}
			else
			{
				unsigned int mode;
				unsigned long long perm;
				if (sscanf(e->name, "%u %llx", &mode, &perm) == 2)
					loaded = TouchGLSL_Precompile(mode, perm);
			}
			if (loaded)
			{
				n[pass]++;
				spent[pass] += Sys_DirtyTime() - t0;
			}
		}
	}
	tp.replaying = false;
	tp.glsl_pending = false;
	if (touch_loadtime.integer || developer_loading.integer)
		Con_Printf("LOADTIME preload %s list=%d models=%d (%.1f ms) pics=%d (%.1f ms) shaders=%d (%.1f ms) sounds=%d (%.1f ms) over_budget=%d\n",
			have ? "map" : (touch_preload_seed.integer ? "seed" : "none"), tp.count, n[0], spent[0] * 1000.0, n[1], spent[1] * 1000.0, n[2], spent[2] * 1000.0, n[3], spent[3] * 1000.0, skipped);
}

// touch_warm.c: the entries the next game on map would preload (its list,
// or the seed when it has none), without opening the list as a game does;
// returns how many were passed to fn
int TouchPreload_ForEach(const char *map, void (*fn)(char kind, const char *name, void *ctx), void *ctx)
{
	char path[MAX_QPATH + 32];
	char *text, *p;
	int i, n = 0;
	if (!touch_preload.integer || !map || !map[0])
		return 0;
	TP_Path(path, sizeof(path), map);
	if ((text = (char *)FS_LoadFile(path, tempmempool, true, NULL)))
	{
		for (p = text; *p; )
		{
			char kind, name[TP_NAMELEN + 64];
			int age;
			float cost;
			char *eol = strchr(p, '\n');
			if (eol)
				*eol = 0;
			if (p[0] != '/' && sscanf(p, "%c %d %f %191[^\r\n]", &kind, &age, &cost, name) == 4 && (kind == 'm' || kind == 'g' || kind == 's' || kind == 'p'))
			{
				fn(kind, name, ctx);
				n++;
			}
			if (!eol)
				break;
			p = eol + 1;
		}
		Mem_Free(text);
		return n;
	}
	if (!touch_preload_seed.integer)
		return 0;
	for (i = 0; tp_seed[i]; i++, n++)
		fn(tp_seed[i][0], tp_seed[i] + 2, ctx);
	return n;
}

// once per host frame
static void TP_Frame(double now)
{
	if (!touch_preload.integer)
		return;
	// a load: replay at its last signon step, before the first game frame
	// (the client has its precaches and CSQC has run CSQC_Init by then)
	if (tlt.loading && !tp.done && cls.state == ca_connected && cls.signon >= SIGNONS - 1 && cl.worldbasename[0])
	{
		tp.done = true;
		TP_Replay();
	}
	if (!tlt.loading)
		tp.done = false;
	// written soon after anything new, so a killed app keeps it
	if (tp.dirty && now - tp.last_save > 2 && cls.state == ca_connected && cls.signon == SIGNONS)
		TP_Save();
	if (cls.state != ca_connected && tp.dirty)
		TP_Save();
}

static void TP_Init(void)
{
	Cvar_RegisterVariable(&touch_preload);
	Cvar_RegisterVariable(&touch_preload_budget);
	Cvar_RegisterVariable(&touch_preload_keep);
	Cvar_RegisterVariable(&touch_preload_seed);
}
