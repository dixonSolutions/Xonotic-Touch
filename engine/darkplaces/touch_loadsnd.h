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
// is loaded (and found missing) at once, as before. A file bigger than
// snd_lazyload_maxsize KiB would be one long frame however it is scheduled:
// a Vorbis one is decoded on worker threads during the load
// (snd_lazyload_async, touch_loadogg.h), any other at precache as before.


#include "touch_warm.h"

static cvar_t snd_lazyload = {CF_CLIENT, "snd_lazyload", "1", "precached sounds are decoded when first played, or in the background during the game (Godnotic); 0 = all at precache"};
static cvar_t snd_lazyload_maxsize = {CF_CLIENT, "snd_lazyload_maxsize", "48", "sound files bigger than this many KiB are decoded at precache even with snd_lazyload (one decode of a big file would be a long frame in the game)"};
static cvar_t snd_lazyload_maxlength = {CF_CLIENT, "snd_lazyload_maxlength", "4", "Ogg sounds (Vorbis or Opus) that decode to more than this many seconds are decoded during the load, on worker threads where there are any, even with snd_lazyload: what a first play would cost goes by the length, and an Opus file holds about twice the audio of a Vorbis file of its size; 0 = go by snd_lazyload_maxsize alone (Godnotic)"};
static cvar_t snd_lazyload_budget = {CF_CLIENT, "snd_lazyload_budget", "4", "milliseconds per frame spent decoding precached sounds not played yet while a menu or dialog is open in the game (0 = only on first play)"};

// The file S_LoadSound would read for name (sound/<name> or <name>, a .wav
// also as .ogg, Touch_Opus_LoadFile's .opus for a .ogg): its size, and its
// path in path; -1 when there is none.
static fs_offset_t TouchSnd_File(const char *name, char *path, size_t pathsize)
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
				dp_strlcpy(path, buf, pathsize);
				return size;
			}
		}
	}
	return -1;
}

// How long an Ogg file plays, from its first page (codec, sample rate, Opus
// pre-skip) and the granule position of its last page; -1 when it is not an
// Ogg Vorbis or Opus file or cannot be told. Two small reads, no decoder.
static double TouchSnd_OggSeconds(const char *path, fs_offset_t size)
{
	unsigned char head[64], tail[8192];
	fs_offset_t n, i, at;
	double rate, skip = 0;
	int64_t granule = -1;
	qfile_t *f = FS_OpenVirtualFile(path, true);
	if (!f)
		return -1;
	n = FS_Read(f, head, sizeof(head));
	if (n < 48 || memcmp(head, "OggS", 4))
	{
		FS_Close(f);
		return -1;
	}
	i = 27 + head[26];
	if (i + 19 <= n && !memcmp(head + i, "OpusHead", 8))
	{
		rate = 48000;
		skip = head[i + 10] | (head[i + 11] << 8);
	}
	else if (i + 16 <= n && !memcmp(head + i, "\x01vorbis", 7))
		rate = head[i + 12] | (head[i + 13] << 8) | (head[i + 14] << 16) | ((unsigned)head[i + 15] << 24);
	else
		rate = 0;
	// the last page starts within its own size of the end: 8 KiB holds it for
	// every sound effect (a page is a quarter second or so of audio)
	at = max(0, size - (fs_offset_t)sizeof(tail));
	if (rate > 0 && FS_Seek(f, at, SEEK_SET) == 0)
	{
		n = FS_Read(f, tail, sizeof(tail));
		for (i = n - 27; i >= 0; i--)
		{
			if (!memcmp(tail + i, "OggS", 4) && tail[i + 4] == 0)
			{
				int b;
				granule = 0;
				for (b = 7; b >= 0; b--)
					granule = (granule << 8) | tail[i + 6 + b];
				break;
			}
		}
	}
	FS_Close(f);
	if (granule < 0)
		return -1;
	return max(0, (double)granule - skip) / rate;
}

// An Ogg file by its name: .ogg (Vorbis or Opus), or .opus (the name
// TouchSnd_File and Touch_Opus_LoadFile also try for a .ogg)
static qbool TouchSnd_IsOgg(const char *path)
{
	size_t len = strlen(path);
	return (len >= 4 && !strcasecmp(path + len - 4, ".ogg")) || (len >= 5 && !strcasecmp(path + len - 5, ".opus"));
}

// snd_ogg.c (touch_loadogg.h): big Vorbis and Opus files decoded on worker threads
void TouchOgg_Init(void);
qbool TouchOgg_StartAsync(sfx_t *sfx, const char *filename);
qbool TouchOgg_Pending(const sfx_t *sfx);
void TouchOgg_Finish(sfx_t *sfx);
void TouchOgg_Forget(sfx_t *sfx);
void TouchOgg_Poll(void);

// S_PrecacheSound, instead of loading: true when left for later.
static qbool TouchSnd_Defer(sfx_t *sfx)
{
	fs_offset_t size;
	char path[MAX_QPATH + 16];
	qbool big;
	if (sfx->fetcher || (sfx->flags & TOUCH_SFXFLAG_DEFERRED))
		return sfx->fetcher == NULL;
	if (!snd_lazyload.integer || nosound.integer || !snd_precache.integer || snd_renderbuffer == NULL)
		return false;
	// a big file takes one long decode however it is scheduled (tens of
	// milliseconds on a desktop, several times that on a phone): that stays
	// in the load rather than becoming a hitch in the game
	size = TouchSnd_File(sfx->name, path, sizeof(path));
	if (size < 0)
		return false;
	{
		// By decoded length where it can be told: at 32 kbit/s and up, a file
		// of maxlength * 4000 bytes or less cannot be longer than maxlength.
		double seconds = -1;
		big = size > snd_lazyload_maxsize.value * 1024;
		if (snd_lazyload_maxlength.value > 0 && TouchSnd_IsOgg(path))
		{
			if (size <= snd_lazyload_maxlength.value * 4000)
				big = false;
			else if ((seconds = TouchSnd_OggSeconds(path, size)) >= 0)
				big = seconds > snd_lazyload_maxlength.value;
		}
	}
	if (big)
	{
		// decoded on a worker thread while the load goes on
		if (!TouchSnd_IsOgg(path) || !TouchOgg_StartAsync(sfx, path))
			return false;
	}
	sfx->flags |= TOUCH_SFXFLAG_DEFERRED;
	return true;
}

static qbool TouchSnd_Deferred(const sfx_t *sfx)
{
	return sfx && (sfx->flags & TOUCH_SFXFLAG_DEFERRED);
}

static void TouchSnd_Ensure(sfx_t *sfx)
{
	if (sfx && (sfx->flags & TOUCH_SFXFLAG_PRELOADED))
	{
		sfx->flags &= ~TOUCH_SFXFLAG_PRELOADED;
		TouchPreload_Used('s', sfx->name);
	}
	if (!sfx || !(sfx->flags & TOUCH_SFXFLAG_DEFERRED))
		return;
	sfx->flags &= ~TOUCH_SFXFLAG_DEFERRED;
	if (TouchOgg_Pending(sfx))
		TouchOgg_Finish(sfx);
	if (!sfx->fetcher)
	{
		double t0 = Sys_DirtyTime();
		S_LoadSound(sfx, true);
		TouchPreload_Loaded('s', sfx->name, Sys_DirtyTime() - t0);
	}
}

// touch_preload: decode a precached sound before its first play
qbool TouchSnd_PreloadName(const char *name)
{
	sfx_t *sfx;
	if (snd_renderbuffer == NULL)
		return false;
	for (sfx = known_sfx; sfx; sfx = sfx->next)
	{
		if ((sfx->flags & TOUCH_SFXFLAG_DEFERRED) && !strcmp(sfx->name, name))
		{
			TouchSnd_Ensure(sfx);
			sfx->flags |= TOUCH_SFXFLAG_PRELOADED;
			return true;
		}
	}
	return false;
}

// touch_warm.c: decode a sound while the menu is idle, before the game
// that will precache it; 1 decoded now, 0 already decoded, -1 none
int TouchSnd_WarmName(const char *name)
{
	sfx_t *sfx;
	qbool had;
	if (snd_renderbuffer == NULL || nosound.integer)
		return -1;
	// as a level sound: the next game's S_ClearUsed and purge free it unless
	// that game precaches it (a menu sound would never be freed)
	if (!(sfx = S_PrecacheSound(name, false, true)))
		return -1;
	had = sfx->fetcher != NULL && !(sfx->flags & TOUCH_SFXFLAG_DEFERRED);
	if (sfx->flags & TOUCH_SFXFLAG_DEFERRED)
		TouchSnd_Ensure(sfx);
	if (!sfx->fetcher)
		return -1;
	if (had)
		return 0;
	sfx->flags |= TOUCH_SFXFLAG_PRELOADED;
	return 1;
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
		if (!(sfx->flags & TOUCH_SFXFLAG_DEFERRED) || TouchOgg_Pending(sfx))
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
	Cvar_RegisterVariable(&snd_lazyload_maxlength);
	touch_snd_loadsome = TouchSnd_LoadSome;
	touch_snd_poll = TouchOgg_Poll;
	TouchOgg_Init();
}
