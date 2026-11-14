// Godnotic: Vorbis sounds decoded on worker threads (snd_lazyload_async).
// Included by snd_ogg.c, before OGG_LoadVorbisFile, for its Vorbis helpers.
//
// snd_lazyload leaves big sound files to be decoded at precache, since one
// decode of a 500 KB file takes ~80 ms on a desktop and cannot be split
// across frames. With snd_lazyload_async 1 such a file is read and its
// headers parsed on the main thread, the PCM buffer allocated there, and the
// decode itself runs on up to snd_lazyload_threads worker threads while the
// load goes on. The main thread installs a finished sound exactly as
// OGG_LoadVorbisFile would (wav fetcher, loop tags, ReplayGain) at the next
// host frame, or waits for it if the sound is played first. Workers exit
// when the queue is empty: nothing spins while idle. Builds without threads
// (the web build) decode at precache as before. Opus files take the same road
// (touch_assets_opus.c opens, decodes and installs them); files that would be
// streamed and files that fail to open go the normal way.

#ifndef TOUCH_SFXFLAG_DEFERRED
#define TOUCH_SFXFLAG_DEFERRED (1u << 28) // as in touch_loadtime.h
#endif
void TouchOgg_Init(void);
qbool TouchOgg_StartAsync(sfx_t *sfx, const char *filename);
qbool TouchOgg_Pending(const sfx_t *sfx);
void TouchOgg_Finish(sfx_t *sfx);
void TouchOgg_Forget(sfx_t *sfx);
void TouchOgg_Poll(void);

static cvar_t snd_lazyload_async = {CF_CLIENT, "snd_lazyload_async", "1", "decode big precached sounds (beyond snd_lazyload_maxsize) on worker threads during the load (Godnotic)"};
static cvar_t snd_lazyload_threads = {CF_CLIENT, "snd_lazyload_threads", "3", "worker threads for snd_lazyload_async"};

#define TOUCHOGG_MAXTHREADS 8

typedef struct touchogg_job_s
{
	struct touchogg_job_s *next;     // all jobs, in start order
	struct touchogg_job_s *qnext;    // jobs not yet taken by a worker
	sfx_t *sfx;                      // NULL once the sfx was freed
	unsigned char *data;
	ov_decode_t ov_decode;
	OggVorbis_File vf;
	char *pcm;
	int64_t len;
	int rate, channels;
	int64_t total;
	void *opus;                      // an Opus file: its decoder (vf and ov_decode unused)
	int done;                        // set by the worker, under the mutex
}
touchogg_job_t;

// touch_assets_opus.c
void *Touch_Opus_AsyncOpen(const char *filename, unsigned char **filedata, int *channels, int64_t *frames);
void Touch_Opus_AsyncDecode(void *handle, int channels, short *pcm, int64_t frames);
void Touch_Opus_AsyncInstall(void *handle, sfx_t *sfx, short *pcm, int channels, int64_t frames);
void Touch_Opus_AsyncClose(void *handle);

static void *touchogg_mutex;
static touchogg_job_t *touchogg_jobs, *touchogg_queue, *touchogg_queuetail;
static void *touchogg_thread[TOUCHOGG_MAXTHREADS];
static int touchogg_thread_exited[TOUCHOGG_MAXTHREADS];

void TouchOgg_Init(void)
{
	Cvar_RegisterVariable(&snd_lazyload_async);
	Cvar_RegisterVariable(&snd_lazyload_threads);
}

static int TouchOgg_Worker(void *p)
{
	int slot = (int)(size_t)p;
	for (;;)
	{
		touchogg_job_t *job;
		int64_t done = 0;
		long ret;
		int bs = 0;
		Thread_LockMutex(touchogg_mutex);
		job = touchogg_queue;
		if (job)
		{
			touchogg_queue = job->qnext;
			if (!touchogg_queue)
				touchogg_queuetail = NULL;
		}
		else
			touchogg_thread_exited[slot] = 1;
		Thread_UnlockMutex(touchogg_mutex);
		if (!job)
			return 0;
		if (job->opus)
			Touch_Opus_AsyncDecode(job->opus, job->channels, (short *)job->pcm, job->total);
		else
		while (done < job->len && (ret = qov_read(&job->vf, job->pcm + done, (int)(job->len - done), mem_bigendian, 2, 1, &bs)) > 0)
			done += ret;
		Thread_LockMutex(touchogg_mutex);
		job->done = 1;
		Thread_UnlockMutex(touchogg_mutex);
	}
}

// main thread: join workers that finished, start ones the queue needs
static void TouchOgg_Workers(void)
{
	int i, want = bound(1, snd_lazyload_threads.integer, TOUCHOGG_MAXTHREADS);
	qbool queued;
	for (i = 0; i < TOUCHOGG_MAXTHREADS; i++)
	{
		int exited;
		if (!touchogg_thread[i])
			continue;
		Thread_LockMutex(touchogg_mutex);
		exited = touchogg_thread_exited[i];
		Thread_UnlockMutex(touchogg_mutex);
		if (exited)
		{
			Thread_WaitThread(touchogg_thread[i], 0);
			touchogg_thread[i] = NULL;
		}
	}
	Thread_LockMutex(touchogg_mutex);
	queued = touchogg_queue != NULL;
	Thread_UnlockMutex(touchogg_mutex);
	for (i = 0; queued && i < want; i++)
	{
		if (touchogg_thread[i])
			continue;
		touchogg_thread_exited[i] = 0;
		touchogg_thread[i] = Thread_CreateThread(TouchOgg_Worker, (void *)(size_t)i);
	}
}

// Start decoding a Vorbis file for sfx; false when it is not one this can do
// (the caller then loads it the normal way).
qbool TouchOgg_StartAsync(sfx_t *sfx, const char *filename)
{
	touchogg_job_t *job;
	vorbis_info *vi;
	fs_offset_t filesize;
	if (!snd_lazyload_async.integer || !Thread_HasThreads())
		return false;
	if (!touchogg_mutex && !(touchogg_mutex = Thread_CreateMutex()))
		return false;
	job = (touchogg_job_t *)Z_Malloc(sizeof(*job));
	job->opus = Touch_Opus_AsyncOpen(filename, &job->data, &job->channels, &job->total);
	if (job->opus)
	{
		job->rate = 48000;
		goto queue;
	}
#ifndef LINK_TO_LIBVORBIS
	if (!vf_dll)
	{
		Z_Free(job);
		return false;
	}
#endif
	job->data = FS_LoadFile(filename, snd_mempool, true, &filesize);
	if (!job->data)
	{
		Z_Free(job);
		return false;
	}
	job->ov_decode.buffer = job->data;
	job->ov_decode.ind = 0;
	job->ov_decode.buffsize = filesize;
	if (qov_open_callbacks(&job->ov_decode, &job->vf, NULL, 0, callbacks) < 0)
	{
		Mem_Free(job->data);
		Z_Free(job);
		return false;
	}
	vi = qov_info(&job->vf, -1);
	job->total = qov_pcm_total(&job->vf, -1);
	if (vi->channels < 1 || vi->channels > 2 || job->total <= 0
	 || (snd_streaming.integer && (snd_streaming.integer >= 2 || job->total > (int64_t)max(sizeof(ogg_stream_perchannel_t), snd_streaming_length.value * vi->rate))))
	{
		qov_clear(&job->vf);
		Mem_Free(job->data);
		Z_Free(job);
		return false;
	}
	job->rate = vi->rate;
	job->channels = vi->channels;
queue:
	job->len = job->total * job->channels * 2;
	job->pcm = (char *)Mem_Alloc(snd_mempool, (size_t)job->len);
	job->sfx = sfx;
	job->next = touchogg_jobs;
	touchogg_jobs = job;
	Thread_LockMutex(touchogg_mutex);
	if (touchogg_queuetail)
		touchogg_queuetail->qnext = job;
	else
		touchogg_queue = job;
	touchogg_queuetail = job;
	Thread_UnlockMutex(touchogg_mutex);
	TouchOgg_Workers();
	Con_DPrintf("snd_lazyload_async: %s (%s) decodes on a worker\n", filename, job->opus ? "Opus" : "Vorbis");
	return true;
}

static touchogg_job_t **TouchOgg_FindLink(const sfx_t *sfx)
{
	touchogg_job_t **link;
	for (link = &touchogg_jobs; *link; link = &(*link)->next)
		if ((*link)->sfx == sfx)
			return link;
	return NULL;
}

qbool TouchOgg_Pending(const sfx_t *sfx)
{
	return sfx && touchogg_jobs && TouchOgg_FindLink(sfx) != NULL;
}

static qbool TouchOgg_Done(touchogg_job_t *job)
{
	int done;
	Thread_LockMutex(touchogg_mutex);
	done = job->done;
	Thread_UnlockMutex(touchogg_mutex);
	return done != 0;
}

// unlink a finished job and install its sound, as OGG_LoadVorbisFile does
static void TouchOgg_Install(touchogg_job_t **link)
{
	touchogg_job_t *job = *link;
	sfx_t *sfx = job->sfx;
	*link = job->next;
	if (job->opus)
	{
		if (sfx && !sfx->fetcher)
			Touch_Opus_AsyncInstall(job->opus, sfx, (short *)job->pcm, job->channels, job->total);
		else
			Mem_Free(job->pcm);
		Touch_Opus_AsyncClose(job->opus);
		Mem_Free(job->data);
		Z_Free(job);
		return;
	}
	if (sfx && !sfx->fetcher)
	{
		vorbis_comment *vc;
		double peak, gaindb;
		sfx->volume_peak = 0.0;
		sfx->format.speed = job->rate;
		sfx->format.channels = job->channels;
		sfx->format.width = 2;
		sfx->total_length = (unsigned int)job->total;
		sfx->flags &= ~SFXFLAG_STREAMED;
		sfx->memsize += job->len;
		sfx->fetcher_data = job->pcm;
		sfx->fetcher = &wav_fetcher;
		vc = qov_comment(&job->vf, -1);
		OGG_DecodeTags(vc, &sfx->loopstart, &sfx->total_length, sfx->total_length, &peak, &gaindb);
		if (peak)
		{
			sfx->volume_mult = min(1.0f / peak, exp(gaindb * 0.05f * log(10.0f)));
			sfx->volume_peak = peak;
		}
		else if (gaindb != 0)
		{
			sfx->volume_mult = min(1.0f / peak, exp(gaindb * 0.05f * log(10.0f)));
			sfx->volume_peak = 1.0;
		}
	}
	else
		Mem_Free(job->pcm);
	qov_clear(&job->vf);
	Mem_Free(job->data);
	Z_Free(job);
}

// A job no worker has taken yet is decoded here, on the main thread (also
// what happens when no thread could be started); a taken one is waited for.
static void TouchOgg_Complete(touchogg_job_t *job)
{
	touchogg_job_t **q;
	qbool mine = false;
	Thread_LockMutex(touchogg_mutex);
	for (q = &touchogg_queue; *q; q = &(*q)->qnext)
	{
		if (*q == job)
		{
			*q = job->qnext;
			mine = true;
			break;
		}
	}
	if (mine)
	{
		touchogg_job_t *t;
		touchogg_queuetail = NULL;
		for (t = touchogg_queue; t; t = t->qnext)
			touchogg_queuetail = t;
	}
	Thread_UnlockMutex(touchogg_mutex);
	if (mine)
	{
		int64_t done = 0;
		long ret;
		int bs = 0;
		if (job->opus)
			Touch_Opus_AsyncDecode(job->opus, job->channels, (short *)job->pcm, job->total);
		else
		while (done < job->len && (ret = qov_read(&job->vf, job->pcm + done, (int)(job->len - done), mem_bigendian, 2, 1, &bs)) > 0)
			done += ret;
		Thread_LockMutex(touchogg_mutex);
		job->done = 1;
		Thread_UnlockMutex(touchogg_mutex);
		return;
	}
	while (!TouchOgg_Done(job))
		Sys_Sleep(0.0005);
}

// sfx is about to be played: finish its decode and install it now
void TouchOgg_Finish(sfx_t *sfx)
{
	touchogg_job_t **link = TouchOgg_FindLink(sfx);
	if (!link)
		return;
	if (developer_loading.integer)
		Con_Printf("snd_lazyload_async: %s played before its decode finished\n", sfx->name);
	TouchOgg_Complete(*link);
	TouchOgg_Install(link);
}

// sfx is being freed: its job must not outlive it
void TouchOgg_Forget(sfx_t *sfx)
{
	touchogg_job_t **link;
	if (!touchogg_jobs || !(link = TouchOgg_FindLink(sfx)))
		return;
	(*link)->sfx = NULL;
	TouchOgg_Complete(*link);
	TouchOgg_Install(link);
}

// once per host frame: install what is done, keep the workers going
void TouchOgg_Poll(void)
{
	touchogg_job_t **link;
	if (!touchogg_mutex)
		return;
	if (developer_loading.integer && touchogg_jobs)
	{
		static double lastreport;
		int n = 0;
		touchogg_job_t *j;
		for (j = touchogg_jobs; j; j = j->next)
			n++;
		if (Sys_DirtyTime() - lastreport > 0.2)
		{
			lastreport = Sys_DirtyTime();
			Con_Printf("snd_lazyload_async: %d decodes pending\n", n);
		}
	}
	for (link = &touchogg_jobs; *link; )
	{
		if (TouchOgg_Done(*link))
		{
			sfx_t *sfx = (*link)->sfx;
			TouchOgg_Install(link);
			if (sfx)
				sfx->flags &= ~TOUCH_SFXFLAG_DEFERRED;
		}
		else
			link = &(*link)->next;
	}
	TouchOgg_Workers();
}
