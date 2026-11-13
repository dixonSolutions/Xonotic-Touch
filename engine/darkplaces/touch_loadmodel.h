// Godnotic: precached models load when first used (mod_lazyload).
// Included by model_shared.c, after its model list.
//
// A map load precached and loaded every model the server's QC and CSQC ask
// for: 315 on stormkeep, a third of them monsters, vehicles, turrets and other
// game modes' objects a deathmatch never shows (1.4 s on a desktop, 30 models
// over 10 ms each, the spider alone 131 ms).
//
// mod_lazyload 1: inside a precache (TouchModel_PrecacheScope around the
// server's SV_ModelIndex while the map loads, CSQC's precache_model and a
// remote client's precache list) Mod_ForName only registers a model whose
// file exists; the model loads the first time anything looks it up through
// SV_GetModelByIndex, SV_GetModelFromEdict, CL_GetModelByIndex or CSQC's
// setmodel - setmodel, collision, entity drawing all go through those - and
// while a menu or dialog is open in the game (the welcome dialog after a
// join, the game menu) the small ones load within mod_lazyload_budget ms per
// frame.
// World and brush models (*.bsp, *N submodels), a missing file, a reload of
// a loaded model and the experimental server thread keep loading at once.

#include "touch_warm.h"

static cvar_t mod_lazyload = {CF_CLIENT | CF_SERVER, "mod_lazyload", "1", "precached models load when first used, or in the background during the game (Godnotic); 0 = all at precache"};
static cvar_t mod_lazyload_budget = {CF_CLIENT | CF_SERVER, "mod_lazyload_budget", "8", "milliseconds per frame spent loading precached models not used yet while a menu or dialog is open in the game (0 = only on first use)"};
static cvar_t mod_lazyload_maxsize = {CF_CLIENT | CF_SERVER, "mod_lazyload_maxsize", "1536", "models whose file is bigger than this many KiB are not loaded in the background, only on first use (one load cannot be split across frames)"};

#define TOUCHMOD_MAX 4096
static model_t *touchmod_deferred[TOUCHMOD_MAX];
static signed char touchmod_big[TOUCHMOD_MAX]; // -1 unknown, 0 small, 1 first use only
static int touchmod_numdeferred;
static int touchmod_scope;

static int TouchModel_Find(const model_t *mod)
{
	int i;
	for (i = 0; i < touchmod_numdeferred; i++)
		if (touchmod_deferred[i] == mod)
			return i;
	return -1;
}

static void TouchModel_Remove(int i)
{
	--touchmod_numdeferred;
	touchmod_deferred[i] = touchmod_deferred[touchmod_numdeferred];
	touchmod_big[i] = touchmod_big[touchmod_numdeferred];
}

void TouchModel_PrecacheScope(int enter)
{
	touchmod_scope += enter ? 1 : -1;
	if (touchmod_scope < 0)
		touchmod_scope = 0;
}

// Mod_ForName, before loading: true when the model is only registered.
static qbool TouchModel_Defer(model_t *mod, qbool checkdisk)
{
	size_t len;
	// checkdisk (the client's download pass) reloads a model whose file
	// changed; one not loaded yet will read the current file when it loads
	if (!touchmod_scope || !mod_lazyload.integer || mod->loaded || svs.threaded)
		return false;
	if (!mod->name[0] || mod->name[0] == '*' || !strcmp(mod->name, "null"))
		return false;
	len = strlen(mod->name);
	if (len >= 4 && !strcasecmp(mod->name + len - 4, ".bsp"))
		return false;
	if (TouchModel_Find(mod) >= 0)
		return true;
	if (touchmod_numdeferred >= TOUCHMOD_MAX || !FS_FileExists(mod->name))
		return false;
	touchmod_big[touchmod_numdeferred] = -1;
	touchmod_deferred[touchmod_numdeferred++] = mod;
	return true;
}

qbool TouchModel_IsDeferred(model_t *mod)
{
	return mod && !mod->loaded && touchmod_numdeferred && TouchModel_Find(mod) >= 0;
}

// loaded by touch_preload and not used yet: its first use keeps it listed
static model_t *touchmod_preloaded[TOUCHMOD_MAX];
static int touchmod_numpreloaded;

static void TouchModel_FirstUse(model_t *mod)
{
	int i;
	for (i = 0; i < touchmod_numpreloaded; i++)
	{
		if (touchmod_preloaded[i] == mod)
		{
			touchmod_preloaded[i] = touchmod_preloaded[--touchmod_numpreloaded];
			TouchPreload_Used('m', mod->name);
			return;
		}
	}
}

model_t *TouchModel_Ensure(model_t *mod)
{
	int i;
	if (mod && touchmod_numpreloaded && mod->loaded)
		TouchModel_FirstUse(mod);
	if (!mod || mod->loaded || !touchmod_numdeferred || (i = TouchModel_Find(mod)) < 0)
		return mod;
	TouchModel_Remove(i);
	{
		double t0 = Sys_DirtyTime();
		Mod_LoadModel(mod, false, false);
		TouchPreload_Loaded('m', mod->name, Sys_DirtyTime() - t0);
	}
	return mod;
}

// touch_preload: load a registered model before its first use
qbool TouchModel_PreloadName(const char *name)
{
	int i;
	for (i = 0; i < touchmod_numdeferred; i++)
	{
		model_t *mod = touchmod_deferred[i];
		if (!strcmp(mod->name, name))
		{
			TouchModel_Remove(i);
			if (mod->loaded)
				return false;
			Mod_LoadModel(mod, false, false);
			if (touchmod_numpreloaded < TOUCHMOD_MAX)
				touchmod_preloaded[touchmod_numpreloaded++] = mod;
			return true;
		}
	}
	return false;
}

// touch_warm.c: load a model while the menu is idle, before the game that
// will use it; its first use in that game keeps it on the preload list as a
// replayed one's does. 1 loaded now, 0 already loaded, -1 no such file.
int TouchModel_WarmName(const char *name)
{
	model_t *mod;
	int i;
	if (!FS_FileExists(name))
		return -1;
	mod = Mod_FindName(name, NULL);
	if (mod->loaded)
		return 0;
	if ((i = TouchModel_Find(mod)) >= 0)
		TouchModel_Remove(i);
	Mod_LoadModel(mod, false, false);
	if (!mod->loaded)
		return -1;
	if (touchmod_numpreloaded < TOUCHMOD_MAX)
		touchmod_preloaded[touchmod_numpreloaded++] = mod;
	return 1;
}

// Mod_PurgeUnused frees this model.
static void TouchModel_Purged(model_t *mod)
{
	int i = TouchModel_Find(mod);
	if (i >= 0)
		TouchModel_Remove(i);
	for (i = 0; i < touchmod_numpreloaded; i++)
		if (touchmod_preloaded[i] == mod)
			touchmod_preloaded[i--] = touchmod_preloaded[--touchmod_numpreloaded];
}

// once per host frame in the game (touch_loadtime.c)
static void TouchModel_LoadSome(void)
{
	double start, budget = mod_lazyload_budget.value * 0.001;
	int i;
	if (budget <= 0 || !touchmod_numdeferred || svs.threaded)
		return;
	start = Sys_DirtyTime();
	for (i = touchmod_numdeferred - 1; i >= 0; i--)
	{
		model_t *mod = touchmod_deferred[i];
		if (mod->loaded)
		{
			TouchModel_Remove(i);
			continue;
		}
		if (touchmod_big[i] < 0)
		{
			qfile_t *f;
			fs_offset_t size = -1;
			if ((f = FS_OpenVirtualFile(mod->name, true)))
			{
				size = FS_FileSize(f);
				FS_Close(f);
			}
			touchmod_big[i] = (size < 0 || size > mod_lazyload_maxsize.value * 1024) ? 1 : 0;
		}
		if (touchmod_big[i])
			continue; // on first use only
		TouchModel_Remove(i);
		Mod_LoadModel(mod, false, false);
		if (Sys_DirtyTime() - start >= budget)
			return;
	}
}

static void TouchModel_Init(void)
{
	Cvar_RegisterVariable(&mod_lazyload);
	Cvar_RegisterVariable(&mod_lazyload_budget);
	Cvar_RegisterVariable(&mod_lazyload_maxsize);
	touch_model_loadsome = TouchModel_LoadSome;
}
