// Godnotic: sounds decoded on first play or in the background (snd_lazyload).
// Included by snd_main.c, after its list of known sounds.
//
// S_PrecacheSound decoded every sound it was given: the client precaches the
// server's list (~500 on a Xonotic map) and CSQC adds hundreds more, nearly
// all short Vorbis or Opus effects decoded whole into memory. That was the
// largest single part of a map load (2.5 s on a desktop for stormkeep).
//
// snd_lazyload 1: a precache of a file that exists marks the sfx deferred and
// returns; S_IsSoundPrecached counts it as precached. It is decoded the first
// time it is started (S_StartSound, S_StaticSound) or its length is asked
// for, and while a menu or dialog is open in the game (the welcome dialog
// after a join, the game menu) the rest are decoded within
// snd_lazyload_budget milliseconds per frame. A name with no file behind it
// is loaded (and found missing) at once, as before, and so is a file bigger
// than snd_lazyload_maxsize KiB: one decode of it cannot be split across
// frames, so it would be a long frame in the game.

#define TOUCH_SFXFLAG_DEFERRED (1u << 28)

static cvar_t snd_lazyload = {CF_CLIENT, "snd_lazyload", "1", "precached sounds are decoded when first played, or in the background during the game (Godnotic); 0 = all at precache"};
static cvar_t snd_lazyload_maxsize = {CF_CLIENT, "snd_lazyload_maxsize", "48", "sound files bigger than this many KiB are decoded at precache even with snd_lazyload (one decode of a big file would be a long frame in the game)"};
static cvar_t snd_lazyload_budget = {CF_CLIENT, "snd_lazyload_budget", "4", "milliseconds per frame spent decoding precached sounds not played yet while a menu or dialog is open in the game (0 = only on first play)"};

// The size of the file S_LoadSound would read for name (sound/<name> or
// <name>, a .wav also as .ogg, Touch_Opus_LoadFile's .opus for a .ogg), or -1
// when there is none.
static fs_offset_t TouchSnd_FileSize(const char *name)
{
	char buf[MAX_QPATH + 16];
	size_t len;
	int pass, ext;
	for (pass = 0; pass < 2; pass++)
	{
		if (pass == 0)
		{
			if (!strncasecmp(name, "sound/", 6))
				continue;
			dpsnprintf(buf, sizeof(buf), "sound/%s", name);
		}
		else
			dpsnprintf(buf, sizeof(buf), "%s", name);
		len = strlen(buf);
		for (ext = 0; ext < 3; ext++)
		{
			qfile_t *f;
			if (ext > 0)
			{
				if (len < 4 || (strcasecmp(buf + len - 4, ".wav") && strcasecmp(buf + len - 4, ".ogg")))
					break;
				if (ext == 1)
					memcpy(buf + len - 3, "ogg", 4);
				else if (len + 1 < sizeof(buf))
					memcpy(buf + len - 3, "opus", 5);
				else
					break;
			}
			if (FS_FileExists(buf) && (f = FS_OpenVirtualFile(buf, true)))
			{
				fs_offset_t size = FS_FileSize(f);
				FS_Close(f);
				return size;
			}
		}
	}
	return -1;
}

// S_PrecacheSound, instead of loading: true when left for later.
static qbool TouchSnd_Defer(sfx_t *sfx)
{
	fs_offset_t size;
	if (sfx->fetcher || (sfx->flags & TOUCH_SFXFLAG_DEFERRED))
		return sfx->fetcher == NULL;
	if (!snd_lazyload.integer || nosound.integer || !snd_precache.integer || snd_renderbuffer == NULL)
		return false;
	// a big file takes one long decode however it is scheduled (tens of
	// milliseconds on a desktop, several times that on a phone): that stays
	// in the load rather than becoming a hitch in the game
	size = TouchSnd_FileSize(sfx->name);
	if (size < 0 || size > snd_lazyload_maxsize.value * 1024)
		return false;
	sfx->flags |= TOUCH_SFXFLAG_DEFERRED;
	return true;
}

static qbool TouchSnd_Deferred(const sfx_t *sfx)
{
	return sfx && (sfx->flags & TOUCH_SFXFLAG_DEFERRED);
}

static void TouchSnd_Ensure(sfx_t *sfx)
{
	if (!sfx || !(sfx->flags & TOUCH_SFXFLAG_DEFERRED))
		return;
	sfx->flags &= ~TOUCH_SFXFLAG_DEFERRED;
	if (!sfx->fetcher)
		S_LoadSound(sfx, true);
}

// once per host frame in the game (touch_loadtime.c)
static void TouchSnd_LoadSome(void)
{
	double start, budget = snd_lazyload_budget.value * 0.001;
	sfx_t *sfx;
	if (budget <= 0 || snd_renderbuffer == NULL)
		return;
	start = Sys_DirtyTime();
	for (sfx = known_sfx; sfx; sfx = sfx->next)
	{
		if (!(sfx->flags & TOUCH_SFXFLAG_DEFERRED))
			continue;
		{
			double t0 = Sys_DirtyTime();
			TouchSnd_Ensure(sfx);
			if (developer_loading.integer && Sys_DirtyTime() - t0 > 0.005)
				Con_Printf("snd_lazyload: %s took %.1f ms in the background\n", sfx->name, (Sys_DirtyTime() - t0) * 1000.0);
		}
		if (Sys_DirtyTime() - start >= budget)
			break;
	}
	if (developer_loading.integer && Sys_DirtyTime() - start > 0.010)
		Con_Printf("snd_lazyload: frame spent %.1f ms\n", (Sys_DirtyTime() - start) * 1000.0);
}

static void TouchSnd_Init(void)
{
	Cvar_RegisterVariable(&snd_lazyload);
	Cvar_RegisterVariable(&snd_lazyload_budget);
	Cvar_RegisterVariable(&snd_lazyload_maxsize);
	touch_snd_loadsome = TouchSnd_LoadSome;
}
