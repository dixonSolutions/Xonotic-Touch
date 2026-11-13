/*
Godnotic: answer "is this file in that loose directory?" from cached
directory listings instead of a stat() per lookup.

FS_FindFile tries every search path in turn, and every loose directory among
them (the user dir, the game dir, each *.pk3dir) costs a stat() even though
nearly every lookup misses there: the game data lives in pk3s. Xonotic's
start-up alone makes ~15000 lookups (image loaders try each extension, the
Quake status bar pictures do not exist at all) and a map load ~26000, so a
start-up and one map cost ~200000 stat()s. On Android the game data is on
external storage behind FUSE, where a stat is far dearer than on a desktop.

touch_fs_dirindex 1: each directory along a looked-up name is read once
(opendir/readdir) and kept; a name whose directory or entry is not in the
listing is a miss without a system call. A name that is listed still gets its
real stat(), so a hit means exactly what it meant before. Names are matched
case-insensitively, so a case-insensitive filesystem (Android external
storage) never loses a file. Listings are dropped when the engine writes,
creates or removes a file (FS_DIRNAMES_CHANGED, see fs.c), and are re-read
after touch_fs_dirindex_ttl seconds for files added behind its back.
Relative paths (Android APK assets), directories that cannot be listed for a
reason other than not existing, and huge directories fall back to stat().
Not built on Windows (no opendir) or Emscripten (fs.c has FS_DirMayHave).
*/

#include "quakedef.h"
#include "touch_loadfs.h"

#if !defined(WIN32) && !defined(__EMSCRIPTEN__)
#include <dirent.h>
#include <errno.h>
#define TOUCH_LOADFS_ENABLED 1
#endif

static cvar_t touch_fs_dirindex = {CF_CLIENT | CF_SERVER, "touch_fs_dirindex", "1", "answer file lookups in loose directories (user dir, game dir, *.pk3dir) from cached directory listings instead of one stat() per search path (Godnotic); 0 = stat every time"};
static cvar_t touch_fs_dirindex_ttl = {CF_CLIENT | CF_SERVER, "touch_fs_dirindex_ttl", "5", "seconds a cached directory listing is trusted for files added by other programs (the engine's own writes drop the cache at once)"};

#ifdef TOUCH_LOADFS_ENABLED

#define TLFS_HASH 1024
#define TLFS_MAXDIRS 8192
#define TLFS_MAXNAMES 20000

typedef enum { TLFS_UNKNOWN, TLFS_LISTED, TLFS_ABSENT } tlfs_state_t;

typedef struct tlfs_dir_s
{
	struct tlfs_dir_s *next;
	char *path;              // absolute, ends in '/'
	tlfs_state_t state;
	int numnames;
	char **names;            // sorted case-insensitively
	char *namebuf;
	double time;
	unsigned int generation;
}
tlfs_dir_t;

static mempool_t *tlfs_pool;
static tlfs_dir_t *tlfs_hash[TLFS_HASH];
static int tlfs_numdirs;
static unsigned int tlfs_generation = 1;

static unsigned int TLFS_Hash(const char *s)
{
	unsigned int h = 2166136261u;
	for (; *s; s++)
		h = (h ^ (unsigned char)*s) * 16777619u;
	return h & (TLFS_HASH - 1);
}

static int TLFS_CompareNames(const void *a, const void *b)
{
	return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static void TLFS_FreeListing(tlfs_dir_t *d)
{
	if (d->names)
		Mem_Free(d->names);
	if (d->namebuf)
		Mem_Free(d->namebuf);
	d->names = NULL;
	d->namebuf = NULL;
	d->numnames = 0;
}

static void TLFS_List(tlfs_dir_t *d)
{
	DIR *dir;
	struct dirent *ent;
	size_t bufsize = 0, bufused = 0;
	int count = 0, i;
	char *p;

	TLFS_FreeListing(d);
	d->time = Sys_DirtyTime();
	d->generation = tlfs_generation;
	dir = opendir(d->path);
	if (!dir)
	{
		// only a directory that is not there is a sure miss
		d->state = (errno == ENOENT || errno == ENOTDIR) ? TLFS_ABSENT : TLFS_UNKNOWN;
		return;
	}
	while ((ent = readdir(dir)))
	{
		size_t len;
		if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
			continue;
		if (++count > TLFS_MAXNAMES)
			break;
		len = strlen(ent->d_name) + 1;
		if (bufused + len > bufsize)
		{
			size_t newsize = bufsize ? bufsize * 2 : 4096;
			while (newsize < bufused + len)
				newsize *= 2;
			d->namebuf = d->namebuf ? (char *)Mem_Realloc(tlfs_pool, d->namebuf, newsize) : (char *)Mem_Alloc(tlfs_pool, newsize);
			bufsize = newsize;
		}
		memcpy(d->namebuf + bufused, ent->d_name, len);
		bufused += len;
	}
	closedir(dir);
	if (count > TLFS_MAXNAMES)
	{
		TLFS_FreeListing(d);
		d->state = TLFS_UNKNOWN;
		return;
	}
	d->numnames = count;
	if (count)
	{
		d->names = (char **)Mem_Alloc(tlfs_pool, count * sizeof(char *));
		for (i = 0, p = d->namebuf; i < count; i++, p += strlen(p) + 1)
			d->names[i] = p;
		qsort(d->names, count, sizeof(char *), TLFS_CompareNames);
	}
	d->state = TLFS_LISTED;
}

static tlfs_dir_t *TLFS_Get(const char *path)
{
	unsigned int h = TLFS_Hash(path);
	tlfs_dir_t *d;
	double now;

	for (d = tlfs_hash[h]; d; d = d->next)
		if (!strcmp(d->path, path))
			break;
	if (!d)
	{
		size_t len = strlen(path) + 1;
		if (tlfs_numdirs >= TLFS_MAXDIRS)
			return NULL;
		d = (tlfs_dir_t *)Mem_Alloc(tlfs_pool, sizeof(*d));
		d->path = (char *)Mem_Alloc(tlfs_pool, len);
		memcpy(d->path, path, len);
		d->next = tlfs_hash[h];
		tlfs_hash[h] = d;
		tlfs_numdirs++;
		TLFS_List(d);
		return d;
	}
	now = Sys_DirtyTime();
	if (d->generation != tlfs_generation || now - d->time > touch_fs_dirindex_ttl.value || now < d->time)
		TLFS_List(d);
	return d;
}

static qbool TLFS_Has(const tlfs_dir_t *d, const char *name)
{
	int lo = 0, hi = d->numnames - 1;
	while (lo <= hi)
	{
		int mid = (lo + hi) / 2;
		int c = strcasecmp(d->names[mid], name);
		if (!c)
			return true;
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return false;
}

qbool TouchLoadFS_MayHave(const char *dirpath, const char *name)
{
	char path[MAX_OSPATH], comp[MAX_OSPATH];
	size_t pathlen;
	const char *s = name;

	// FS_FindFile has no lock of its own; the experimental server thread
	// (sv_threaded) would race the cache
	if (!touch_fs_dirindex.integer || !tlfs_pool || dirpath[0] != '/' || svs.threaded)
		return true;
	pathlen = strlen(dirpath);
	if (pathlen == 0 || pathlen + 2 >= sizeof(path) || dirpath[pathlen - 1] != '/')
		return true;
	memcpy(path, dirpath, pathlen + 1);
	for (;;)
	{
		const char *slash = strchr(s, '/');
		size_t len = slash ? (size_t)(slash - s) : strlen(s);
		tlfs_dir_t *d;
		if (len == 0 || len >= sizeof(comp) || (len == 1 && s[0] == '.') || (len == 2 && s[0] == '.' && s[1] == '.'))
			return true; // not a plain name: let stat() decide
		memcpy(comp, s, len);
		comp[len] = 0;
		d = TLFS_Get(path);
		if (!d || d->state == TLFS_UNKNOWN)
			return true;
		if (d->state == TLFS_ABSENT || !TLFS_Has(d, comp))
			return false;
		if (!slash || !slash[1])
			return true;
		if (pathlen + len + 2 >= sizeof(path))
			return true;
		memcpy(path + pathlen, comp, len);
		pathlen += len;
		path[pathlen++] = '/';
		path[pathlen] = 0;
		s = slash + 1;
	}
}

void TouchLoadFS_Changed(void)
{
	tlfs_generation++;
}

static void TouchLoadFS_Flush_f(cmd_state_t *cmd)
{
	TouchLoadFS_Changed();
	Con_Printf("touch_fs_dirindex: %d directories cached, all to be read again\n", tlfs_numdirs);
}

void TouchLoadFS_Init(void)
{
	if (!tlfs_pool)
		tlfs_pool = Mem_AllocPool("touch_fs_dirindex", 0, NULL);
	Cvar_RegisterVariable(&touch_fs_dirindex);
	Cvar_RegisterVariable(&touch_fs_dirindex_ttl);
	Cmd_AddCommand(CF_CLIENT | CF_SERVER, "touch_fs_dirindex_flush", TouchLoadFS_Flush_f, "forget the cached directory listings of touch_fs_dirindex");
}

#else // no opendir, or fs.c's own FS_DirMayHave (Emscripten)

qbool TouchLoadFS_MayHave(const char *dirpath, const char *name)
{
	return true;
}

void TouchLoadFS_Changed(void)
{
}

void TouchLoadFS_Init(void)
{
	Cvar_RegisterVariable(&touch_fs_dirindex);
	Cvar_RegisterVariable(&touch_fs_dirindex_ttl);
}

#endif
