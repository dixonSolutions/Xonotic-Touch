// Touch: Ogg Opus sound for the phone packs (docs/PHONE_ASSETS.md).
//
// scripts/repack-phone-assets.py --audio opus re-encodes sound/foo.ogg as
// Ogg Opus and keeps the name: the QuakeC checks that sound files exist by
// their .ogg or .wav name before it precaches them ("Missing sound"), so a
// foo.opus beside a missing foo.ogg would never be asked for. OGG_LoadVorbisFile
// calls Touch_Opus_LoadFile first; that looks at the first Ogg page and
// decodes the file when it is Opus. A foo.opus is also taken when there is
// no foo.ogg at all (engine-side requests, packs made by other tools).
//
// The decoder is libopusfile: linked in where LINK_TO_LIBOPUSFILE is defined
// (Android, built by scripts/android-deps.sh), loaded at run time elsewhere.
// Short sounds are decoded once into memory like the Vorbis ones; long ones
// (music) are decoded as they play, each channel with its own decoder.
//
// Opus always decodes at 48 kHz, but LOOP_START and LOOP_END count samples of
// the source, and the transcode keeps the source's tags and rate (the
// OpusHead input rate): loop points are scaled by 48000 / that rate.

#include "quakedef.h"
#include "snd_main.h"
#include "snd_wav.h"
#include "touch_assets.h"

// The web build links libopus and libopusfile statically (scripts/build-web.sh
// builds them with emcc): WebAssembly has no dlopen.
#if defined(__EMSCRIPTEN__) && defined(XT_WEB_OPUSFILE) && !defined(LINK_TO_LIBOPUSFILE)
#define LINK_TO_LIBOPUSFILE
#endif

#ifdef LINK_TO_LIBOPUSFILE
#include <opus/opusfile.h>
#define qop_open_memory op_open_memory
#define qop_free op_free
#define qop_channel_count op_channel_count
#define qop_pcm_total op_pcm_total
#define qop_pcm_seek op_pcm_seek
#define qop_read op_read
#define qop_read_stereo op_read_stereo
#define qop_head op_head
#define qop_tags op_tags
#define qopus_tags_query opus_tags_query
typedef OggOpusFile touch_opusfile_t;
static qbool Touch_Opus_Open(void) { return true; }
#else
// the few opusfile calls this needs, with the types they use spelt out
typedef struct OggOpusFile touch_opusfile_t;
typedef struct
{
	int version;
	int channel_count;
	unsigned pre_skip;
	unsigned int input_sample_rate;
	// more fields follow; only the ones above are read
} OpusHead;
typedef struct OpusTags OpusTags;
static touch_opusfile_t *(*qop_open_memory)(const unsigned char *data, size_t size, int *error);
static void (*qop_free)(touch_opusfile_t *of);
static int (*qop_channel_count)(const touch_opusfile_t *of, int li);
static int64_t (*qop_pcm_total)(const touch_opusfile_t *of, int li);
static int (*qop_pcm_seek)(touch_opusfile_t *of, int64_t offset);
static int (*qop_read)(touch_opusfile_t *of, short *pcm, int bufsize, int *li);
static int (*qop_read_stereo)(touch_opusfile_t *of, short *pcm, int bufsize);
static const OpusHead *(*qop_head)(const touch_opusfile_t *of, int li);
static const OpusTags *(*qop_tags)(const touch_opusfile_t *of, int li);
static const char *(*qopus_tags_query)(const OpusTags *tags, const char *tag, int count);

static dllfunction_t opusfilefuncs[] =
{
	{"op_open_memory", (void **) &qop_open_memory},
	{"op_free", (void **) &qop_free},
	{"op_channel_count", (void **) &qop_channel_count},
	{"op_pcm_total", (void **) &qop_pcm_total},
	{"op_pcm_seek", (void **) &qop_pcm_seek},
	{"op_read", (void **) &qop_read},
	{"op_read_stereo", (void **) &qop_read_stereo},
	{"op_head", (void **) &qop_head},
	{"op_tags", (void **) &qop_tags},
	{"opus_tags_query", (void **) &qopus_tags_query},
	{NULL, NULL}
};
static dllhandle_t opusfile_dll = NULL;

static qbool Touch_Opus_Open(void)
{
	static int tried;
	const char *dllnames[] =
	{
#if defined(WIN32)
		"libopusfile-0.dll",
		"opusfile.dll",
#elif defined(MACOSX)
		"libopusfile.0.dylib",
		"libopusfile.dylib",
#else
		"libopusfile.so.0",
		"libopusfile.so",
#endif
		NULL
	};
	if (!tried)
	{
		tried = true;
		// COMMANDLINEOPTION: Sound: -noopus disables Ogg Opus sound (the phone packs' .opus files)
		if (!Sys_CheckParm("-noopus"))
			Sys_LoadDependency(dllnames, &opusfile_dll, opusfilefuncs);
	}
	return opusfile_dll != NULL;
}
#endif

#define OPUS_RATE 48000
#define OPUS_STREAM_FRAMES 4096 // per channel decode buffer, in sample frames

typedef struct
{
	unsigned char *file;
	size_t filesize;
} opus_persfx_t;

typedef struct
{
	touch_opusfile_t *of;
	int buffer_firstframe;
	int buffer_numframes;
	short buffer[OPUS_STREAM_FRAMES * 2];
} opus_perchannel_t;

/// decode up to "frames" sample frames of "nch" channels; returns frames
static int Touch_Opus_Decode(touch_opusfile_t *of, int nch, short *out, int frames)
{
	int done = 0, n;
	while (done < frames)
	{
		if (nch == 2)
			n = qop_read_stereo(of, out + done * 2, (frames - done) * 2);
		else
			n = qop_read(of, out + done, frames - done, NULL);
		if (n <= 0)
			break;
		done += n;
	}
	return done;
}

static void Touch_Opus_GetSamplesFloat(channel_t *ch, sfx_t *sfx, int firstframe, int numframes, float *out)
{
	opus_perchannel_t *pc = (opus_perchannel_t *)ch->fetcher_data;
	opus_persfx_t *ps = (opus_persfx_t *)sfx->fetcher_data;
	int nch = sfx->format.channels;
	int i, n;
	short *src;

	if (!pc)
	{
		int err = 0;
		pc = (opus_perchannel_t *)Mem_Alloc(snd_mempool, sizeof(*pc));
		pc->of = qop_open_memory(ps->file, ps->filesize, &err);
		if (!pc->of)
		{
			Mem_Free(pc);
			return;
		}
		ch->fetcher_data = pc;
	}

	while (numframes > 0)
	{
		// not in the buffer: seek there (a loop, or the channel was silent a while)
		if (firstframe < pc->buffer_firstframe || firstframe >= pc->buffer_firstframe + pc->buffer_numframes)
		{
			if (firstframe != pc->buffer_firstframe + pc->buffer_numframes && qop_pcm_seek(pc->of, firstframe) != 0)
				return;
			pc->buffer_firstframe = firstframe;
			pc->buffer_numframes = Touch_Opus_Decode(pc->of, nch, pc->buffer, OPUS_STREAM_FRAMES);
			if (pc->buffer_numframes <= 0)
			{
				// past the end: silence
				memset(out, 0, numframes * nch * sizeof(*out));
				return;
			}
		}
		n = min(numframes, pc->buffer_firstframe + pc->buffer_numframes - firstframe);
		src = pc->buffer + (firstframe - pc->buffer_firstframe) * nch;
		for (i = 0; i < n * nch; i++)
			out[i] = src[i] * (1.0f / 32768.0f);
		out += n * nch;
		firstframe += n;
		numframes -= n;
	}
}

static void Touch_Opus_StopChannel(channel_t *ch)
{
	opus_perchannel_t *pc = (opus_perchannel_t *)ch->fetcher_data;
	if (pc)
	{
		qop_free(pc->of);
		Mem_Free(pc);
	}
}

static void Touch_Opus_FreeSfx(sfx_t *sfx)
{
	opus_persfx_t *ps = (opus_persfx_t *)sfx->fetcher_data;
	Mem_Free(ps->file);
	Mem_Free(ps);
}

static const snd_fetcher_t touch_opus_fetcher = {Touch_Opus_GetSamplesFloat, Touch_Opus_StopChannel, Touch_Opus_FreeSfx};

static double Touch_Opus_Tag(const OpusTags *tags, const char *name, double fallback)
{
	const char *v = tags ? qopus_tags_query(tags, name, 0) : NULL;
	return v ? atof(v) : fallback;
}

/// ReplayGain tags, as OGG_LoadVorbisFile applies them
static void Touch_Opus_Gain(const OpusTags *tags, sfx_t *sfx)
{
	double peak = Touch_Opus_Tag(tags, "REPLAYGAIN_TRACK_PEAK", 0);
	double gaindb = Touch_Opus_Tag(tags, "REPLAYGAIN_TRACK_GAIN", 0);
	if (peak)
	{
		sfx->volume_mult = min(1.0f / peak, exp(gaindb * 0.05f * log(10.0f)));
		sfx->volume_peak = peak;
	}
	else if (gaindb != 0)
	{
		sfx->volume_mult = exp(gaindb * 0.05f * log(10.0f));
		sfx->volume_peak = 1.0;
	}
}

/// whether the first Ogg page of "filename" starts an Opus stream
static qbool Touch_Opus_Sniff(const char *filename)
{
	unsigned char head[64];
	fs_offset_t n, i;
	qfile_t *f = FS_OpenVirtualFile(filename, true);
	if (!f)
		return false;
	n = FS_Read(f, head, sizeof(head));
	FS_Close(f);
	if (n < 36 || memcmp(head, "OggS", 4))
		return false;
	// the page header is 27 bytes plus one byte per segment
	i = 27 + head[26];
	return i + 8 <= n && !memcmp(head + i, "OpusHead", 8);
}

qbool Touch_Opus_LoadFile(const char *filename, sfx_t *sfx)
{
	char opusname[MAX_QPATH + 16];
	size_t len = strlen(filename);
	unsigned char *data;
	fs_offset_t filesize;
	touch_opusfile_t *of;
	const OpusHead *head;
	const OpusTags *tags;
	int err = 0, nch;
	int64_t total;
	double scale, loopstart, loopend;

	if (sfx->fetcher != NULL)
		return true;
	if (len < 4 || len >= sizeof(opusname) - 2 || strcasecmp(filename + len - 4, ".ogg"))
		return false;
	if (FS_FileExists(filename))
	{
		// an Ogg file that holds Opus rather than Vorbis
		if (!Touch_Opus_Sniff(filename))
			return false;
		dp_strlcpy(opusname, filename, sizeof(opusname));
	}
	else
	{
		memcpy(opusname, filename, len - 4);
		memcpy(opusname + len - 4, ".opus", 6);
		if (!FS_FileExists(opusname))
			return false;
	}
	if (!Touch_Opus_Open())
	{
		Con_DPrintf("%s: no Opus decoder (libopusfile)\n", opusname);
		return false;
	}

	data = FS_LoadFile(opusname, snd_mempool, false, &filesize);
	if (!data)
		return false;
	of = qop_open_memory(data, (size_t)filesize, &err);
	if (!of)
	{
		Con_Printf("error %d while opening Ogg Opus file \"%s\"\n", err, opusname);
		Mem_Free(data);
		return false;
	}
	if (developer_loading.integer >= 2)
		Con_Printf("Loading Ogg Opus file \"%s\"\n", opusname);

	nch = qop_channel_count(of, -1) >= 2 ? 2 : 1;
	total = qop_pcm_total(of, -1);
	head = qop_head(of, -1);
	tags = qop_tags(of, -1);
	if (total <= 0 || total > INT_MAX / 4)
	{
		Con_Printf("%s: unsupported length\n", opusname);
		qop_free(of);
		Mem_Free(data);
		return false;
	}

	sfx->format.speed = OPUS_RATE;
	sfx->format.channels = nch;
	sfx->format.width = 2;
	sfx->total_length = (unsigned int)total;

	// LOOP_START / LOOP_END / LOOP_LENGTH count source samples
	scale = (head && head->input_sample_rate) ? (double)OPUS_RATE / head->input_sample_rate : 1.0;
	loopstart = Touch_Opus_Tag(tags, "LOOP_START", -1);
	if (loopstart >= 0)
	{
		loopend = Touch_Opus_Tag(tags, "LOOP_END", -1);
		if (loopend < 0 && Touch_Opus_Tag(tags, "LOOP_LENGTH", -1) >= 0)
			loopend = loopstart + Touch_Opus_Tag(tags, "LOOP_LENGTH", 0);
		sfx->loopstart = (unsigned int)bound(0, loopstart * scale, (double)total);
		if (loopend >= 0)
			sfx->total_length = (unsigned int)bound(0, loopend * scale, (double)total);
	}
	else
		sfx->loopstart = sfx->total_length;

	if (snd_streaming.integer && (snd_streaming.integer >= 2 || sfx->total_length > max(sizeof(opus_perchannel_t), snd_streaming_length.value * OPUS_RATE)))
	{
		opus_persfx_t *ps = (opus_persfx_t *)Mem_Alloc(snd_mempool, sizeof(*ps));
		ps->file = data;
		ps->filesize = (size_t)filesize;
		sfx->memsize += sizeof(*ps) + filesize;
		sfx->fetcher_data = ps;
		sfx->fetcher = &touch_opus_fetcher;
		sfx->flags |= SFXFLAG_STREAMED;
	}
	else
	{
		size_t bytes = (size_t)total * nch * 2;
		short *pcm = (short *)Mem_Alloc(snd_mempool, bytes);
		int got = Touch_Opus_Decode(of, nch, pcm, (int)total);
		if (got < total)
			memset(pcm + got * nch, 0, (size_t)(total - got) * nch * 2);
		sfx->flags &= ~SFXFLAG_STREAMED;
		sfx->memsize += bytes;
		sfx->fetcher = &wav_fetcher;
		sfx->fetcher_data = pcm;
		Mem_Free(data);
		data = NULL;
	}

	Touch_Opus_Gain(tags, sfx);
	qop_free(of);
	return true;
}

// ---- decoded on a worker thread (snd_lazyload_async, touch_loadogg.h) ------
//
// The three steps of Touch_Opus_LoadFile for a sound that is decoded whole:
// open on the main thread, decode anywhere (a decoder handle is only ever
// used by one thread), install on the main thread.

void *Touch_Opus_AsyncOpen(const char *filename, unsigned char **filedata, int *channels, int64_t *frames);
void Touch_Opus_AsyncDecode(void *handle, int channels, short *pcm, int64_t frames);
void Touch_Opus_AsyncInstall(void *handle, sfx_t *sfx, short *pcm, int channels, int64_t frames);
void Touch_Opus_AsyncClose(void *handle);

/// NULL when this is not an Opus file to decode whole (not Opus, no decoder,
/// or long enough to be streamed): the caller loads it the normal way.
void *Touch_Opus_AsyncOpen(const char *filename, unsigned char **filedata, int *nch, int64_t *frames)
{
	unsigned char *data;
	fs_offset_t filesize;
	touch_opusfile_t *of;
	int err = 0;
	int64_t total;

	if (!Touch_Opus_Sniff(filename) || !Touch_Opus_Open())
		return NULL;
	data = FS_LoadFile(filename, snd_mempool, true, &filesize);
	if (!data)
		return NULL;
	of = qop_open_memory(data, (size_t)filesize, &err);
	if (!of)
	{
		Mem_Free(data);
		return NULL;
	}
	total = qop_pcm_total(of, -1);
	// (a loop tag can only shorten it: what is not streamed whole is not streamed looped)
	if (total <= 0 || total > INT_MAX / 4
	 || (snd_streaming.integer && (snd_streaming.integer >= 2 || total > (int64_t)max(sizeof(opus_perchannel_t), snd_streaming_length.value * OPUS_RATE))))
	{
		qop_free(of);
		Mem_Free(data);
		return NULL;
	}
	*filedata = data;
	*nch = qop_channel_count(of, -1) >= 2 ? 2 : 1;
	*frames = total;
	return of;
}

void Touch_Opus_AsyncDecode(void *handle, int nch, short *pcm, int64_t frames)
{
	int got = Touch_Opus_Decode((touch_opusfile_t *)handle, nch, pcm, (int)frames);
	if (got < frames)
		memset(pcm + got * nch, 0, (size_t)(frames - got) * nch * 2);
}

void Touch_Opus_AsyncInstall(void *handle, sfx_t *sfx, short *pcm, int nch, int64_t frames)
{
	touch_opusfile_t *of = (touch_opusfile_t *)handle;
	const OpusHead *head = qop_head(of, -1);
	const OpusTags *tags = qop_tags(of, -1);
	double scale, loopstart, loopend;

	sfx->format.speed = OPUS_RATE;
	sfx->format.channels = nch;
	sfx->format.width = 2;
	sfx->total_length = (unsigned int)frames;
	scale = (head && head->input_sample_rate) ? (double)OPUS_RATE / head->input_sample_rate : 1.0;
	loopstart = Touch_Opus_Tag(tags, "LOOP_START", -1);
	if (loopstart >= 0)
	{
		loopend = Touch_Opus_Tag(tags, "LOOP_END", -1);
		if (loopend < 0 && Touch_Opus_Tag(tags, "LOOP_LENGTH", -1) >= 0)
			loopend = loopstart + Touch_Opus_Tag(tags, "LOOP_LENGTH", 0);
		sfx->loopstart = (unsigned int)bound(0, loopstart * scale, (double)frames);
		if (loopend >= 0)
			sfx->total_length = (unsigned int)bound(0, loopend * scale, (double)frames);
	}
	else
		sfx->loopstart = sfx->total_length;
	sfx->flags &= ~SFXFLAG_STREAMED;
	sfx->memsize += (size_t)frames * nch * 2;
	sfx->fetcher = &wav_fetcher;
	sfx->fetcher_data = pcm;
	Touch_Opus_Gain(tags, sfx);
}

void Touch_Opus_AsyncClose(void *handle)
{
	qop_free((touch_opusfile_t *)handle);
}
