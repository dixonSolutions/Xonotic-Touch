/*
Godnotic: a stock server's CSQC from our own HTTP release (touch_csprogs.h,
docs/JOIN_SPEED.md).

The release (`touch_csprogs_url`, published by .github/workflows/csprogs.yml
from the official Xonotic release archives) holds, for each build in
tcp_known below, `csprogs.dat.SIZE.CRC.deflate`: the file deflated raw, as
DarkPlaces deflates downloads. A fetched file counts only when it inflates to
exactly SIZE bytes whose CRC is CRC and whose SHA-256 is the one compiled in
here, so the URL is not trusted with anything: a wrong, truncated or hostile
answer is thrown away and the server is asked, as before.
*/

#include "quakedef.h"
#include "netconn.h"
#include "libcurl.h"
#include "touch_csprogs.h"
#include "touch_jointime.h"
#include "touch_csprogs_logic.h"
#ifdef LINK_TO_ZLIB
#include <zlib.h>
#endif

static cvar_t touch_csprogs = {CF_CLIENT, "touch_csprogs", "1", "fetch a stock server's CSQC (a known Xonotic release build) over HTTP from touch_csprogs_url instead of the server's slow UDP download (Godnotic); 0 = always the server's download"};
static cvar_t touch_csprogs_url = {CF_CLIENT, "touch_csprogs_url", "https://github.com/dixonSolutions/Godnotic/releases/download/csprogs/", "where touch_csprogs finds csprogs.dat.SIZE.CRC.deflate"};
static cvar_t touch_csprogs_timeout = {CF_CLIENT, "touch_csprogs_timeout", "15", "seconds a join waits for the HTTP fetch of the CSQC before asking the server for it"};

// the builds the release holds: raw size, DarkPlaces CRC-16 (what the server
// announces) and the SHA-256 of the raw file (what we accept);
// scripts/csprogs-catalog.py prints these lines
typedef struct tcp_known_s
{
	int size, crc;
	const char *sha256;
	const char *label;
}
tcp_known_t;
static const tcp_known_t tcp_known[] =
{
	{4021257, 18349, "7d7807166d38521aadb8109830b69580596be4c05924d1b1fd8b71c31d4def13", "Xonotic 0.8.6"},
};

enum { TCP_NONE, TCP_RUNNING, TCP_DONE, TCP_FAILED };

#define TCP_MAXFAILED 16
#define TCP_PEEK_BYTES 2048
#define TCP_PEEK_MAPBYTES 8192

static struct
{
	int status;
	int size, crc;
	const tcp_known_t *known;
	unsigned char *buf;
	size_t bufsize;
	double started;
	qbool waiting;     // CL_BeginDownloads is waiting on this fetch
	qbool resume;      // carry on with the load at the next frame
	qbool peek_done;   // this connection's first message was looked at
	qbool peek_map_done;
	char peek_map[MAX_QPATH]; // "maps/NAME.bsp" from the same message
	int failed[TCP_MAXFAILED][2];
	int nfailed;
}
tcp;

/*
==============
The fetch
==============
*/

static const tcp_known_t *TCP_Known(int size, int crc)
{
	size_t i;
	for (i = 0; i < sizeof(tcp_known) / sizeof(tcp_known[0]); i++)
		if (tcp_known[i].size == size && tcp_known[i].crc == crc)
			return &tcp_known[i];
	return NULL;
}

static qbool TCP_Failed(int size, int crc)
{
	int i;
	for (i = 0; i < tcp.nfailed; i++)
		if (tcp.failed[i][0] == size && tcp.failed[i][1] == crc)
			return true;
	return false;
}

static void TCP_Fail(const char *why)
{
	if (tcp.nfailed < TCP_MAXFAILED && !TCP_Failed(tcp.size, tcp.crc))
	{
		tcp.failed[tcp.nfailed][0] = tcp.size;
		tcp.failed[tcp.nfailed][1] = tcp.crc;
		tcp.nfailed++;
	}
	tcp.status = TCP_FAILED;
	Con_Printf("Could not fetch the server's game code (%s) over HTTP: %s; asking the server for it\n", tcp.known ? tcp.known->label : "?", why);
}

// inflate raw deflate data into exactly size bytes; false on anything else
static qbool TCP_Inflate(const unsigned char *in, size_t inlen, unsigned char *out, size_t size)
{
#ifdef LINK_TO_ZLIB
	z_stream s;
	int ret;
	memset(&s, 0, sizeof(s));
	if (inflateInit2(&s, -MAX_WBITS) != Z_OK)
		return false;
	s.next_in = (Bytef *)in;
	s.avail_in = (uInt)inlen;
	s.next_out = out;
	s.avail_out = (uInt)size;
	ret = inflate(&s, Z_FINISH);
	inflateEnd(&s);
	return ret == Z_STREAM_END && s.total_out == size;
#else
	return false;
#endif
}

static void TCP_Done(int status, size_t length, unsigned char *buffer, void *cbdata)
{
	unsigned char *out;
	char sha[65], name[MAX_QPATH];
	if (tcp.status != TCP_RUNNING)
		return;
	if (tcp.waiting)
		tcp.resume = true;
	if (status != CURLCBSTATUS_OK)
	{
		TCP_Fail(status == CURLCBSTATUS_ABORTED ? "too slow" : status > 0 ? va(name, sizeof(name), "HTTP %d", status) : "no answer");
		return;
	}
	out = (unsigned char *)Mem_Alloc(tempmempool, tcp.size);
	if (!TCP_Inflate(buffer, length, out, tcp.size))
	{
		Mem_Free(out);
		TCP_Fail("not a deflated file of the right size");
		return;
	}
	TCPL_Sha256Hex(out, tcp.size, sha);
	if (strcmp(sha, tcp.known->sha256) || CRC_Block(out, tcp.size) != tcp.crc)
	{
		Mem_Free(out);
		TCP_Fail("checksum mismatch");
		return;
	}
	// where the server's download would have put it: the stock code takes
	// it from there (CL_BeginDownloads, CL_VM_Init check size and CRC again)
	dpsnprintf(name, sizeof(name), "dlcache/csprogs.dat.%i.%i", tcp.size, tcp.crc);
	if (!FS_WriteFile(name, out, tcp.size))
	{
		Mem_Free(out);
		TCP_Fail("cannot write dlcache");
		return;
	}
	Mem_Free(out);
	tcp.status = TCP_DONE;
	TouchJoinTime_Mark(TJT_CSPROGS_END);
	TouchJoinTime_CSProgsSource("http", (long long)length);
	Con_Printf("Fetched the server's game code (%s, %u KB) over HTTP in %.1f s\n", tcp.known->label, (unsigned)(length / 1024), Sys_DirtyTime() - tcp.started);
}

// true = a fetch of this build is running
static qbool TCP_Start(int size, int crc)
{
	const tcp_known_t *known;
	char url[MAX_INPUTLINE];
	if (tcp.status == TCP_RUNNING && tcp.size == size && tcp.crc == crc)
		return true;
#ifndef LINK_TO_ZLIB
	return false; // no bounded inflate here (the web build): the server's download
#endif
	if (!touch_csprogs.integer || cls.demoplayback || (strncmp(touch_csprogs_url.string, "http://", 7) && strncmp(touch_csprogs_url.string, "https://", 8)))
		return false;
	if (!(known = TCP_Known(size, crc)) || TCP_Failed(size, crc))
		return false;
	if (FS_FileExists(va(url, sizeof(url), "dlcache/csprogs.dat.%i.%i", size, crc)))
		return false;
	if (tcp.status == TCP_DONE && tcp.size == size && tcp.crc == crc)
		return false; // fetched, but not on disk: the server's download
	if (tcp.status == TCP_RUNNING)
	{
		tcp.status = TCP_NONE; // another build: not a failure of this one
		Curl_Cancel_ToMemory(TCP_Done, NULL);
	}
	if (tcp.buf)
		Mem_Free(tcp.buf);
	// a deflated progs is a third of its size; the raw size bounds it
	tcp.bufsize = size + 4096;
	tcp.buf = (unsigned char *)Mem_Alloc(cls.permanentmempool, tcp.bufsize);
	tcp.size = size;
	tcp.crc = crc;
	tcp.known = known;
	tcp.waiting = tcp.resume = false;
	tcp.started = Sys_DirtyTime();
	tcp.status = TCP_RUNNING;
	dpsnprintf(url, sizeof(url), "%s%scsprogs.dat.%i.%i.deflate", touch_csprogs_url.string,
		touch_csprogs_url.string[strlen(touch_csprogs_url.string) - 1] == '/' ? "" : "/", size, crc);
	if (!Curl_Begin_ToMemory(url, 0, tcp.buf, tcp.bufsize, TCP_Done, NULL))
	{
		TCP_Fail("HTTP is not available");
		return false;
	}
	TouchJoinTime_Mark(TJT_CSPROGS_BEGIN);
	TouchJoinTime_CSProgsSource("http", 0);
	Con_DPrintf("Fetching the server's game code (%s) from %s\n", known->label, url);
	return true;
}

qbool TouchCSProgs_Running(void)
{
	return tcp.status == TCP_RUNNING;
}

extern cvar_t cl_csqc_download;

// the world model: the first "maps/NAME.bsp" string of the precache list
static void TCP_PeekMap(const unsigned char *d, int n)
{
	if (TCPL_FindMap(d, n, tcp.peek_map, sizeof(tcp.peek_map)))
		tcp.peek_map_done = true;
}

const char *TouchCSProgs_PeekMap(void)
{
	return tcp.peek_map[0] && cls.state == ca_connected && cls.signon == 0 ? tcp.peek_map : NULL;
}

void TouchCSProgs_Peek(netconn_t *conn, const unsigned char *data, int length)
{
	int n, size, crc, i;
	if (conn != cls.netcon || cls.state != ca_connected || cls.signon != 0 || sv.active)
		return;
	if (!tcp.peek_map_done)
	{
		TCP_PeekMap(data, min(length, TCP_PEEK_MAPBYTES));
		if (length >= TCP_PEEK_MAPBYTES)
			tcp.peek_map_done = true;
	}
	if (tcp.peek_done || !touch_csprogs.integer || !cl_csqc_download.integer)
		return;
	n = min(length, TCP_PEEK_BYTES);
	size = TCPL_FindInt(data, n, "csqc_progsize ");
	crc = TCPL_FindInt(data, n, "csqc_progcrc ");
	if (size < 0 || crc < 0)
	{
		if (length >= TCP_PEEK_BYTES)
			tcp.peek_done = true; // not in the first message's head: CL_BeginDownloads asks
		return;
	}
	tcp.peek_done = true;
	// only the standard name (the stufftext comes first in the same message)
	for (i = 0; i + 14 < n; i++)
		if (!memcmp(data + i, "csqc_progname ", 14))
		{
			if (n - i - 14 < 12 || memcmp(data + i + 14, "csprogs.dat\n", 12))
				return;
			break;
		}
	if (TCP_Start(size, crc))
		Con_DPrintf("touch_csprogs: fetch started from the server's first fragment\n");
}

qbool TouchCSProgs_Wait(const char *progname, int size, int crc)
{
	if (strcmp(progname, "csprogs.dat") || !cls.netcon || sv.active)
		return false;
	if (!TCP_Start(size, crc))
		return false;
	tcp.waiting = true;
	cl.downloadcsqc = true; // asked again when the fetch ends
	Con_Printf("Fetching the server's game code (%s) over HTTP\n", tcp.known->label);
	return true;
}

void CL_ResumeHeldLoad(void); // cl_parse.c

void TouchCSProgs_Frame(void)
{
	if (cls.state != ca_connected || cls.signon != 0)
	{
		tcp.peek_done = false;
		tcp.peek_map_done = false;
		tcp.peek_map[0] = 0;
	}
	if (tcp.status == TCP_RUNNING && tcp.waiting && Sys_DirtyTime() - tcp.started > touch_csprogs_timeout.value)
	{
		// a cancel does not call TCP_Done
		Curl_Cancel_ToMemory(TCP_Done, NULL);
		TCP_Fail("too slow");
		tcp.resume = true;
	}
	if (tcp.status != TCP_RUNNING && tcp.buf)
	{
		Mem_Free(tcp.buf);
		tcp.buf = NULL;
	}
	if (tcp.resume)
	{
		tcp.resume = false;
		tcp.waiting = false;
		// the load carries on: the CSQC is in dlcache now, or the server is asked
		if (cls.state == ca_connected && cl.loadbegun && !cl.loadfinished)
			CL_ResumeHeldLoad();
	}
}

static void TouchCSProgs_Status_f(cmd_state_t *cmd)
{
	static const char *st[] = {"idle", "fetching", "done", "failed"};
	size_t i;
	Con_Printf("touch_csprogs: %s", st[tcp.status]);
	if (tcp.known)
		Con_Printf(" (%s, %i bytes, CRC %i)", tcp.known->label, tcp.size, tcp.crc);
	Con_Printf("\nknown builds:\n");
	for (i = 0; i < sizeof(tcp_known) / sizeof(tcp_known[0]); i++)
		Con_Printf("  %s: csprogs.dat.%i.%i.deflate\n", tcp_known[i].label, tcp_known[i].size, tcp_known[i].crc);
}

void TouchCSProgs_Init(void)
{
	Cvar_RegisterVariable(&touch_csprogs);
	Cvar_RegisterVariable(&touch_csprogs_url);
	Cvar_RegisterVariable(&touch_csprogs_timeout);
	Cmd_AddCommand(CF_CLIENT, "touch_csprogs_status", TouchCSProgs_Status_f, "what touch_csprogs fetched and which builds it knows");
}
