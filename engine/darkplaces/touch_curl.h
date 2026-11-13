// Touch: one large HTTP download fetched as several ranges at once
// (docs/NETWORK.md). Included by libcurl.c, whose download list and curl
// function pointers it uses; the hooks there are single "// Touch:" lines.
//
// A map pack is one file of tens of MB. A single HTTP connection over a link
// with latency and some loss, or from a server that limits each connection,
// moves far less than the link can. Once the server has answered the stock
// download (its size is then known) and more than two curl_segment_size
// pieces are left, the rest is split: the stock transfer keeps the first
// piece, up to curl_segments - 1 more connections fetch the others with
// Range requests into <file>.partN, and when every piece is in, the parts are
// appended to the file in order and the stock code finishes the download as
// usual (FS_AddPack). The stock transfer is only cut short once every extra
// connection has answered 206 Partial Content: a server without ranges, an
// error or a redirect to something else leaves the stock download alone.
// A failed piece is asked for once more from where it stopped; if that fails
// too, the download fails as a stock one would. A download that is cancelled
// or interrupted drops its parts; the next attempt resumes the main file
// (stock CURLOPT_RESUME_FROM). Downloads with a speed limit (curl_maxspeed,
// sv_curl_maxspeed) are never split, so the limit holds.

static cvar_t curl_segments = {CF_CLIENT | CF_ARCHIVE, "curl_segments", "4", "connections one large HTTP download may use at once (Godnotic, docs/NETWORK.md); 1 = one, as stock"};
static cvar_t curl_segment_size = {CF_CLIENT | CF_ARCHIVE, "curl_segment_size", "2097152", "smallest piece in bytes worth its own connection when a download is split"};

#define TC_MAXSEG 7

struct tc_dl_s;

typedef struct tc_seg_s
{
	CURL *curle;
	qfile_t *file;
	char path[MAX_OSPATH];
	fs_offset_t from, to;   // bytes [from, to) of the file
	fs_offset_t have;
	int state;              // 0 running, 1 complete, 2 failed for good
	int tries;
	qbool bad;              // the server did not answer 206
	struct tc_dl_s *dl;
}
tc_seg_t;

typedef struct tc_dl_s
{
	struct tc_dl_s *next;
	downloadinfo *di;
	int nseg;               // 0: not split (checked, or given up)
	tc_seg_t seg[TC_MAXSEG];
	fs_offset_t total;      // file size
	fs_offset_t limit;      // the stock transfer stops here once confirmed
	qbool confirmed;
	qbool maindone;
	int result;             // 0 pending, 1 complete, 2 failed
	qbool reported;
	double seen;            // when the server's answer arrived
	qbool decided;
}
tc_dl_t;

static tc_dl_t *tc_list;
static int touchcurl_ok;

static tc_dl_t *TC_Find(downloadinfo *di)
{
	tc_dl_t *d;
	for (d = tc_list; d; d = d->next)
		if (d->di == di)
			return d;
	return NULL;
}

static void TC_DropSeg(tc_seg_t *s)
{
	if (s->curle)
	{
		qcurl_multi_remove_handle(curlm, s->curle);
		qcurl_easy_cleanup(s->curle);
		s->curle = NULL;
	}
	if (s->file)
	{
		FS_RemoveOnClose(s->file);
		FS_Close(s->file);
		s->file = NULL;
	}
}

// give up splitting: the stock transfer carries on alone
static void TC_Abandon(tc_dl_t *d)
{
	int i;
	for (i = 0; i < d->nseg; i++)
		TC_DropSeg(&d->seg[i]);
	d->nseg = 0;
	d->limit = 0;
	d->confirmed = false;
}

static size_t TC_Write(void *data, size_t size, size_t nmemb, void *vseg)
{
	tc_seg_t *s = (tc_seg_t *)vseg;
	size_t bytes = size * nmemb;
	long code = 0;
	qcurl_easy_getinfo(s->curle, CURLINFO_RESPONSE_CODE, &code);
	if (code != 206)
	{
		// the whole file, or an error page: not this piece
		s->bad = true;
		return 0;
	}
	if (s->from + s->have + (fs_offset_t)bytes > s->to)
		bytes = (size_t)(s->to - s->from - s->have);
	if (bytes && FS_Write(s->file, data, bytes) != (fs_offset_t)bytes)
		return 0;
	s->have += bytes;
	return bytes;
}

static qbool TC_StartSeg(tc_dl_t *d, tc_seg_t *s, const char *url)
{
	char range[64];
	const char *ua = engineversion;
	s->curle = qcurl_easy_init();
	if (!s->curle)
		return false;
	dpsnprintf(range, sizeof(range), "%lld-%lld", (long long)(s->from + s->have), (long long)(s->to - 1));
	qcurl_easy_setopt(s->curle, CURLOPT_URL, url);
	qcurl_easy_setopt(s->curle, (CURLoption)(CURLOPTTYPE_OBJECTPOINT + 7), range); // CURLOPT_RANGE
	if (curl_useragent.integer)
		qcurl_easy_setopt(s->curle, CURLOPT_USERAGENT, ua ? ua : "");
	qcurl_easy_setopt(s->curle, CURLOPT_REFERER, d->di->referer);
	qcurl_easy_setopt(s->curle, CURLOPT_FOLLOWLOCATION, 1);
	qcurl_easy_setopt(s->curle, CURLOPT_WRITEFUNCTION, TC_Write);
	qcurl_easy_setopt(s->curle, CURLOPT_WRITEDATA, (void *)s);
	qcurl_easy_setopt(s->curle, CURLOPT_PRIVATE, (void *)s);
	qcurl_easy_setopt(s->curle, CURLOPT_LOW_SPEED_LIMIT, (long)256);
	qcurl_easy_setopt(s->curle, CURLOPT_LOW_SPEED_TIME, (long)45);
	qcurl_easy_setopt(s->curle, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
	qcurl_easy_setopt(s->curle, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#ifdef __ANDROID__
	if (curl_android_capath)
		qcurl_easy_setopt(s->curle, CURLOPT_CAPATH, curl_android_capath);
#endif
	qcurl_multi_add_handle(curlm, s->curle);
	return true;
}

// a download the server answered: note it, and half a second later split
// what is left if it would still take more than three seconds
static void TC_Split(downloadinfo *di, tc_dl_t *d)
{
	double length = -1, speed = 0;
	long code = 0;
	const char *url = NULL;
	fs_offset_t pos, remaining;
	int n, i;

	qcurl_easy_getinfo(di->curle, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &length);
	qcurl_easy_getinfo(di->curle, CURLINFO_RESPONSE_CODE, &code);
	if (!d)
	{
		if (length <= 0 || !code)
			return; // no answer yet: ask again next frame
		d = (tc_dl_t *)Z_Malloc(sizeof(*d));
		d->di = di;
		d->seen = host.realtime;
		d->next = tc_list;
		tc_list = d;
		return;
	}
	if (host.realtime - d->seen < 0.5)
		return;
	d->decided = true;
	if (code != 200 && code != 206)
		return;
	if (qcurl_easy_getinfo(di->curle, CURLINFO_EFFECTIVE_URL, &url) != CURLE_OK || !url || strncmp(url, "http", 4))
		return;
	d->total = di->startpos + (fs_offset_t)length;
	pos = di->startpos + (fs_offset_t)di->bytes_received;
	remaining = d->total - pos;
	qcurl_easy_getinfo(di->curle, CURLINFO_SPEED_DOWNLOAD, &speed);
	if (speed > 0 && remaining / speed < 3)
		return; // nearly there: another connection would not pay for itself
	n = (int)min(remaining / max(curl_segment_size.integer, 65536), (fs_offset_t)curl_segments.integer);
	n = min(n, TC_MAXSEG + 1);
	if (n < 2)
		return;
	// the stock transfer keeps [pos, B1); piece i takes [B(i+1), B(i+2))
	d->nseg = n - 1;
	for (i = 0; i < d->nseg; i++)
	{
		tc_seg_t *s = &d->seg[i];
		s->dl = d;
		s->from = pos + remaining * (i + 1) / n;
		s->to = i == d->nseg - 1 ? d->total : pos + remaining * (i + 2) / n;
		dpsnprintf(s->path, sizeof(s->path), "%s.part%d", di->filename, i + 1);
		s->file = FS_OpenRealFile(s->path, "wb", false);
		if (!s->file || !TC_StartSeg(d, s, url))
		{
			TC_Abandon(d);
			return;
		}
	}
	Con_DPrintf("%s: %d connections for %lld bytes\n", di->filename, n, (long long)remaining);
}

static void TC_CheckComplete(tc_dl_t *d)
{
	int i;
	unsigned char buf[65536];
	fs_offset_t got;
	qfile_t *f;
	if (d->result || !d->maindone)
		return;
	for (i = 0; i < d->nseg; i++)
	{
		if (d->seg[i].state == 2)
		{
			d->result = 2;
			return;
		}
		if (d->seg[i].state != 1)
			return;
	}
	// every piece is in: append them in order
	for (i = 0; i < d->nseg; i++)
	{
		tc_seg_t *s = &d->seg[i];
		if (s->file)
		{
			FS_Close(s->file);
			s->file = NULL;
		}
		f = FS_OpenRealFile(s->path, "rb", false);
		if (!f)
		{
			d->result = 2;
			return;
		}
		while ((got = FS_Read(f, buf, sizeof(buf))) > 0)
			FS_Write(d->di->stream, buf, got);
		FS_RemoveOnClose(f);
		FS_Close(f);
	}
	d->result = FS_Tell(d->di->stream) == d->total ? 1 : 2;
	if (d->result == 2)
		Con_Printf("%s: pieces do not add up to %lld bytes\n", d->di->filename, (long long)d->total);
}

// libcurl.c Curl_Frame, after curl ran: split new downloads, confirm splits
static void TouchCurl_Frame(void)
{
	downloadinfo *di;
	tc_dl_t *d;
	int i;
	long code;
	if (curl_segments.integer < 2 || curl_maxspeed.value > 0)
		return;
	List_For_Each_Entry(di, &downloads, downloadinfo, list)
		if (di->started && di->curle && di->stream && !di->buffer && !di->post_content_type && di->maxspeed <= 0 && (!(d = TC_Find(di)) || !d->decided))
			TC_Split(di, d);
	for (d = tc_list; d; d = d->next)
	{
		if (!d->nseg || d->confirmed)
			continue;
		for (i = 0; i < d->nseg; i++)
		{
			code = 0;
			qcurl_easy_getinfo(d->seg[i].curle, CURLINFO_RESPONSE_CODE, &code);
			if (d->seg[i].bad || (code && code != 206) || d->seg[i].state == 2)
			{
				TC_Abandon(d);
				break;
			}
			if (!code)
				break;
		}
		if (d->nseg && i == d->nseg)
		{
			// every piece is coming: the stock transfer stops at the first
			if (d->di->startpos + (fs_offset_t)d->di->bytes_received >= d->seg[0].from)
				TC_Abandon(d); // it got there first
			else
			{
				d->limit = d->seg[0].from;
				d->confirmed = true;
			}
		}
	}
}

// CURL_fwrite: the stock transfer writes no further than its piece
static size_t TouchCurl_Clip(downloadinfo *di, size_t bytes)
{
	tc_dl_t *d;
	fs_offset_t pos;
	if (!tc_list || !di->stream || !(d = TC_Find(di)) || !d->confirmed)
		return bytes;
	pos = di->startpos + (fs_offset_t)di->bytes_received;
	if (pos + (fs_offset_t)bytes > d->limit)
		bytes = pos < d->limit ? (size_t)(d->limit - pos) : 0;
	return bytes;
}

// Curl_Frame, a finished transfer: true when it was one of ours
static qbool TouchCurl_Done(CURL *curle, CURLcode result)
{
	tc_dl_t *d;
	int i;
	for (d = tc_list; d; d = d->next)
	{
		if (d->confirmed && d->di->curle == curle)
		{
			if (d->di->startpos + (fs_offset_t)d->di->bytes_received != d->limit)
				return false; // a real failure before its piece was done: stock handling
			// its piece is done (curl stopped it with a write error); the
			// handle stays for the progress line until Curl_EndDownload
			qcurl_multi_remove_handle(curlm, curle);
			d->maindone = true;
			TC_CheckComplete(d);
			return true;
		}
		for (i = 0; i < d->nseg; i++)
		{
			tc_seg_t *s = &d->seg[i];
			if (s->curle != curle)
				continue;
			qcurl_multi_remove_handle(curlm, curle);
			qcurl_easy_cleanup(curle);
			s->curle = NULL;
			if (s->from + s->have == s->to)
				s->state = 1;
			else if (d->confirmed && !s->bad && ++s->tries < 2 && TC_StartSeg(d, s, d->di->url))
				Con_DPrintf("%s: piece %d again from %lld (%d)\n", d->di->filename, i + 1, (long long)(s->from + s->have), (int)result);
			else
				s->state = 2;
			TC_CheckComplete(d);
			return true;
		}
	}
	return false;
}

// Curl_Frame: a split download that is complete or failed, for Curl_EndDownload
static downloadinfo *TouchCurl_Finished(void)
{
	tc_dl_t *d;
	for (d = tc_list; d; d = d->next)
	{
		if (d->result && !d->reported)
		{
			d->reported = true;
			touchcurl_ok = d->result == 1;
			return d->di;
		}
	}
	return NULL;
}

// Curl_EndDownload: the pieces go with the download
static void TouchCurl_End(downloadinfo *di)
{
	tc_dl_t **p, *d;
	for (p = &tc_list; (d = *p); p = &d->next)
	{
		if (d->di == di)
		{
			TC_Abandon(d);
			*p = d->next;
			Z_Free(d);
			return;
		}
	}
}

// Curl_GetDownloadInfo: progress and speed over all pieces
static void TouchCurl_Info(downloadinfo *di, double *progress, double *speed, int index)
{
	tc_dl_t *d = tc_list ? TC_Find(di) : NULL;
	double have, v;
	int i;
	if (!d || !d->nseg || d->total <= 0)
	{
		TouchNet_CurlProgress(index, (double)di->startpos + di->bytes_received);
		return;
	}
	have = (double)di->startpos + di->bytes_received;
	if (d->maindone)
		*speed = 0; // the stock transfer is through its piece
	for (i = 0; i < d->nseg; i++)
	{
		have += d->seg[i].have;
		v = 0;
		if (d->seg[i].curle)
			qcurl_easy_getinfo(d->seg[i].curle, CURLINFO_SPEED_DOWNLOAD, &v);
		*speed += v;
	}
	*progress = have / (double)d->total;
	TouchNet_CurlProgress(index, have);
}

void TouchCurl_Init(void)
{
	Cvar_RegisterVariable(&curl_segments);
	Cvar_RegisterVariable(&curl_segment_size);
}
