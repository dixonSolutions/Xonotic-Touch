/*
Godnotic: the Lua sandbox and the mod manager (see touch_lua.h).

What a mod can reach is exactly what this file and the two realm files put in
its lua_State; everything else in Lua's libraries is either not compiled in
(touch_lua_vm.c) or removed here before the mod's first line runs:

- libraries: base, coroutine, table, string, math, utf8. No io, os, debug,
  package; no dofile, loadfile, string.dump, warn.
- load() takes text only (mode "t", never binary chunks), strings only.
- require(name) loads <mod folder>/<name with . as />.lua and nothing else.
- setmetatable() refuses __gc: finalizers run with hooks off (during GC and
  lua_close), where no limit could stop an endless loop.
- limits per call into the mod: memory (allocator), instructions (count hook),
  wall time (count hook, and the pattern matcher through touch_lua_vm.c);
  string pattern functions refuse subjects and replacements over 1 MB and
  patterns over 512 bytes.
- library functions run in C, where the count hook cannot fire. Every loop in
  the table library is charged to the instruction budget element by element
  (touch_lua_vm.c), string.rep never loops on an empty result, and after any
  string, table or utf8 call the time limit is checked. The rest of the C
  surface is bounded by the size of its arguments (docs/MODDING.md, "C
  library functions").
- a mod that hits a limit is "killed": its hook then fires on every
  instruction and raises again, and pcall, xpcall, coroutine.resume/close and
  load re-raise instead of returning, so the mod cannot catch its own death.
  xpcall's message handler is skipped once killed (it would run inside the
  hook, where hooks are off).
- cvar writes go through an allowlist per realm plus the mod's own
  <prefix><mod>_<name> namespace; private cvars and secrets cannot be read;
  console commands go through an allowlist and may not contain ; " $ \ or
  line breaks.
- files: a mod's code, mod.txt and its storage are opened one path component
  at a time below the realm folder, never through a symlink, and only when
  the last one is a regular file with a single link. The manager's own state
  (grants, the crash marker) lives in <userdir>/luastate/, which no mod path
  reaches.
- a server cannot drive the manager: while a server this engine does not host
  may have put text in the command buffer, manager commands, the limit and
  enable cvars and mod commands refuse (TLua_FromServer).
*/

#include "quakedef.h"
#include "cl_screen.h"
#include "touch_lua.h"
#include "touch_lua_int.h"
#include "touch_lua54/lualib.h"
#include "touch_lua_lib.h"
#include "touch_lua_api_gen.h"
#include "image.h"
#include "image_png.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0 // checked with fstat as well
#endif
#else
#include <io.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static cvar_t lua_memory_mb = {CF_CLIENT | CF_SERVER | CF_ARCHIVE, "lua_memory_mb", "64", "memory limit of each Lua mod in MB (1-256)"};
static cvar_t lua_instructions = {CF_CLIENT | CF_SERVER | CF_ARCHIVE, "lua_instructions", "20000000", "instruction limit of each call into a Lua mod (an event, a command); loading gets 10 times as many"};
static cvar_t lua_time_ms = {CF_CLIENT | CF_SERVER | CF_ARCHIVE, "lua_time_ms", "100", "time limit in milliseconds of each call into a Lua mod (1-1000); loading gets 2 seconds"};
static cvar_t cl_lua_enable = {CF_CLIENT | CF_ARCHIVE, "cl_lua_enable", "1", "run the enabled Lua client mods from <userdir>/mods (lua_mods lists them)"};
static cvar_t sv_lua_enable = {CF_SERVER | CF_ARCHIVE, "sv_lua_enable", "1", "run the enabled Lua server mods from <userdir>/plugins on servers this engine hosts (sv_lua_mods lists them)"};
static cvar_t cl_lua_status = {CF_CLIENT | CF_READONLY, "_cl_lua_mods", "", "client mods for the Mods dialog: one line per mod, name<TAB>state<TAB>title<TAB>error<TAB>description<TAB>icon (mod:<id>/<file>, empty without one)"};
static cvar_t cl_lua_permissions = {CF_CLIENT | CF_READONLY, "_cl_lua_permissions", "", "client permissions: name<TAB>mode<TAB>declared<TAB>granted<TAB>not decided yet, one mod per line"};
static cvar_t sv_lua_status = {CF_CLIENT | CF_SERVER | CF_READONLY, "_sv_lua_plugins", "", "server mods for the Mods dialog: one line per server mod, name<TAB>state<TAB>title<TAB>error<TAB>description<TAB>icon (mod:<id>/<file>, empty without one)"};
static cvar_t lua_select = {CF_CLIENT, "_lua_select", "", "the Mods dialog's selection, \"mod <name>\" or \"plugin <name>\" (a server mod): _lua_detail describes it"};
static cvar_t lua_detail = {CF_CLIENT | CF_READONLY, "_lua_detail", "", "the selected mod or server mod (_lua_select) for the Mods dialog: key<TAB>value lines"};
static cvar_t lua_last = {CF_CLIENT | CF_READONLY, "_lua_last", "", "the Lua manager's last message, for the Mods dialog"};

tlua_mod_t *tlua_current;
static unsigned tlua_callframe;   // host.framecount when tlua_current was set
static double tlua_deadline;
static unsigned tlua_patsteps, tlua_worksteps;

static char tlua_key_events, tlua_key_cmds, tlua_key_loaded, tlua_key_timers, tlua_key_threads; // registry keys (addresses)
static qbool tlua_inited;
static qbool tlua_nothrottle; // set around "unload": it always runs

#define TLUA_HOOK_STEP 1000
#define TLUA_PRINTS_PER_CALL 64
#define TLUA_MAX_PATTERN 512

static void tlua_updatestatus(tlua_realm_t *r);
static void tlua_fail(tlua_mod_t *m, const char *msg);
static void tlua_marker_clear(tlua_realm_t *r);
void TouchLua_ResetAborted(lua_State *L); // touch_lua_vm.c

// ---------------------------------------------------------------------------
// Limits

static void tlua_kill(tlua_mod_t *m, const char *why)
{
	if (m->killed)
		return;
	m->killed = true;
	dp_strlcpy(m->killreason, why, sizeof(m->killreason));
}

tlua_mod_t *TLua_Self(lua_State *L)
{
	return *(tlua_mod_t **)lua_getextraspace(L);
}

static void *tlua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	tlua_mod_t *m = (tlua_mod_t *)ud;
	size_t old = ptr ? osize : 0;
	void *p;
	if (nsize == 0)
	{
		if (ptr)
		{
			m->mem -= old;
			free(ptr);
		}
		return NULL;
	}
	if (nsize > old)
	{
		if (m->killed)
			return NULL;
		if (m->mem - old + nsize > m->memlimit)
		{
			char why[64];
			dpsnprintf(why, sizeof(why), "memory limit (%i MB)", (int)(m->memlimit >> 20));
			tlua_kill(m, why);
			return NULL;
		}
	}
	p = realloc(ptr, nsize);
	if (!p)
		return nsize <= old ? ptr : NULL;
	m->mem = m->mem - old + nsize;
	return p;
}

static void tlua_hook(lua_State *L, lua_Debug *ar)
{
	tlua_mod_t *m = TLua_Self(L);
	(void)ar;
	if (!m->killed)
	{
		m->insns -= TLUA_HOOK_STEP;
		if (m->insns <= 0)
			tlua_kill(m, "instruction limit");
		else if (Sys_DirtyTime() > tlua_deadline)
			tlua_kill(m, "time limit");
	}
	if (m->killed)
	{
		// from now on every instruction of this thread raises again
		lua_sethook(L, tlua_hook, LUA_MASKCOUNT, 1);
		luaL_error(L, "%s", m->killreason);
	}
}

// touch_lua_vm.c: asked on every l_unlikely() in lstrlib.c
int TouchLua_PatternAbort(void)
{
	tlua_mod_t *m = tlua_current;
	if (!m)
		return 0;
	if (m->killed)
		return 1;
	if ((++tlua_patsteps & 15) != 0)
		return 0;
	if (Sys_DirtyTime() > tlua_deadline)
	{
		tlua_kill(m, "time limit");
		return 1;
	}
	return 0;
}

// touch_lua_vm.c: every element the table library reads (lua_geti) costs one
// instruction of the mod's budget; the time limit is checked every 256
int TouchLua_WorkStep(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	if (!m->killed)
	{
		if (--m->insns <= 0)
			tlua_kill(m, "instruction limit");
		else if ((++tlua_worksteps & 255) == 0 && Sys_DirtyTime() > tlua_deadline)
			tlua_kill(m, "time limit");
	}
	if (m->killed)
		luaL_error(L, "%s", m->killreason);
	return 0;
}

static int tlua_rethrow(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	return luaL_error(L, "%s", m->killreason);
}

// after a library function returned: the time limit, then re-raise if killed
static void tlua_aftercall(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	if (!m->killed && Sys_DirtyTime() > tlua_deadline)
		tlua_kill(m, "time limit");
	if (m->killed)
		tlua_rethrow(L);
}

static int tlua_panic(lua_State *L)
{
	const char *msg = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "?";
	Sys_Error("Lua: unprotected error (%s)", msg);
	return 0;
}

// ---------------------------------------------------------------------------
// Calls in progress, aborted frames, and text from servers

// Host_AbortCurrentFrame (loading CANCEL, Host_Error) longjmps to the top of
// the host frame. When that happens inside a call into a mod, tlua_current
// stays set and the mod's lua_State still points at the dead C frames: the
// next host frame finds it here, resets the state, stops that mod and lets
// every other mod run again.
static void tlua_recover_abort(void)
{
	tlua_mod_t *m = tlua_current;
	tlua_current = NULL;
	tlua_cl_drawing = false;
	if (!m || !m->L)
		return;
	TouchLua_ResetAborted(m->L);
	tlua_kill(m, "interrupted: the engine abandoned a frame during its call; reload it");
	tlua_fail(m, m->killreason);
	tlua_marker_clear(m->realm); // the process went on: not a crash
	tlua_updatestatus(m->realm);
}

qbool TLua_Busy(void)
{
	if (!tlua_current)
		return false;
	if (tlua_callframe == host.framecount)
		return true;
	tlua_recover_abort();
	return false;
}

// The engine runs a server's stuffed text (svc_stufftext, a server CSQC's
// localcmd) exactly like console input, so the mod manager keeps its own
// note: "tainted" from the moment a server this engine does not host can add
// text, until all of that text has run. Text it queued with defer counts too:
// the deferred commands that already existed when the taint began are
// remembered (the list node, its delay then and a hash of its text), and the
// taint lasts while any other deferred command is pending.
static qbool tlua_tainted;
#define TLUA_MAX_DEFSNAP 64
typedef struct tlua_defsnap_s { const void *node; double delay; unsigned hash; } tlua_defsnap_t;
static tlua_defsnap_t tlua_defsnap[TLUA_MAX_DEFSNAP];
static int tlua_numdefsnap;

static unsigned tlua_hash(const char *s)
{
	unsigned h = 2166136261u;
	for (; *s; s++)
		h = (h ^ (unsigned char)*s) * 16777619u;
	return h;
}

static qbool tlua_remote(void)
{
	return cls.demoplayback || (cls.state == ca_connected && !sv.active);
}

static void tlua_taint(void)
{
	cmd_input_t *d;
	if (tlua_tainted || cls.state == ca_dedicated)
		return;
	tlua_tainted = true;
	tlua_numdefsnap = 0;
	List_For_Each_Entry(d, &cmd_local->cbuf->deferred, cmd_input_t, list)
	{
		if (tlua_numdefsnap == TLUA_MAX_DEFSNAP)
			break; // the rest count as the server's: the taint just lasts longer
		tlua_defsnap[tlua_numdefsnap].node = d;
		tlua_defsnap[tlua_numdefsnap].delay = d->delay;
		tlua_defsnap[tlua_numdefsnap].hash = tlua_hash(d->text);
		tlua_numdefsnap++;
	}
}

void TouchLua_ServerText(void)
{
	if (!sv.active) // text from the server this engine hosts is our own game's
		tlua_taint();
}

static qbool tlua_presnapshot(const cmd_input_t *d)
{
	int i;
	for (i = 0; i < tlua_numdefsnap; i++)
		if (tlua_defsnap[i].node == d && d->delay <= tlua_defsnap[i].delay && tlua_defsnap[i].hash == tlua_hash(d->text))
			return true;
	return false;
}

// once per client frame, after the command buffer ran
static void tlua_taint_frame(void)
{
	cmd_buf_t *cb = cmd_local->cbuf;
	cmd_input_t *d;
	if (tlua_remote())
	{
		tlua_taint();
		return;
	}
	if (!tlua_tainted || !List_Is_Empty(&cb->start))
		return;
	List_For_Each_Entry(d, &cb->deferred, cmd_input_t, list)
		if (!tlua_presnapshot(d))
			return;
	tlua_tainted = false;
	tlua_numdefsnap = 0;
}

qbool TLua_FromServer(void)
{
	if (cls.state == ca_dedicated)
		return false;
	return tlua_remote() || tlua_tainted;
}

// ---------------------------------------------------------------------------
// Files: confined to a realm folder, no symlinks, regular files only

#ifndef _WIN32
// Opens base + rel, where base is a trusted folder (the realm's, ends in '/')
// and rel is '/'-separated components ("hud/init.lua"). Each component is
// opened from the previous one with O_NOFOLLOW, so a symlink anywhere below
// base (the mod folder, data/, the file) is refused, and must be a real
// directory; the last must be a regular file with a single link (TLUA_RD,
// TLUA_WR: created, truncated only after the checks) or a directory
// (TLUA_DIR). O_NONBLOCK keeps a FIFO from blocking the open. mkdirs creates
// missing directories on the way. Returns an fd, or -1 with errno set.
static int tlua_open_confined(const char *base, const char *rel, int mode, qbool mkdirs)
{
	char comp[256];
	const char *p = rel;
	int dirfd, fd, e;
	struct stat st;
	dirfd = open(base, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dirfd < 0)
		return -1;
	for (;;)
	{
		const char *slash = strchr(p, '/');
		size_t n = slash ? (size_t)(slash - p) : strlen(p);
		if (n == 0 || n >= sizeof(comp))
			goto refused;
		memcpy(comp, p, n);
		comp[n] = 0;
		if (!strcmp(comp, ".") || !strcmp(comp, "..") || strchr(comp, '\\'))
			goto refused;
		if (!slash && mode != TLUA_DIR)
			break;
		fd = openat(dirfd, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0 && errno == ENOENT && mkdirs && (mkdirat(dirfd, comp, 0755) == 0 || errno == EEXIST))
			fd = openat(dirfd, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0)
		{
			e = errno;
			close(dirfd);
			errno = e;
			return -1;
		}
		close(dirfd);
		dirfd = fd;
		if (fstat(dirfd, &st) || !S_ISDIR(st.st_mode))
			goto refused;
		if (!slash)
			return dirfd; // TLUA_DIR
		p = slash + 1;
	}
	fd = openat(dirfd, comp, (mode == TLUA_WR ? O_WRONLY | O_CREAT : O_RDONLY) | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0644);
	e = errno;
	close(dirfd);
	if (fd < 0)
	{
		errno = e == ELOOP ? EPERM : e;
		return -1;
	}
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink > 1 || (mode == TLUA_WR && ftruncate(fd, 0)))
	{
		close(fd);
		errno = EPERM;
		return -1;
	}
	return fd;
refused:
	close(dirfd);
	errno = EPERM;
	return -1;
}

// files in a confined directory (an fd from TLUA_DIR, which is closed)
static int tlua_countdir(int dirfd)
{
	DIR *d = fdopendir(dirfd);
	struct dirent *ent;
	int n = 0;
	if (!d)
	{
		close(dirfd);
		return -1;
	}
	while ((ent = readdir(d)))
		if (strcmp(ent->d_name, ".") && strcmp(ent->d_name, ".."))
			n++;
	closedir(d);
	return n;
}

// a direct entry of a folder that is a real directory, not a symlink
static qbool tlua_isrealdir(const char *base, const char *name)
{
	char path[2048];
	struct stat st;
	dpsnprintf(path, sizeof(path), "%s%s", base, name);
	return !lstat(path, &st) && S_ISDIR(st.st_mode);
}

// Reads a whole state file (no symlink as the last component, regular only)
static long tlua_readstate(const char *path, char *buf, size_t bufsize)
{
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	struct stat st;
	size_t total = 0;
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode))
	{
		close(fd);
		errno = EPERM;
		return -1;
	}
	while (total + 1 < bufsize)
	{
		ssize_t got = read(fd, buf + total, bufsize - 1 - total);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		total += (size_t)got;
	}
	close(fd);
	buf[total] = 0;
	return (long)total;
}

// Replaces dir + name with text: a new temporary file (never through a
// symlink), then rename, which replaces a symlink rather than following it.
// durable: fsync the file and the folder before returning.
static qbool tlua_writestate(const char *dir, const char *name, const char *text, qbool durable)
{
	char path[2048], tmp[2100];
	size_t len = strlen(text), done = 0;
	int fd, failed = 0;
	dpsnprintf(path, sizeof(path), "%s%s", dir, name);
	dpsnprintf(tmp, sizeof(tmp), "%s.%s.tmp", dir, name);
	FS_CreatePath(path);
	unlink(tmp);
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
	if (fd < 0)
		return false;
	while (done < len)
	{
		ssize_t w = write(fd, text + done, len - done);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
		{
			failed = 1;
			break;
		}
		done += (size_t)w;
	}
	if (durable && fsync(fd))
		failed = 1;
	if (close(fd))
		failed = 1;
	if (failed || rename(tmp, path))
	{
		unlink(tmp);
		return false;
	}
	if (durable && (fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) >= 0)
	{
		fsync(fd);
		close(fd);
	}
	return true;
}
#else
// Not a target of this engine; mod files and manager state fail closed.
static int tlua_open_confined(const char *base, const char *rel, int mode, qbool mkdirs) { (void)base; (void)rel; (void)mode; (void)mkdirs; errno = EPERM; return -1; }
static int tlua_countdir(int dirfd) { (void)dirfd; return -1; }
static qbool tlua_isrealdir(const char *base, const char *name) { (void)base; (void)name; return false; }
static long tlua_readstate(const char *path, char *buf, size_t bufsize) { (void)path; (void)buf; (void)bufsize; errno = EPERM; return -1; }
static qbool tlua_writestate(const char *dir, const char *name, const char *text, qbool durable) { (void)dir; (void)name; (void)text; (void)durable; return false; }
#endif

// mod code, mod.txt and saved text: read whole into one buffer (a call never
// reads a second file before it is done with the first: the chunk is
// compiled, or copied into a Lua string, before any Lua code runs)
static char tlua_filebuf[TLUA_MAX_FILE + 1];

// realm folder + rel; returns the length, -1 when it cannot be opened (errno),
// -2 when it is over 1 MB or unreadable
static long tlua_slurp(const char *base, const char *rel)
{
	int fd = tlua_open_confined(base, rel, TLUA_RD, false);
	size_t total = 0;
	if (fd < 0)
		return -1;
	while (total < sizeof(tlua_filebuf))
	{
		long got = (long)read(fd, tlua_filebuf + total, sizeof(tlua_filebuf) - total);
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0)
		{
			close(fd);
			return -2;
		}
		if (got == 0)
			break;
		total += (size_t)got;
	}
	close(fd);
	if (total > TLUA_MAX_FILE)
		return -2;
	tlua_filebuf[total] = 0;
	return (long)total;
}

static void tlua_dir(tlua_realm_t *r, char *out, size_t outsize)
{
	dpsnprintf(out, outsize, "%s%s/", fs_userdir, r->subdir);
}

// the manager's own files: <userdir>/luastate/<realm>/, outside every folder
// a mod package unpacks into or a mod can write to
static void tlua_statedir(tlua_realm_t *r, char *out, size_t outsize)
{
	dpsnprintf(out, outsize, "%sluastate/%s/", fs_userdir, r->subdir);
}

void TLua_RealmDir(tlua_realm_t *r, char *out, size_t outsize) { tlua_dir(r, out, outsize); }
void TLua_StateDir(tlua_realm_t *r, char *out, size_t outsize) { tlua_statedir(r, out, outsize); }
int TLua_OpenConfined(const char *base, const char *rel, int mode, qbool mkdirs) { return tlua_open_confined(base, rel, mode, mkdirs); }
long TLua_ReadState(const char *path, char *buf, size_t bufsize) { return tlua_readstate(path, buf, bufsize); }
qbool TLua_WriteState(const char *dir, const char *name, const char *text, qbool durable) { return tlua_writestate(dir, name, text, durable); }

// ---------------------------------------------------------------------------
// Recent output per mod, and the manager's last message

static qbool tlua_detail_dirty;

void TLua_Log(tlua_mod_t *m, const char *fmt, ...)
{
	va_list ap;
	char text[TLUA_LOGLEN * 2];
	va_start(ap, fmt);
	dpvsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	TLua_CleanText(text, m->log[m->lognext], TLUA_LOGLEN, false);
	m->lognext = (m->lognext + 1) % TLUA_LOGLINES;
	if (m->logcount < TLUA_LOGLINES)
		m->logcount++;
	tlua_detail_dirty = true;
}

void TLua_Say(const char *fmt, ...)
{
	va_list ap;
	char text[1024], clean[1024];
	va_start(ap, fmt);
	dpvsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	Con_Printf("%s\n", text);
	if (tlua_inited && cls.state != ca_dedicated)
		Cvar_SetQuick(&lua_last, TLua_CleanText(text, clean, sizeof(clean), false));
}

// Reads <mod>/<rel> into the shared buffer; NULL when it does not exist.
static const char *tlua_readfile(lua_State *L, tlua_mod_t *m, const char *rel, size_t *outlen)
{
	char dir[1024], path[512];
	long n;
	tlua_dir(m->realm, dir, sizeof(dir));
	dpsnprintf(path, sizeof(path), "%s/%s", m->name, rel);
	n = tlua_slurp(dir, path);
	if (n == -1 && errno == ENOENT)
		return NULL;
	if (n == -1)
	{
		luaL_error(L, "%s: refused (a symlink, a hard link or not a regular file)", rel);
		return NULL;
	}
	if (n < 0)
	{
		luaL_error(L, "%s: unreadable or over 1 MB", rel);
		return NULL;
	}
	*outlen = (size_t)n;
	return tlua_filebuf;
}

// ---------------------------------------------------------------------------
// The sandboxed base functions

static int tlua_print(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	char line[1024], clean[1024];
	size_t used = 0;
	int i, n = lua_gettop(L);
	line[0] = 0;
	if (m->prints <= 0)
		return 0;
	if (--m->prints == 0)
	{
		Con_Printf("^3[%s]^7 (more prints this call dropped)\n", m->name);
		return 0;
	}
	// stops at a full line: print(table.unpack(t)) can pass a million values
	for (i = 1; i <= n && used + 1 < sizeof(line); i++)
	{
		const char *s;
		switch (lua_type(L, i))
		{
		case LUA_TSTRING:
		case LUA_TNUMBER:
			s = lua_tostring(L, i); // a number turns into a string in place; harmless here
			break;
		case LUA_TBOOLEAN:
			s = lua_toboolean(L, i) ? "true" : "false";
			break;
		case LUA_TNIL:
			s = "nil";
			break;
		default:
			s = lua_typename(L, lua_type(L, i));
			break;
		}
		if (i > 1)
			dp_strlcat(line, " ", sizeof(line));
		dp_strlcat(line, s, sizeof(line));
		used = strlen(line);
	}
	Con_Printf("^3[%s]^7 %s\n", m->name, TLua_CleanText(line, clean, sizeof(clean), false));
	TLua_Log(m, "%s", clean);
	return 0;
}

static int tlua_finishpcall(lua_State *L, int status, lua_KContext extra)
{
	if (TLua_Self(L)->killed)
		return tlua_rethrow(L);
	if (status != LUA_OK && status != LUA_YIELD)
	{
		lua_pushboolean(L, 0);
		lua_pushvalue(L, -2);
		return 2;
	}
	return lua_gettop(L) - (int)extra;
}

static int tlua_pcall(lua_State *L)
{
	int status;
	luaL_checkany(L, 1);
	lua_pushboolean(L, 1);
	lua_insert(L, 1);
	status = lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0, tlua_finishpcall);
	return tlua_finishpcall(L, status, 0);
}

// xpcall's handler, wrapped: skipped once the mod is killed
static int tlua_msgh(lua_State *L)
{
	if (TLua_Self(L)->killed)
		return 1;
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_insert(L, 1);
	lua_call(L, lua_gettop(L) - 1, 1);
	return 1;
}

static int tlua_xpcall(lua_State *L)
{
	int status;
	int n = lua_gettop(L);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	lua_pushvalue(L, 2);
	lua_pushcclosure(L, tlua_msgh, 1);
	lua_replace(L, 2);
	lua_pushboolean(L, 1);
	lua_pushvalue(L, 1);
	lua_rotate(L, 3, 2);
	status = lua_pcallk(L, n - 2, LUA_MULTRET, 2, 2, tlua_finishpcall);
	return tlua_finishpcall(L, status, 2);
}

// calls upvalue 1 with all arguments, then re-raises if the mod got killed
static int tlua_passthrough(lua_State *L)
{
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_insert(L, 1);
	lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
	if (TLua_Self(L)->killed)
		return tlua_rethrow(L);
	return lua_gettop(L);
}

// a string, table or utf8 function: upvalue 1, then the time limit
static int tlua_libcall(lua_State *L)
{
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_insert(L, 1);
	lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
	tlua_aftercall(L);
	return lua_gettop(L);
}

// find, match, gmatch, gsub: bounded subject, pattern and replacement
static int tlua_strguard(lua_State *L)
{
	size_t len;
	if (lua_type(L, 1) == LUA_TSTRING)
	{
		lua_tolstring(L, 1, &len);
		if (len > TLUA_MAX_PATTERN_SUBJECT)
			return luaL_error(L, "string too long for pattern functions (limit 1 MB)");
	}
	if (lua_type(L, 2) == LUA_TSTRING)
	{
		lua_tolstring(L, 2, &len);
		if (len > TLUA_MAX_PATTERN)
			return luaL_error(L, "pattern too long (limit %d bytes)", TLUA_MAX_PATTERN);
	}
	// gsub copies the replacement once per match: keep it as small as a subject
	if (lua_type(L, 3) == LUA_TSTRING)
	{
		lua_tolstring(L, 3, &len);
		if (len > TLUA_MAX_PATTERN_SUBJECT)
			return luaL_error(L, "replacement too long (limit 1 MB)");
	}
	return tlua_libcall(L);
}

// string.rep: the original loops n times even when the result is empty
// (string.rep("", math.maxinteger) never returned). With something to copy,
// n is at most the result's length, which the allocator holds to the memory
// limit before the loop starts.
static int tlua_rep(lua_State *L)
{
	size_t l, lsep;
	lua_Integer n;
	luaL_checklstring(L, 1, &l);
	n = luaL_checkinteger(L, 2);
	luaL_optlstring(L, 3, "", &lsep);
	if (n <= 0 || l + lsep == 0)
	{
		lua_pushliteral(L, "");
		return 1;
	}
	return tlua_libcall(L);
}

static int tlua_load(lua_State *L)
{
	size_t len;
	const char *s, *chunkname;
	int status;
	if (lua_type(L, 1) != LUA_TSTRING)
		return luaL_error(L, "load: only a string chunk is allowed here");
	s = lua_tolstring(L, 1, &len);
	if (len > TLUA_MAX_FILE)
		return luaL_error(L, "load: chunk too long (limit 1 MB)");
	chunkname = luaL_optstring(L, 2, "=(load)");
	// argument 3 (mode) is ignored: always text, never precompiled bytecode
	status = luaL_loadbufferx(L, s, len, chunkname, "t");
	if (TLua_Self(L)->killed)
		return tlua_rethrow(L);
	if (status != LUA_OK)
	{
		lua_pushnil(L);
		lua_insert(L, -2);
		return 2;
	}
	if (!lua_isnone(L, 4))
	{
		lua_pushvalue(L, 4);
		if (!lua_setupvalue(L, -2, 1))
			lua_pop(L, 1);
	}
	return 1;
}

static int tlua_setmetatable(lua_State *L)
{
	int t = lua_type(L, 2);
	luaL_checktype(L, 1, LUA_TTABLE);
	luaL_argexpected(L, t == LUA_TNIL || t == LUA_TTABLE, 2, "nil or table");
	if (t == LUA_TTABLE)
	{
		lua_pushliteral(L, "__gc");
		if (lua_rawget(L, 2) != LUA_TNIL)
			return luaL_error(L, "setmetatable: __gc metamethods are not allowed");
		lua_pop(L, 1);
	}
	if (luaL_getmetafield(L, 1, "__metatable") != LUA_TNIL)
		return luaL_error(L, "cannot change a protected metatable");
	lua_settop(L, 2);
	lua_setmetatable(L, 1);
	return 1;
}

static int tlua_collectgarbage(lua_State *L)
{
	const char *opt = luaL_optstring(L, 1, "collect");
	if (!strcmp(opt, "count"))
	{
		lua_pushnumber(L, (lua_Number)lua_gc(L, LUA_GCCOUNT) + (lua_Number)lua_gc(L, LUA_GCCOUNTB) / 1024.0);
		return 1;
	}
	if (!strcmp(opt, "collect"))
	{
		lua_gc(L, LUA_GCCOLLECT);
		lua_pushinteger(L, 0);
		return 1;
	}
	if (!strcmp(opt, "step"))
	{
		lua_pushboolean(L, lua_gc(L, LUA_GCSTEP, 0));
		return 1;
	}
	return luaL_error(L, "collectgarbage: only \"count\", \"collect\" and \"step\" are allowed");
}

static qbool tlua_validname(const char *s, qbool dots)
{
	const char *p;
	size_t n = strlen(s);
	if (n == 0 || n > 64)
		return false;
	for (p = s; *p; p++)
	{
		if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')
			continue;
		if (dots && *p == '.' && p != s && p[1] && p[1] != '.')
			continue;
		if (dots && *p >= 'A' && *p <= 'Z')
			continue;
		return false;
	}
	return true;
}

qbool TLua_ValidModName(const char *name)
{
	return strlen(name) <= TLUA_NAME_LEN && tlua_validname(name, false);
}

// the <name> of a mod's own command or cvar: a-z, 0-9, no '_', so that
// <prefix><mod>_<name> names exactly one mod (mod names may contain '_')
static qbool tlua_validsuffix(const char *s)
{
	size_t n = strlen(s);
	if (n == 0 || n > TLUA_SUFFIX_LEN)
		return false;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9')))
			return false;
	return true;
}

static int tlua_require(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	const char *name = luaL_checkstring(L, 1);
	char rel[256], chunk[320];
	const char *src;
	size_t len, i;
	// Only the name: the loaded table is addressed below as stack index 2, and
	// a second argument (pcall(require, "x", 1)) used to sit there instead.
	lua_settop(L, 1);
	if (!tlua_validname(name, true) || strlen(name) > 120)
		return luaL_error(L, "require: bad module name '%s' (letters, digits, _ and dots only)", name);
	lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_loaded);
	lua_pushvalue(L, 1);
	if (lua_rawget(L, -2) != LUA_TNIL)
		return 1;
	lua_pop(L, 1);
	if (!strcmp(name, "xt.ui") && m->realm == &tlua_client)
	{
		if (luaL_loadbufferx(L, tlua_lib_ui, sizeof(tlua_lib_ui) - 1, "=xt.ui", "t") != LUA_OK)
			return lua_error(L);
		lua_pushvalue(L, 1);
		lua_call(L, 1, 1);
		lua_pushvalue(L, 1);
		lua_pushvalue(L, -2);
		lua_rawset(L, 2);
		return 1;
	}
	dp_strlcpy(rel, name, sizeof(rel) - 4);
	for (i = 0; rel[i]; i++)
		if (rel[i] == '.')
			rel[i] = '/';
	dp_strlcat(rel, ".lua", sizeof(rel));
	src = tlua_readfile(L, m, rel, &len);
	if (!src)
		return luaL_error(L, "require: module '%s' not found in this mod's folder (%s)", name, rel);
	dpsnprintf(chunk, sizeof(chunk), "@%s/%s", m->name, rel);
	if (luaL_loadbufferx(L, src, len, chunk, "t") != LUA_OK)
		return lua_error(L);
	lua_pushvalue(L, 1);
	lua_call(L, 1, 1);
	if (lua_isnil(L, -1))
	{
		lua_pop(L, 1);
		lua_pushboolean(L, 1);
	}
	lua_pushvalue(L, 1);
	lua_pushvalue(L, -2);
	lua_rawset(L, 2); // loaded[name] = result
	return 1;
}

const char *TLua_ReadModFile(lua_State *L, tlua_mod_t *m, const char *rel, size_t *outlen)
{
	return tlua_readfile(L, m, rel, outlen);
}

// ---------------------------------------------------------------------------
// The common xt API

void TLua_SetField(lua_State *L, const char *k, lua_CFunction f)
{
	lua_pushcfunction(L, f);
	lua_setfield(L, -2, k);
}

const char *TLua_CleanText(const char *in, char *out, size_t outsize, qbool allownewline)
{
	size_t o = 0;
	for (; *in && o + 1 < outsize; in++)
	{
		unsigned char c = (unsigned char)*in;
		if (c == '\n' && allownewline)
			out[o++] = '\n';
		else if (c < 32 || c == 127)
			out[o++] = ' ';
		else
			out[o++] = (char)c;
	}
	out[o] = 0;
	return out;
}

static int tlua_eventindex(tlua_realm_t *r, const char *ev)
{
	int i;
	for (i = 0; r->events[i]; i++)
		if (!strcmp(r->events[i], ev))
			return i;
	return -1;
}

static unsigned long long *tlua_maskof(tlua_mod_t *m)
{
	return &m->eventmask;
}

// docs/PLUGIN_STANDARD.md, "Tiers"
const tlua_permission_t tlua_permissions[] = {
	{"standard", TLUA_TIER_STANDARD, 1, "what every mod can do: draw, listen to the game, its own settings, buttons and storage"},
	{"gamerules", TLUA_TIER_GAMERULES, 1, "change the game in matches you host: damage, items, players, entities, votes, the match"},
	{"internet", TLUA_TIER_INTERNET, 3, "connect to the web sites its manifest names (https)"},
	{"engine_full", TLUA_TIER_ENGINE, 3, "WARNING: full engine access: any console command and any setting except passwords and keys"},
	{NULL, 0, 0, NULL}
};

// API 1/2 scope names: still valid in mod.txt ("permissions:", "reason.<scope>")
// and in lua_mod_permission, as the tier they belong to now
typedef struct tlua_alias_s { const char *name; unsigned bit; int api; } tlua_alias_t;
static const tlua_alias_t tlua_scope_aliases[] = {
	{"events", TLUA_TIER_STANDARD, 1}, {"commands", TLUA_TIER_STANDARD, 1}, {"cvar_read", TLUA_TIER_STANDARD, 1},
	{"cvar_write", TLUA_TIER_STANDARD, 1}, {"exec", TLUA_TIER_STANDARD, 1}, {"storage", TLUA_TIER_STANDARD, 1},
	{"display", TLUA_TIER_STANDARD, 1}, {"input", TLUA_TIER_STANDARD, 1}, {"players", TLUA_TIER_STANDARD, 1},
	{"messages", TLUA_TIER_STANDARD, 1}, {"state", TLUA_TIER_STANDARD, 1}, {"hooks", TLUA_TIER_STANDARD, 2},
	{"hooks_write", TLUA_TIER_GAMERULES, 2}, {"game_rules", TLUA_TIER_GAMERULES, 3}, {"http", TLUA_TIER_INTERNET, 3},
	{"engine", TLUA_TIER_ENGINE, 3}, {NULL, 0, 0}
};

static qbool tlua_has(tlua_mod_t *m, unsigned permission)
{
	if (permission == TLUA_TIER_STANDARD)
		return true; // granted with turning the mod on
	return (m->requested & m->granted & permission) == permission;
}

qbool TLua_Has(tlua_mod_t *m, unsigned permission)
{
	return tlua_has(m, permission);
}

// the requested scopes not granted yet, comma separated ("" when none)
static void tlua_missing(tlua_mod_t *m, char *out, size_t outsize)
{
	int i;
	out[0] = 0;
	for (i = 0; tlua_permissions[i].name; i++)
		if ((m->requested & tlua_permissions[i].bit) && !(m->granted & tlua_permissions[i].bit))
		{
			if (out[0])
				dp_strlcat(out, ", ", outsize);
			dp_strlcat(out, tlua_permissions[i].name, outsize);
		}
}

void TLua_Require(lua_State *L, unsigned permission)
{
	tlua_mod_t *m = TLua_Self(L);
	const char *name = "?";
	int i;
	if (tlua_has(m, permission))
	{
		// what a mod actually used, for lua_mod_info and the Mods page
		for (i = 0; tlua_permissions[i].name; i++)
			if (tlua_permissions[i].bit == permission && permission != TLUA_TIER_STANDARD)
				m->tiercalls[i]++;
		return;
	}
	for (i = 0; tlua_permissions[i].name; i++)
		if (tlua_permissions[i].bit == permission)
			name = tlua_permissions[i].name;
	if (!(m->requested & permission) && m->nomanifest)
		luaL_error(L, "permission denied: %s: this mod has no mod.txt, so it declares nothing beyond the standard tier", name);
	if (!(m->requested & permission))
		luaL_error(L, "permission denied: needs the %s tier (add \"permissions: %s\" to mod.txt)", name, name);
	luaL_error(L, "permission denied: the %s tier is not turned on for this mod (Mods, or %s%s_permission %s grant %s)",
		name, m->realm->cmdprefix, "mod", m->name, name);
}

static int tlua_on(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	const char *ev = luaL_checkstring(L, 1);
	int idx = tlua_eventindex(m->realm, ev);
	TLua_Require(L, TLUA_PERM_EVENTS);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	if (idx < 0)
		return luaL_error(L, "xt.on: unknown event '%s'", ev);
	lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_events);
	lua_pushvalue(L, 1);
	if (lua_rawget(L, -2) != LUA_TTABLE)
	{
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, 1);
		lua_pushvalue(L, -2);
		lua_rawset(L, -4);
	}
	lua_pushvalue(L, 2);
	lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
	*tlua_maskof(m) |= 1ull << idx;
	return 0;
}

// Console commands mods register. The engine cannot remove a command, so a
// slot (and its engine command) outlives the mod's state, belongs to the
// realm and mod name that made it, and is taken again when that mod
// registers the same name; the dispatcher looks the mod up on each call.
// The engine keeps the description pointer: it points into the slot and is
// rewritten on every registration, so it always is the current mod's.
typedef struct tlua_cmd_s
{
	char full[96];
	tlua_realm_t *realm;
	char mod[TLUA_NAME_LEN + 1];
	char cmd[TLUA_SUFFIX_LEN + 1];
	char desc[200];
	qbool fromserver;               // the mod lets a server run it (xt.command option)
} tlua_cmd_t;
#define TLUA_MAX_CMDSLOTS (TLUA_MAX_MODS * 2 * TLUA_MAX_MODCMDS)
static tlua_cmd_t tlua_cmds[TLUA_MAX_CMDSLOTS];
static int tlua_numcmds;

static void tlua_modcmd_f(cmd_state_t *cmd);

// xt.command(name, fn [, help [, {fromserver = true}]])
static int tlua_command(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	const char *name = luaL_checkstring(L, 1);
	const char *help = luaL_optstring(L, 3, "Lua mod command");
	char full[96], clean[160];
	int i, live = 0;
	qbool fromserver = false;
	TLua_Require(L, TLUA_PERM_COMMANDS);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	if (!lua_isnoneornil(L, 4))
	{
		luaL_checktype(L, 4, LUA_TTABLE);
		lua_getfield(L, 4, "fromserver");
		fromserver = lua_toboolean(L, -1);
		lua_pop(L, 1);
	}
	lua_settop(L, 2);
	if (!tlua_validsuffix(name))
		return luaL_error(L, "xt.command: bad name '%s' (a-z and 0-9, up to %d, no '_')", name, TLUA_SUFFIX_LEN);
	dpsnprintf(full, sizeof(full), "%s%s_%s", m->realm->cmdprefix, m->name, name);
	// the cap counts the commands this state registered
	lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_cmds);
	lua_pushvalue(L, 1);
	if (lua_rawget(L, -2) == LUA_TNIL)
	{
		lua_pop(L, 1);
		lua_pushnil(L);
		while (lua_next(L, -2))
		{
			live++;
			lua_pop(L, 1);
		}
		if (live >= TLUA_MAX_MODCMDS)
			return luaL_error(L, "xt.command: too many commands (%d per mod)", TLUA_MAX_MODCMDS);
	}
	else
		lua_pop(L, 1);
	for (i = 0; i < tlua_numcmds; i++)
		if (!strcmp(tlua_cmds[i].full, full))
			break;
	if (i < tlua_numcmds && (tlua_cmds[i].realm != m->realm || strcmp(tlua_cmds[i].mod, m->name)))
		return luaL_error(L, "xt.command: '%s' already exists", full);
	if (i == tlua_numcmds)
	{
		if (tlua_numcmds >= TLUA_MAX_CMDSLOTS)
			return luaL_error(L, "xt.command: too many commands");
		if (Cmd_Exists(cmd_local, full) || Cvar_FindVar(&cvars_all, full, ~0))
			return luaL_error(L, "xt.command: '%s' already exists", full);
		dp_strlcpy(tlua_cmds[i].full, full, sizeof(tlua_cmds[i].full));
		tlua_cmds[i].realm = m->realm;
		dp_strlcpy(tlua_cmds[i].mod, m->name, sizeof(tlua_cmds[i].mod));
		dp_strlcpy(tlua_cmds[i].cmd, name, sizeof(tlua_cmds[i].cmd));
		tlua_numcmds++;
		dpsnprintf(tlua_cmds[i].desc, sizeof(tlua_cmds[i].desc), "Lua %s %s: %s", TLUA_NOUN(m->realm), m->name, TLua_CleanText(help, clean, sizeof(clean), false));
		Cmd_AddCommand(m->realm->cmdflags, tlua_cmds[i].full, tlua_modcmd_f, tlua_cmds[i].desc);
	}
	else
		dpsnprintf(tlua_cmds[i].desc, sizeof(tlua_cmds[i].desc), "Lua %s %s: %s", TLUA_NOUN(m->realm), m->name, TLua_CleanText(help, clean, sizeof(clean), false));
	tlua_cmds[i].fromserver = fromserver;
	lua_pushvalue(L, 1);
	lua_pushvalue(L, 2);
	lua_rawset(L, -3);
	lua_pushstring(L, full);
	return 1;
}

static qbool tlua_cvar_secret(const char *name)
{
	static const char *const bad[] = {"password", "rcon", "secret", "token", "crypto", "_key", "log_dest", "cookie", "auth", NULL};
	int i;
	char lower[256];
	size_t k;
	for (k = 0; name[k] && k + 1 < sizeof(lower); k++)
		lower[k] = (char)tolower((unsigned char)name[k]);
	lower[k] = 0;
	for (i = 0; bad[i]; i++)
		if (strstr(lower, bad[i]))
			return true;
	return false;
}

qbool TLua_CvarSecret(const char *name) { return tlua_cvar_secret(name); }

// The bridge's image refs into a mod's package (touch_uib_img.c):
// "mod:<id>/<file>", its icon or an image row of its pages; a .png/.tga read
// from the mod's folder (client mods first, then server mods), confined like
// every mod file, at most 256 KB and 256x256.
// Returns BGRA pixels from the image decoders (Mem_Free them) with
// image_width / image_height set, or NULL.
unsigned char *TLua_ModImageBGRA(const char *ref)
{
	char id[TLUA_NAME_LEN + 1], dir[1024], rel[128];
	const char *slash, *file;
	tlua_realm_t *realms[2] = {&tlua_client, &tlua_server};
	tlua_mod_t *m = NULL;
	int r, mip = 0;
	long n;
	size_t fl;
	unsigned char *px;
	if (strncmp(ref, "mod:", 4) || !(slash = strchr(ref + 4, '/')) || (size_t)(slash - ref - 4) > TLUA_NAME_LEN)
		return NULL;
	memcpy(id, ref + 4, (size_t)(slash - ref - 4));
	id[slash - ref - 4] = 0;
	file = slash + 1;
	for (r = 0; r < 2 && !m; r++)
	{
		if (!(m = TLua_Find(realms[r], id)) && !TLua_Busy())
		{
			TLua_Rescan(realms[r]);
			m = TLua_Find(realms[r], id);
		}
		if (m && m->manifest_error)
			m = NULL;
	}
	fl = strlen(file);
	if (!m || fl < 5 || fl > 96 || strstr(file, "..") || file[0] == '/' || file[0] == '.' || strspn(file, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/_-.") != fl
		|| (strcasecmp(file + fl - 4, ".png") && strcasecmp(file + fl - 4, ".tga")))
		return NULL;
	tlua_dir(m->realm, dir, sizeof(dir));
	dpsnprintf(rel, sizeof(rel), "%s/%s", m->name, file);
	n = tlua_slurp(dir, rel);
	if (n <= 0 || n > 256 * 1024)
		return NULL;
	px = !strcasecmp(file + fl - 4, ".png") ? PNG_LoadImage_BGRA((const unsigned char *)tlua_filebuf, (int)n, &mip)
		: LoadTGA_BGRA((const unsigned char *)tlua_filebuf, (int)n, &mip);
	if (px && (image_width <= 0 || image_height <= 0 || image_width > 256 || image_height > 256))
	{
		Mem_Free(px);
		px = NULL;
	}
	return px;
}

static int tlua_cvar(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	cvar_t *v = Cvar_FindVar(&cvars_all, name, ~0);
	TLua_Require(L, TLUA_PERM_CVAR_READ);
	if (!v || (v->flags & CF_PRIVATE) || tlua_cvar_secret(name))
	{
		lua_pushnil(L);
		return 1;
	}
	lua_pushstring(L, v->string);
	return 1;
}

// <prefix><mod>_<name>: lua_hud_clock_corner, sv_lua_underdog_scale
static qbool tlua_owncvar(tlua_mod_t *m, const char *name)
{
	char prefix[96];
	dpsnprintf(prefix, sizeof(prefix), "%s%s_", m->realm->cmdprefix, m->name);
	return !strncmp(name, prefix, strlen(prefix)) && tlua_validsuffix(name + strlen(prefix));
}

static qbool tlua_cleanvalue(const char *s)
{
	if (strlen(s) > 256)
		return false;
	for (; *s; s++)
		if ((unsigned char)*s < 32 || *s == '"' || *s == ';' || *s == '$' || *s == '\\')
			return false;
	return true;
}

qbool TLua_OwnCvar(tlua_mod_t *m, const char *name) { return tlua_owncvar(m, name); }
qbool TLua_CleanValue(const char *s) { return tlua_cleanvalue(s); }

static int tlua_setcvar(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	const char *name = luaL_checkstring(L, 1);
	const char *value;
	cvar_t *v;
	TLua_Require(L, TLUA_PERM_CVAR_WRITE);
	if (lua_type(L, 2) == LUA_TBOOLEAN)
		value = lua_toboolean(L, 2) ? "1" : "0";
	else
		value = luaL_checkstring(L, 2);
	if (!tlua_cleanvalue(value))
		return luaL_error(L, "xt.setcvar: value has a forbidden character or is too long");
	v = Cvar_FindVar(&cvars_all, name, ~0);
	if (tlua_owncvar(m, name))
	{
		// saved in config.cfg only with storage: a cvar per call used to be
		// another archived line, thousands of them from one loop
		qbool archive = tlua_has(m, TLUA_PERM_STORAGE);
		if (!v)
		{
			char desc[96];
			if (m->owncvars < 0)
			{
				m->owncvars = 0;
				for (v = cvars_all.vars; v; v = v->next)
					if (tlua_owncvar(m, v->name))
						m->owncvars++;
			}
			if (m->owncvars >= TLUA_MAX_MODCVARS)
				return luaL_error(L, "xt.setcvar: too many cvars (%d per mod)", TLUA_MAX_MODCVARS);
			if (Cmd_Exists(cmd_local, name))
				return luaL_error(L, "xt.setcvar: '%s' is a command", name);
			dpsnprintf(desc, sizeof(desc), "setting of Lua %s %s", TLUA_NOUN(m->realm), m->name);
			Cvar_Get(&cvars_all, name, value, m->realm->cmdflags | (archive ? CF_ARCHIVE : 0), desc);
			m->owncvars++;
			lua_pushboolean(L, 1);
			return 1;
		}
		if (!archive && (v->flags & CF_ALLOCATED))
			v->flags &= ~CF_ARCHIVE;
	}
	else if (!m->realm->cvar_writable(name) || tlua_cvar_secret(name))
		return luaL_error(L, "xt.setcvar: '%s' is not on the allowlist (see docs/MODDING.md)", name);
	if (!v || (v->flags & (CF_PRIVATE | CF_READONLY)))
		return luaL_error(L, "xt.setcvar: no writable cvar '%s'", name);
	Cvar_SetQuick(v, value);
	lua_pushboolean(L, 1);
	return 1;
}

static int tlua_exec(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	const char *line = luaL_checkstring(L, 1);
	char word[64];
	const char *p;
	size_t n = 0;
	TLua_Require(L, TLUA_PERM_EXEC);
	if (strlen(line) > 256)
		return luaL_error(L, "xt.exec: command too long");
	for (p = line; *p; p++)
		if ((unsigned char)*p < 32 || *p == ';' || *p == '"' || *p == '$' || *p == '\\')
			return luaL_error(L, "xt.exec: ; \" $ \\ and control characters are not allowed");
	for (p = line; *p == ' '; p++)
		;
	while (*p && *p != ' ' && n + 1 < sizeof(word))
		word[n++] = (char)tolower((unsigned char)*p++);
	word[n] = 0;
	if (!n || !m->realm->command_allowed(word, line))
		return luaL_error(L, "xt.exec: '%s' is not on the allowlist (see docs/MODDING.md)", word);
	if (m->execs <= 0)
		return luaL_error(L, "xt.exec: too many commands in one call");
	m->execs--;
	{
		char text[300];
		dpsnprintf(text, sizeof(text), "%s\n", line);
		Cbuf_AddText(cmd_local, text);
	}
	return 0;
}

// xt.save(key, text) / xt.load(key): small text files in <mod folder>/data/,
// opened with tlua_open_confined (no symlink at any level, regular files)
#define TLUA_MAX_SAVE (64 * 1024)
#define TLUA_MAX_SAVEFILES 64

static void tlua_savekey(lua_State *L, tlua_mod_t *m, char *rel, size_t relsize)
{
	const char *key = luaL_checkstring(L, 1);
	if (!tlua_validname(key, false) || strlen(key) > TLUA_NAME_LEN)
		luaL_error(L, "bad key '%s' (a-z, 0-9, _; up to 32)", key);
	dpsnprintf(rel, relsize, "%s/data/%s.txt", m->name, key);
}

static int tlua_save(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	char rel[128], dir[1024], datadir[96];
	size_t len, done = 0;
	const char *text;
	int fd;
	TLua_Require(L, TLUA_PERM_STORAGE);
	tlua_savekey(L, m, rel, sizeof(rel));
	text = luaL_checklstring(L, 2, &len);
	if (len > TLUA_MAX_SAVE)
		return luaL_error(L, "xt.save: over 64 KB");
	tlua_dir(m->realm, dir, sizeof(dir));
	dpsnprintf(datadir, sizeof(datadir), "%s/data", m->name);
	fd = tlua_open_confined(dir, datadir, TLUA_DIR, true);
	if (fd < 0)
		return luaL_error(L, "xt.save: the data folder is refused (a symlink or not a folder)");
	{
		// a new key: keep the number of files bounded
		int exists = tlua_open_confined(dir, rel, TLUA_RD, false), n;
		if (exists >= 0)
		{
			close(exists);
			close(fd);
		}
		else if (errno != ENOENT)
		{
			close(fd);
			return luaL_error(L, "xt.save: refused (a symlink, a hard link or not a regular file)");
		}
		else if ((n = tlua_countdir(fd)) < 0 || n >= TLUA_MAX_SAVEFILES)
			return luaL_error(L, "xt.save: too many keys (%d)", TLUA_MAX_SAVEFILES);
	}
	fd = tlua_open_confined(dir, rel, TLUA_WR, false);
	if (fd < 0)
		return luaL_error(L, "xt.save: refused (a symlink, a hard link or not a regular file)");
	while (done < len)
	{
		long w = (long)write(fd, text + done, len - done);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
		{
			close(fd);
			return luaL_error(L, "xt.save: write error");
		}
		done += (size_t)w;
	}
	close(fd);
	lua_pushboolean(L, 1);
	return 1;
}

static int tlua_load_saved(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	char rel[128], dir[1024];
	long n;
	TLua_Require(L, TLUA_PERM_STORAGE);
	tlua_savekey(L, m, rel, sizeof(rel));
	tlua_dir(m->realm, dir, sizeof(dir));
	n = tlua_slurp(dir, rel);
	if (n < 0)
		return 0; // missing, or refused: nothing a mod may read
	lua_pushlstring(L, tlua_filebuf, (size_t)n);
	return 1;
}

// xt.setting(name): the value of one of the settings its mod.txt declares
// (the cvar <prefix><mod>_<name>); its own, so no permission is needed
static int tlua_setting(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	const char *name = luaL_checkstring(L, 1);
	char cv[96];
	int i;
	for (i = 0; i < m->numsettings; i++)
		if (!strcmp(m->settings[i].name, name))
			break;
	if (i == m->numsettings)
		return luaL_error(L, "xt.setting: '%s' is not declared in mod.txt (setting.%s: ...)", name, name);
	dpsnprintf(cv, sizeof(cv), "%s%s_%s", m->realm->cmdprefix, m->name, name);
	lua_pushstring(L, Cvar_VariableString(&cvars_all, cv, ~0));
	return 1;
}

static int tlua_realtime(lua_State *L)
{
	TLua_Require(L, TLUA_PERM_STATE);
	lua_pushnumber(L, host.realtime);
	return 1;
}

// ---------------------------------------------------------------------------
// Timers and coroutines: how a mod does continuous work. A literal endless
// loop is stopped by the instruction and time limits by design; instead a mod
// schedules work and the engine calls it back between frames, each pass under
// one call's limits:
//   xt.after(seconds, fn)      once
//   xt.every(seconds, fn)      repeatedly (0: every frame); fn returning false stops it
//   xt.cancel(id)
//   xt.spawn(fn)               fn runs as a coroutine; inside it xt.sleep(seconds)
//                              pauses it (0: until the next frame), so
//                              "while true do work() xt.sleep(1) end" is fine
// All of a mod's timers and coroutine steps due in one frame share one call's
// budget (lua_instructions, lua_time_ms).

static int tlua_timer_add(lua_State *L, double delay, double every, int fnidx, qbool thread)
{
	tlua_mod_t *m = TLua_Self(L);
	static int nextid;
	double due;
	if (m->numtimers >= TLUA_MAX_TIMERS)
		return luaL_error(L, "too many timers and coroutines (%d per mod)", TLUA_MAX_TIMERS);
	if (!(delay >= 0) || delay > 86400 * 7)
		return luaL_error(L, "bad delay (0 to a week, in seconds)");
	due = host.realtime + delay;
	lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_timers);
	lua_createtable(L, 0, 4);
	lua_pushinteger(L, ++nextid);
	lua_setfield(L, -2, "id");
	lua_pushnumber(L, due);
	lua_setfield(L, -2, "due");
	lua_pushnumber(L, every);
	lua_setfield(L, -2, "every");
	lua_pushvalue(L, fnidx);
	lua_setfield(L, -2, thread ? "co" : "fn");
	lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
	lua_pop(L, 1);
	m->numtimers++;
	if (!m->timerdue || due < m->timerdue)
		m->timerdue = due;
	lua_pushinteger(L, nextid);
	return 1;
}

static int tlua_after(lua_State *L)
{
	double delay = luaL_checknumber(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	return tlua_timer_add(L, delay, -1, 2, false);
}

static int tlua_every(lua_State *L)
{
	double every = luaL_checknumber(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	if (!(every >= 0))
		return luaL_error(L, "xt.every: bad interval");
	return tlua_timer_add(L, every, every, 2, false);
}

static int tlua_cancel(lua_State *L)
{
	lua_Integer id = luaL_checkinteger(L, 1);
	lua_Integer i, n;
	lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_timers);
	n = (lua_Integer)lua_rawlen(L, -1);
	for (i = 1; i <= n; i++)
	{
		lua_rawgeti(L, -1, i);
		lua_getfield(L, -1, "id");
		if (lua_tointeger(L, -1) == id)
		{
			lua_pushboolean(L, 1);
			lua_setfield(L, -3, "dead");
			lua_pushboolean(L, 1);
			return 1;
		}
		lua_pop(L, 2);
	}
	lua_pushboolean(L, 0);
	return 1;
}

static int tlua_spawn(lua_State *L)
{
	lua_State *co;
	luaL_checktype(L, 1, LUA_TFUNCTION);
	co = lua_newthread(L);
	lua_pushvalue(L, 1);
	lua_xmove(L, co, 1);
	// remember it as a scheduler thread: xt.sleep only works there
	lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_threads);
	lua_pushvalue(L, -2);
	lua_pushboolean(L, 1);
	lua_rawset(L, -3);
	lua_pop(L, 1);
	return tlua_timer_add(L, 0, -1, lua_gettop(L), true);
}

static int tlua_sleep(lua_State *L)
{
	double sec = luaL_optnumber(L, 1, 0);
	int ours;
	if (!(sec >= 0) || sec > 86400 * 7)
		return luaL_error(L, "xt.sleep: bad delay");
	lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_threads);
	lua_pushthread(L);
	ours = lua_rawget(L, -2) == LUA_TBOOLEAN;
	lua_pop(L, 2);
	if (!ours || !lua_isyieldable(L))
		return luaL_error(L, "xt.sleep: only inside a function started with xt.spawn");
	lua_settop(L, 0);
	lua_pushnumber(L, sec);
	return lua_yield(L, 1);
}

static int tlua_timers_tramp(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	double now = host.realtime, mindue = 0;
	lua_Integer i, n, live = 0;
	int t, fresh;
	lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_timers);
	t = lua_gettop(L);
	n = (lua_Integer)lua_rawlen(L, t);
	for (i = 1; i <= n; i++)
	{
		double due, every;
		int e;
		lua_rawgeti(L, t, i);
		e = lua_gettop(L);
		lua_getfield(L, e, "dead");
		lua_getfield(L, e, "due");
		due = lua_tonumber(L, -1);
		lua_getfield(L, e, "every");
		every = lua_tonumber(L, -1);
		if (lua_toboolean(L, e + 1) || due > now)
		{
			lua_settop(L, e - 1);
			continue;
		}
		lua_settop(L, e);
		if (lua_getfield(L, e, "co") == LUA_TTHREAD)
		{
			lua_State *co = lua_tothread(L, -1);
			int nres = 0, status = lua_resume(co, L, 0, &nres);
			if (m->killed)
				tlua_rethrow(L);
			if (status == LUA_YIELD)
			{
				lua_pushnumber(L, now + (nres > 0 && lua_type(co, -1) == LUA_TNUMBER ? lua_tonumber(co, -1) : 0));
				lua_setfield(L, e, "due");
				lua_pop(co, nres);
			}
			else if (status == LUA_OK)
			{
				lua_pushboolean(L, 1);
				lua_setfield(L, e, "dead");
			}
			else
			{
				lua_xmove(co, L, 1); // the coroutine's error stops the mod, like any error
				return lua_error(L);
			}
		}
		else
		{
			lua_pop(L, 1);
			lua_getfield(L, e, "fn");
			lua_call(L, 0, 1);
			if (every < 0 || (lua_type(L, -1) == LUA_TBOOLEAN && !lua_toboolean(L, -1)))
			{
				lua_pushboolean(L, 1);
				lua_setfield(L, e, "dead");
			}
			else
			{
				lua_pushnumber(L, now + every);
				lua_setfield(L, e, "due");
			}
		}
		lua_settop(L, e - 1);
	}
	// keep the live ones (including any added meanwhile)
	lua_newtable(L);
	fresh = lua_gettop(L);
	n = (lua_Integer)lua_rawlen(L, t);
	for (i = 1; i <= n; i++)
	{
		lua_rawgeti(L, t, i);
		lua_getfield(L, -1, "dead");
		if (!lua_toboolean(L, -1))
		{
			double due;
			lua_getfield(L, -2, "due");
			due = lua_tonumber(L, -1);
			if (!mindue || due < mindue)
				mindue = due;
			lua_pop(L, 2);
			lua_rawseti(L, fresh, ++live);
		}
		else
			lua_pop(L, 2);
	}
	lua_rawsetp(L, LUA_REGISTRYINDEX, &tlua_key_timers);
	m->numtimers = (int)live;
	m->timerdue = live ? mindue : 0;
	return 0;
}

void TLua_RunTimers(tlua_realm_t *r)
{
	int i;
	for (i = 0; i < r->nummods; i++)
	{
		tlua_mod_t *m = &r->mods[i];
		if (m->state == TLUA_RUNNING && m->L && m->timerdue && m->timerdue <= host.realtime)
			TLua_Run(m, tlua_timers_tramp, NULL, 0);
	}
}

static void tlua_openxt(lua_State *L, tlua_mod_t *m)
{
	lua_newtable(L);
	TLua_SetField(L, "on", tlua_on);
	TLua_SetField(L, "command", tlua_command);
	TLua_SetField(L, "cvar", tlua_cvar);
	TLua_SetField(L, "setcvar", tlua_setcvar);
	TLua_SetField(L, "exec", tlua_exec);
	TLua_SetField(L, "realtime", tlua_realtime);
	TLua_SetField(L, "save", tlua_save);
	TLua_SetField(L, "load", tlua_load_saved);
	TLua_SetField(L, "setting", tlua_setting);
	TLua_SetField(L, "after", tlua_after);
	TLua_SetField(L, "every", tlua_every);
	TLua_SetField(L, "cancel", tlua_cancel);
	TLua_SetField(L, "spawn", tlua_spawn);
	TLua_SetField(L, "sleep", tlua_sleep);
	lua_pushstring(L, m->name);
	lua_setfield(L, -2, "modname");
	lua_pushstring(L, m->realm == &tlua_server ? "server" : "client");
	lua_setfield(L, -2, "realm");
	lua_pushstring(L, m->api >= 3 ? "3" : m->api >= 2 ? "2" : "1");
	lua_setfield(L, -2, "apiversion");
	lua_pushinteger(L, TLUA_API_MAX);
	lua_setfield(L, -2, "apimax");
	m->realm->openapi(L);
	TLua_API_Open(L, m);
	lua_setglobal(L, "xt");
}

// ---------------------------------------------------------------------------
// States and calls

static void tlua_nil(lua_State *L, int table, const char *k)
{
	lua_pushnil(L);
	lua_setfield(L, table, k);
}

static void tlua_wrap(lua_State *L, int table, const char *k, lua_CFunction f)
{
	lua_getfield(L, table, k);
	lua_pushcclosure(L, f, 1);
	lua_setfield(L, table, k);
}

// every function of a library through tlua_libcall (the time limit after it)
static void tlua_wraplib(lua_State *L, int g, const char *lib, const char *const *names)
{
	int t;
	lua_getfield(L, g, lib);
	t = lua_gettop(L);
	for (; *names; names++)
		tlua_wrap(L, t, *names, tlua_libcall);
	lua_pop(L, 1);
}

static int tlua_setup(lua_State *L)
{
	static const char *const strfuncs[] = {"byte", "char", "format", "len", "lower", "reverse", "sub", "upper", "pack", "packsize", "unpack", NULL};
	static const char *const tabfuncs[] = {"concat", "insert", "move", "pack", "remove", "sort", "unpack", NULL};
	static const char *const utf8funcs[] = {"char", "codepoint", "codes", "len", "offset", NULL};
	tlua_mod_t *m = (tlua_mod_t *)lua_touserdata(L, 1);
	const char *src;
	size_t len;
	char chunk[64];
	int g;

	luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
	luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
	luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
	luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
	luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
	luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
	lua_settop(L, 1);

	lua_pushglobaltable(L);
	g = lua_gettop(L);
	tlua_nil(L, g, "dofile");
	tlua_nil(L, g, "loadfile");
	tlua_nil(L, g, "warn");
	TLua_SetField(L, "print", tlua_print);
	TLua_SetField(L, "pcall", tlua_pcall);
	TLua_SetField(L, "xpcall", tlua_xpcall);
	TLua_SetField(L, "load", tlua_load);
	TLua_SetField(L, "require", tlua_require);
	TLua_SetField(L, "setmetatable", tlua_setmetatable);
	TLua_SetField(L, "collectgarbage", tlua_collectgarbage);

	lua_getfield(L, g, "coroutine");
	tlua_wrap(L, lua_gettop(L), "resume", tlua_passthrough);
	tlua_wrap(L, lua_gettop(L), "close", tlua_passthrough);
	lua_pop(L, 1);

	// the string table is also the strings' metatable __index: s:rep() too
	lua_getfield(L, g, "string");
	tlua_nil(L, lua_gettop(L), "dump");
	tlua_wrap(L, lua_gettop(L), "find", tlua_strguard);
	tlua_wrap(L, lua_gettop(L), "match", tlua_strguard);
	tlua_wrap(L, lua_gettop(L), "gmatch", tlua_strguard);
	tlua_wrap(L, lua_gettop(L), "gsub", tlua_strguard);
	tlua_wrap(L, lua_gettop(L), "rep", tlua_rep);
	lua_pop(L, 1);
	tlua_wraplib(L, g, "string", strfuncs);
	tlua_wraplib(L, g, "table", tabfuncs);
	tlua_wraplib(L, g, "utf8", utf8funcs);

	// the loaded-libraries table luaL_requiref keeps in the registry holds the
	// originals; replace it with the module cache require() uses
	lua_pushnil(L);
	lua_setfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &tlua_key_loaded);
	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &tlua_key_events);
	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &tlua_key_cmds);
	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &tlua_key_timers);
	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &tlua_key_threads);

	tlua_openxt(L, m);
	lua_settop(L, 1);
	if (luaL_loadbufferx(L, tlua_lib_prelude, sizeof(tlua_lib_prelude) - 1, "=xt.prelude", "t") != LUA_OK)
		return lua_error(L);
	lua_call(L, 0, 0);

	src = tlua_readfile(L, m, "init.lua", &len);
	if (!src)
		return luaL_error(L, "init.lua not found");
	dpsnprintf(chunk, sizeof(chunk), "@%s/init.lua", m->name);
	if (luaL_loadbufferx(L, src, len, chunk, "t") != LUA_OK)
		return lua_error(L);
	lua_call(L, 0, 0);
	return 0;
}

static void tlua_close(tlua_mod_t *m)
{
	if (!m->L)
		return;
	TLua_API_ModStopped(m); // its panels, buttons, pages, channels, requests, entities
	// keep the killed state's hook: a __close handler run by lua_close then
	// raises at once instead of looping
	m->killed = true;
	if (!m->killreason[0])
		dp_strlcpy(m->killreason, "mod stopped", sizeof(m->killreason));
	lua_sethook(m->L, tlua_hook, LUA_MASKCOUNT, 1);
	tlua_current = m;
	tlua_callframe = host.framecount;
	lua_close(m->L);
	tlua_current = NULL;
	m->L = NULL;
	m->mem = 0;
	*tlua_maskof(m) = 0;
	m->timerdue = 0;
	m->numtimers = 0;
	if (m->realm == &tlua_server)
	{
		memset(m->hookmask, 0, sizeof(m->hookmask));
		TLua_Hooks_Changed();
	}
}

static void tlua_fail(tlua_mod_t *m, const char *msg)
{
	char clean[512];
	TLua_CleanText(msg, clean, sizeof(clean), false);
	dp_strlcpy(m->error, clean, sizeof(m->error));
	Con_Printf(CON_ERROR "Lua %s %s stopped: %s\n", TLUA_NOUN(m->realm), m->name, m->error);
	TLua_Log(m, "^1stopped: %s", m->error);
	m->state = TLUA_ERROR;
	tlua_close(m);
}

// Runs f(L) with ud as its argument, under the mod's limits. Returns true when
// it finished; on any error the mod is stopped and marked.
static qbool tlua_run(tlua_mod_t *m, lua_CFunction f, void *ud, qbool loading)
{
	return TLua_Run(m, f, ud, loading ? -1 : 0);
}

// seconds: 0 is lua_time_ms, -1 is loading (10x instructions, 1 s)
qbool TLua_Run(tlua_mod_t *m, lua_CFunction f, void *ud, double seconds)
{
	int status;
	qbool loading = seconds < 0;
	double t0;
	if (TLua_Busy() || !m->L)
		return false;
	// a mod over its per-frame budget (lua_frame_ms) misses its calls for the
	// rest of the frame, so it cannot starve the others; loading and unload
	// always run
	if (!loading && !tlua_nothrottle && TLua_API_Throttled(m))
	{
		TLua_API_Account(m, 0, true);
		return false;
	}
	if (!seconds)
		seconds = bound(1, lua_time_ms.value, 1000) / 1000.0;
	m->insns = (long long)bound(10000, lua_instructions.value, 500000000) * (loading ? 10 : 1);
	m->prints = TLUA_PRINTS_PER_CALL * (loading ? 16 : 1);
	m->execs = 8;
	m->engcmds = 16;
	t0 = Sys_DirtyTime();
	tlua_deadline = t0 + (loading ? 2.0 : seconds);
	tlua_current = m;
	tlua_callframe = host.framecount;
	lua_settop(m->L, 0);
	lua_pushcfunction(m->L, f);
	lua_pushlightuserdata(m->L, ud);
	status = lua_pcall(m->L, 1, 0, 0);
	tlua_current = NULL;
	TLua_API_Account(m, Sys_DirtyTime() - t0, false);
	if (m->mem > m->mempeak)
		m->mempeak = m->mem;
	if (status != LUA_OK || m->killed)
	{
		char msg[512];
		if (m->killed)
			dpsnprintf(msg, sizeof(msg), "%s", m->killreason);
		else if (lua_type(m->L, -1) == LUA_TSTRING)
			dp_strlcpy(msg, lua_tostring(m->L, -1), sizeof(msg));
		else
			dpsnprintf(msg, sizeof(msg), "error object is a %s", luaL_typename(m->L, -1));
		tlua_fail(m, msg);
		tlua_updatestatus(m->realm);
		return false;
	}
	lua_settop(m->L, 0);
	return true;
}

static int tlua_setup_tramp(lua_State *L)
{
	return tlua_setup(L);
}

// ---------------------------------------------------------------------------
// Crash-loop marker: "loading <mod>" is on disk while a mod loads. If the
// engine dies or hangs there, the next start finds it, keeps that mod off and
// says why (tlua_check_marker). It is <userdir>/luastate/<realm>/loading.txt,
// written durably (fsync) before init.lua runs and removed once loading
// ended either way. On the web a write to the userdir only reaches IndexedDB
// after the frame yields, which a hang never does, so the marker also goes to
// localStorage, which is written synchronously.

#ifdef __EMSCRIPTEN__
static void tlua_webmarker(tlua_realm_t *r, const char *value)
{
	char key[64];
	dpsnprintf(key, sizeof(key), "godnotic.lua.loading.%s", r->subdir);
	EM_ASM({
		try {
			if ($1) localStorage.setItem(UTF8ToString($0), UTF8ToString($1));
			else localStorage.removeItem(UTF8ToString($0));
		} catch (e) {}
	}, key, value);
}
#endif

static void tlua_marker_set(tlua_mod_t *m)
{
	char dir[1024];
	tlua_statedir(m->realm, dir, sizeof(dir));
	if (!tlua_writestate(dir, "loading.txt", m->name, true))
		Con_Printf(CON_WARN "Lua: could not write %sloading.txt\n", dir);
#ifdef __EMSCRIPTEN__
	tlua_webmarker(m->realm, m->name);
#endif
}

static void tlua_marker_clear(tlua_realm_t *r)
{
	char path[1024];
	tlua_statedir(r, path, sizeof(path));
	dp_strlcat(path, "loading.txt", sizeof(path));
	remove(path);
#ifdef __EMSCRIPTEN__
	tlua_webmarker(r, NULL);
#endif
}

// the mod named by the marker, or "" when there is none
static void tlua_marker_get(tlua_realm_t *r, char *name, size_t namesize)
{
	char path[1024], buf[64];
	name[0] = 0;
#ifdef __EMSCRIPTEN__
	{
		char key[64];
		char *v;
		dpsnprintf(key, sizeof(key), "godnotic.lua.loading.%s", r->subdir);
		v = (char *)(size_t)EM_ASM_INT({
			try {
				var v = localStorage.getItem(UTF8ToString($0));
				return v ? stringToNewUTF8(v) : 0;
			} catch (e) { return 0; }
		}, key);
		if (v)
		{
			dp_strlcpy(name, v, namesize);
			free(v);
			name[strcspn(name, "\r\n")] = 0;
			if (name[0])
				return;
		}
	}
#endif
	tlua_statedir(r, path, sizeof(path));
	dp_strlcat(path, "loading.txt", sizeof(path));
	if (tlua_readstate(path, buf, sizeof(buf)) > 0)
	{
		buf[strcspn(buf, "\r\n")] = 0;
		dp_strlcpy(name, buf, namesize);
	}
}

static void tlua_writeenabled(tlua_realm_t *r);
static void tlua_stop(tlua_mod_t *m);
static int tlua_cmpmod(const void *a, const void *b);
static qbool tlua_ismodfolder(const char *dir, const char *name);

// at the realm's first load: a mod that was loading when the engine went down
// is switched off (enabled.txt too), with the reason in its error text
static void tlua_check_marker(tlua_realm_t *r)
{
	char name[TLUA_NAME_LEN + 32];
	tlua_mod_t *m;
	tlua_marker_get(r, name, sizeof(name));
	if (!name[0])
		return;
	tlua_marker_clear(r);
	if (!(m = TLua_Find(r, name)))
		return;
	m->enabled = false;
	tlua_writeenabled(r);
	m->state = TLUA_OFF;
	m->crashed = true;
	dp_strlcpy(m->error, "did not finish loading last time: turned off, turn it on again to retry", sizeof(m->error));
	TLua_Log(m, "^3%s", m->error);
	Con_Printf(CON_WARN "Lua %s %s %s\n", TLUA_NOUN(r), m->name, m->error);
}

static void tlua_start(tlua_mod_t *m)
{
	lua_State *L;
	if (m->L || TLua_Busy())
		return;
	if (m->reserved)
	{
		tlua_fail(m, "the name is reserved: it would share a namespace with the engine's commands or settings; rename the folder");
		return;
	}
	if (m->manifest_error)
	{
		tlua_fail(m, "invalid or unsupported Lua API manifest");
		return;
	}
	m->killed = false;
	m->killreason[0] = 0;
	m->error[0] = 0;
	m->mem = 0;
	m->owncvars = -1;
	m->memlimit = (size_t)bound(1, lua_memory_mb.integer, 256) << 20;
	m->mempeak = 0;
	memset(m->tiercalls, 0, sizeof(m->tiercalls));
	m->hosts[0] = 0;
	m->engcmds_used = 0;
	*tlua_maskof(m) = 0;
	L = lua_newstate(tlua_alloc, m);
	if (!L)
	{
		tlua_fail(m, "could not create a Lua state");
		return;
	}
	*(tlua_mod_t **)lua_getextraspace(L) = m;
	lua_atpanic(L, tlua_panic);
	lua_sethook(L, tlua_hook, LUA_MASKCOUNT, TLUA_HOOK_STEP);
	m->L = L;
	m->state = TLUA_RUNNING;
	tlua_marker_set(m);
	if (tlua_run(m, tlua_setup_tramp, m, true))
		Con_Printf("Lua %s %s loaded (%i KB)\n", TLUA_NOUN(m->realm), m->name, (int)(m->mem >> 10));
	tlua_marker_clear(m->realm);
}

typedef struct tlua_fire_s
{
	const char *event;
	tlua_push_f push;
	tlua_result_f result;
	void *ud;
	tlua_mod_t *m;
} tlua_fire_t;

static int tlua_fire_tramp(lua_State *L)
{
	tlua_fire_t *f = (tlua_fire_t *)lua_touserdata(L, 1);
	lua_Integer i, n;
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_events) != LUA_TTABLE)
		return 0;
	lua_pushstring(L, f->event);
	if (lua_rawget(L, -2) != LUA_TTABLE)
		return 0;
	n = (lua_Integer)lua_rawlen(L, -1);
	for (i = 1; i <= n; i++)
	{
		int nargs;
		if (lua_rawgeti(L, -1, i) != LUA_TFUNCTION)
		{
			lua_pop(L, 1);
			continue;
		}
		nargs = f->push ? f->push(L, f->ud) : 0;
		lua_call(L, nargs, 1);
		if (f->result)
			f->result(L, f->m, f->ud);
		lua_pop(L, 1);
	}
	return 0;
}

static void tlua_fire_one(tlua_mod_t *m, const char *event, tlua_push_f push, tlua_result_f result, void *ud)
{
	tlua_fire_t f;
	int idx = tlua_eventindex(m->realm, event);
	if (m->state != TLUA_RUNNING || !m->L || idx < 0 || !tlua_has(m, TLUA_PERM_EVENTS) || !(*tlua_maskof(m) & (1ull << idx)))
		return;
	f.event = event;
	f.push = push;
	f.result = result;
	f.ud = ud;
	f.m = m;
	tlua_run(m, tlua_fire_tramp, &f, false);
}

void TLua_Fire(tlua_realm_t *r, const char *event, tlua_push_f push, tlua_result_f result, void *ud)
{
	int i;
	for (i = 0; i < r->nummods; i++)
		tlua_fire_one(&r->mods[i], event, push, result, ud);
}

// Callbacks a mod handed to the engine (a button's press, a page's change, a
// channel handler, an HTTP answer) are kept as registry references and
// called with TLua_CallRef, under the same limits as an event.
int TLua_Ref(lua_State *L, int idx)
{
	lua_pushvalue(L, idx);
	return luaL_ref(L, LUA_REGISTRYINDEX);
}

void TLua_Unref(tlua_mod_t *m, int ref)
{
	if (m->L && ref > 0)
		luaL_unref(m->L, LUA_REGISTRYINDEX, ref);
}

typedef struct tlua_refcall_s
{
	int ref;
	int (*push)(lua_State *L, void *ud);
	void (*result)(lua_State *L, void *ud);
	void *ud;
} tlua_refcall_t;

static int tlua_refcall_tramp(lua_State *L)
{
	tlua_refcall_t *c = (tlua_refcall_t *)lua_touserdata(L, 1);
	int n;
	if (lua_rawgeti(L, LUA_REGISTRYINDEX, c->ref) != LUA_TFUNCTION)
		return 0;
	n = c->push ? c->push(L, c->ud) : 0;
	lua_call(L, n, 1);
	if (c->result)
		c->result(L, c->ud);
	return 0;
}

qbool TLua_CallRef(tlua_mod_t *m, int ref, int (*push)(lua_State *L, void *ud), void *ud, void (*result)(lua_State *L, void *ud))
{
	tlua_refcall_t c;
	if (m->state != TLUA_RUNNING || !m->L || ref <= 0)
		return false;
	c.ref = ref;
	c.push = push;
	c.result = result;
	c.ud = ud;
	return tlua_run(m, tlua_refcall_tramp, &c, false);
}

qbool TLua_AnyHandler(tlua_realm_t *r, const char *event)
{
	int i, idx = tlua_eventindex(r, event);
	if (idx < 0)
		return false;
	for (i = 0; i < r->nummods; i++)
		if (r->mods[i].state == TLUA_RUNNING && tlua_has(&r->mods[i], TLUA_PERM_EVENTS) && (*tlua_maskof(&r->mods[i]) & (1ull << idx)))
			return true;
	return false;
}

typedef struct tlua_cmdcall_s
{
	const char *cmd;
	cmd_state_t *c;
} tlua_cmdcall_t;

static int tlua_cmd_tramp(lua_State *L)
{
	tlua_cmdcall_t *cc = (tlua_cmdcall_t *)lua_touserdata(L, 1);
	int i, argc = Cmd_Argc(cc->c);
	lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_cmds);
	lua_pushstring(L, cc->cmd);
	if (lua_rawget(L, -2) != LUA_TFUNCTION)
		return 0;
	for (i = 1; i < argc && i <= 16; i++)
		lua_pushstring(L, Cmd_Argv(cc->c, i));
	lua_call(L, i - 1, 0);
	return 0;
}

static void tlua_modcmd_f(cmd_state_t *cmd)
{
	const char *full = Cmd_Argv(cmd, 0);
	int i;
	for (i = 0; i < tlua_numcmds; i++)
	{
		tlua_cmd_t *c = &tlua_cmds[i];
		tlua_mod_t *m;
		tlua_cmdcall_t cc;
		if (strcmp(c->full, full))
			continue;
		if (cmd->source != src_local || (!c->fromserver && TLua_FromServer()))
		{
			Con_Printf("%s: refused, it may come from a server (a mod can allow that with xt.command's fromserver option)\n", full);
			return;
		}
		if (TLua_Busy())
			return;
		m = TLua_Find(c->realm, c->mod);
		if (!m || m->state != TLUA_RUNNING || !tlua_has(m, TLUA_PERM_COMMANDS))
		{
			Con_Printf("%s: Lua %s %s is not running\n", full, TLUA_NOUN(c->realm), c->mod);
			return;
		}
		cc.cmd = c->cmd;
		cc.c = cmd;
		tlua_run(m, tlua_cmd_tramp, &cc, false);
		return;
	}
}

// ---------------------------------------------------------------------------
// The mod manager

// Names a mod may not have: <prefix><name>_<x> would share the namespace of
// the manager's own commands and cvars (lua_mod_enable, lua_memory_mb,
// sv_lua_mod_info), or of any other engine command or cvar.
static qbool tlua_reservedname(tlua_realm_t *r, const char *name)
{
	static const char *const reserved[] = {"mod", "mods", "plugin", "plugins", "memory", "time", "instructions", "enable", "status", "permission", "permissions", NULL};
	char prefix[96];
	size_t n;
	cvar_t *v;
	cmd_function_t *f;
	int i;
	for (i = 0; reserved[i]; i++)
		if (!strcmp(name, reserved[i]))
			return true;
	dpsnprintf(prefix, sizeof(prefix), "%s%s_", r->cmdprefix, name);
	n = strlen(prefix);
	for (v = cvars_all.vars; v; v = v->next)
		if ((v->flags & CF_REGISTERED) && !strncmp(v->name, prefix, n))
			return true;
	for (f = cmd_local->engine_functions; f; f = f->next)
		if (f->function != tlua_modcmd_f && !strncmp(f->name, prefix, n))
			return true;
	return false;
}

static void tlua_manifest_bad(tlua_mod_t *m, const char *fmt, ...) DP_FUNC_PRINTF(2);
static void tlua_manifest_bad(tlua_mod_t *m, const char *fmt, ...)
{
	va_list ap;
	char text[256];
	if (m->manifest_error)
		return; // the first reason is the one to fix first
	va_start(ap, fmt);
	dpvsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	m->manifest_error = true;
	TLua_CleanText(text, m->manifest_why, sizeof(m->manifest_why), false);
}

// a tier by name, or the tier an API 1/2 scope name belongs to; *api is the
// API level that name needs
static const tlua_permission_t *tlua_findperm_api(const char *name, int *api)
{
	int i, j;
	for (i = 0; tlua_permissions[i].name; i++)
		if (!strcmp(name, tlua_permissions[i].name))
		{
			if (api) *api = tlua_permissions[i].api;
			return &tlua_permissions[i];
		}
	for (j = 0; tlua_scope_aliases[j].name; j++)
		if (!strcmp(name, tlua_scope_aliases[j].name))
			for (i = 0; tlua_permissions[i].name; i++)
				if (tlua_permissions[i].bit == tlua_scope_aliases[j].bit)
				{
					if (api) *api = tlua_scope_aliases[j].api;
					return &tlua_permissions[i];
				}
	return NULL;
}

static const tlua_permission_t *tlua_findperm(const char *name)
{
	return tlua_findperm_api(name, NULL);
}

static qbool tlua_isnumber(const char *s)
{
	char *end;
	double d;
	if (!*s)
		return false;
	d = strtod(s, &end);
	return !*end && d == d;
}

// setting.<name>: <type> <default> [args] | <label>
static void tlua_parsesetting(tlua_mod_t *m, const char *name, char *v)
{
	tlua_setting_t *st;
	char *bar = strchr(v, '|'), *tok, *rest;
	char spec[256];
	if (m->numsettings >= TLUA_MAX_SETTINGS)
	{
		tlua_manifest_bad(m, "more than %d settings", TLUA_MAX_SETTINGS);
		return;
	}
	if (!tlua_validsuffix(name))
	{
		tlua_manifest_bad(m, "setting name '%s': a-z and 0-9 only", name);
		return;
	}
	st = &m->settings[m->numsettings];
	memset(st, 0, sizeof(*st));
	dp_strlcpy(st->name, name, sizeof(st->name));
	if (bar)
	{
		*bar = 0;
		for (rest = bar + 1; *rest == ' '; rest++)
			;
		TLua_CleanText(rest, st->label, sizeof(st->label), false);
	}
	else
		dp_strlcpy(st->label, name, sizeof(st->label));
	dp_strlcpy(spec, v, sizeof(spec));
	tok = strtok(spec, " \t");
	if (tok && (!strcmp(tok, "int") || !strcmp(tok, "float")))
		; // number with a whole-number step (int) or not: kept as written
	else if (!tok || (strcmp(tok, "bool") && strcmp(tok, "number") && strcmp(tok, "choice") && strcmp(tok, "text") && strcmp(tok, "color") && strcmp(tok, "key")))
	{
		tlua_manifest_bad(m, "setting %s: the type is bool, number, int, float, choice, text, color or key", name);
		return;
	}
	dp_strlcpy(st->type, tok, sizeof(st->type));
	tok = strtok(NULL, " \t");
	if (!tok || !tlua_cleanvalue(tok))
	{
		tlua_manifest_bad(m, "setting %s: no default value", name);
		return;
	}
	dp_strlcpy(st->def, tok, sizeof(st->def));
	rest = strtok(NULL, "");
	while (rest && (*rest == ' ' || *rest == '\t'))
		rest++;
	if (rest)
	{
		char *e = rest + strlen(rest);
		while (e > rest && (e[-1] == ' ' || e[-1] == '\t'))
			*--e = 0;
		if (!tlua_cleanvalue(rest))
		{
			tlua_manifest_bad(m, "setting %s: bad characters", name);
			return;
		}
		dp_strlcpy(st->args, rest, sizeof(st->args));
	}
	if (!strcmp(st->type, "bool") && strcmp(st->def, "0") && strcmp(st->def, "1"))
		tlua_manifest_bad(m, "setting %s: a bool's default is 0 or 1", name);
	else if (!strcmp(st->type, "color") && (strlen(st->def) != 6 || strspn(st->def, "0123456789abcdefABCDEF") != 6))
		tlua_manifest_bad(m, "setting %s: a color's default is six hex digits (ff8800)", name);
	else if (!strcmp(st->type, "key") && Key_StringToKeynum(st->def) < 0)
		tlua_manifest_bad(m, "setting %s: a key's default is a key name (F5, JOY3, SPACE)", name);
	else if (!strcmp(st->type, "number") || !strcmp(st->type, "int") || !strcmp(st->type, "float"))
	{
		char a[3][32];
		int n = sscanf(st->args, "%31s %31s %31s", a[0], a[1], a[2]);
		if (!tlua_isnumber(st->def) || n < 2 || !tlua_isnumber(a[0]) || !tlua_isnumber(a[1]) || atof(a[0]) > atof(a[1]) || (n == 3 && !tlua_isnumber(a[2])))
			tlua_manifest_bad(m, "setting %s: number <default> <min> <max> [step]", name);
	}
	else if (!strcmp(st->type, "choice"))
	{
		// the default must be one of the options
		char opts[128], *o;
		qbool found = false;
		int n = 0;
		dp_strlcpy(opts, st->args, sizeof(opts));
		for (o = strtok(opts, " \t"); o; o = strtok(NULL, " \t"))
		{
			n++;
			if (!strcmp(o, st->def))
				found = true;
		}
		if (!found || n < 2 || n > 8)
			tlua_manifest_bad(m, "setting %s: choice <default> <2 to 8 options, the default among them>", name);
	}
	else if (!strcmp(st->type, "text") && st->args[0])
	{
		// "text hello world": the rest belongs to the default
		dp_strlcat(st->def, " ", sizeof(st->def));
		dp_strlcat(st->def, st->args, sizeof(st->def));
		st->args[0] = 0;
	}
	if (!m->manifest_error)
		m->numsettings++;
}

// mod.txt, already read into text (which is modified). docs/PLUGIN_STANDARD.md
void TLua_ParseManifest(tlua_mod_t *m, char *text)
{
	char *line, *next;
	char perms[512] = "";
	qbool haveperms = false;
	int i;
	for (line = text; line; line = next)
	{
		char *colon, *v, *e, *k;
		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		while (*line == ' ' || *line == '\t')
			line++;
		if (line[0] == '#' || strlen(line) >= 511 || !(colon = strchr(line, ':')))
			continue;
		*colon = 0;
		for (e = colon; e > line && (e[-1] == ' ' || e[-1] == '\t'); e--)
			e[-1] = 0;
		k = line;
		for (v = colon + 1; *v == ' ' || *v == '\t'; v++)
			;
		for (e = v + strlen(v); e > v && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'); e--)
			*(e - 1) = 0;
		if (!strcmp(k, "title"))
			TLua_CleanText(v, m->title, sizeof(m->title), false);
		else if (!strcmp(k, "version"))
			TLua_CleanText(v, m->version, sizeof(m->version), false);
		else if (!strcmp(k, "author"))
			TLua_CleanText(v, m->author, sizeof(m->author), false);
		else if (!strcmp(k, "description"))
			TLua_CleanText(v, m->description, sizeof(m->description), false);
		else if (!strcmp(k, "homepage"))
			TLua_CleanText(v, m->homepage, sizeof(m->homepage), false);
		else if (!strcmp(k, "update"))
		{
			if (strncmp(v, "https://", 8) || strlen(v) >= sizeof(m->updateurl) || strpbrk(v, " \"';$\\"))
				tlua_manifest_bad(m, "update: must be an https:// URL");
			else
				dp_strlcpy(m->updateurl, v, sizeof(m->updateurl));
		}
		else if (!strcmp(k, "http"))
		{
			// the hosts xt.http may reach: "example.org, api.example.org"
			const char *c;
			for (c = v; *c; c++)
				if (!isalnum((unsigned char)*c) && !strchr(".-_:, ", *c))
					break;
			if (*c || strlen(v) >= sizeof(m->httphosts))
				tlua_manifest_bad(m, "http: host names, comma separated");
			else
				dp_strlcpy(m->httphosts, v, sizeof(m->httphosts));
		}
		else if (!strcmp(k, "id"))
		{
			if (!TLua_ValidModName(v))
				tlua_manifest_bad(m, "id '%s': a-z, 0-9 and _, up to %d", v, TLUA_NAME_LEN);
			else
				dp_strlcpy(m->id, v, sizeof(m->id));
		}
		else if (!strcmp(k, "api"))
		{
			if (m->api || !tlua_isnumber(v) || atoi(v) < 1 || (double)atoi(v) != atof(v))
				tlua_manifest_bad(m, "api: '%s' is not an API level", v);
			else if (atoi(v) > TLUA_API_MAX)
				tlua_manifest_bad(m, "needs API %d; this game runs API 1 to %d (update the game)", atoi(v), TLUA_API_MAX);
			m->api = atoi(v);
		}
		else if (!strcmp(k, "realm"))
		{
			if (!strcmp(v, "client")) m->realmdecl = TLUA_REALM_CLIENT;
			else if (!strcmp(v, "server")) m->realmdecl = TLUA_REALM_SERVER;
			else if (!strcmp(v, "both")) m->realmdecl = TLUA_REALM_BOTH;
			else tlua_manifest_bad(m, "realm: client, server or both");
		}
		else if (!strcmp(k, "main"))
		{
			if (strcmp(v, "init.lua"))
				tlua_manifest_bad(m, "main: only init.lua in this API");
		}
		else if (!strcmp(k, "permissions"))
		{
			haveperms = true;
			dp_strlcpy(perms, v, sizeof(perms));
		}
		else if (!strcmp(k, "icon"))
		{
			// a picture of the package for the Mods lists: .png or .tga, a path in it
			size_t n = strlen(v);
			const char *c;
			for (c = v; *c; c++)
				if (!isalnum((unsigned char)*c) && !strchr("/_-.", *c))
					break;
			if (*c || n < 5 || n >= sizeof(m->icon) || strstr(v, "..") || v[0] == '/' || v[0] == '.'
				|| (strcasecmp(v + n - 4, ".png") && strcasecmp(v + n - 4, ".tga")))
				tlua_manifest_bad(m, "icon: a .png or .tga file of the package (icon.png)");
			else
				dp_strlcpy(m->icon, v, sizeof(m->icon));
		}
		else if (!strncmp(k, "reason.", 7))
		{
			const tlua_permission_t *p = tlua_findperm(k + 7);
			if (!p)
				tlua_manifest_bad(m, "%s: no such permission", k);
			else if (!m->reasons[p - tlua_permissions][0]) // the first reason given for a tier is shown
				TLua_CleanText(v, m->reasons[p - tlua_permissions], sizeof(m->reasons[0]), false);
		}
		else if (!strncmp(k, "setting.", 8) && strchr(k + 8, '.'))
		{
			// setting.<name>.desc / setting.<name>.group, after the setting itself
			char sname[TLUA_SUFFIX_LEN + 1];
			const char *field = strchr(k + 8, '.') + 1;
			int j;
			size_t slen = min(sizeof(sname) - 1, (size_t)(field - 1 - (k + 8)));
			memcpy(sname, k + 8, slen);
			sname[slen] = 0;
			for (j = 0; j < m->numsettings; j++)
				if (!strcmp(m->settings[j].name, sname))
					break;
			if (j == m->numsettings)
				tlua_manifest_bad(m, "%s: comes before setting.%s, or there is none", k, sname);
			else if (!strcmp(field, "desc"))
				TLua_CleanText(v, m->settings[j].desc, sizeof(m->settings[j].desc), false);
			else if (!strcmp(field, "group"))
				TLua_CleanText(v, m->settings[j].group, sizeof(m->settings[j].group), false);
		}
		else if (!strncmp(k, "setting.", 8))
			tlua_parsesetting(m, k + 8, v);
		// other keys are ignored: a newer standard may add some
	}
	m->scoped = m->api > 0;
	m->declares = haveperms;
	if (haveperms)
	{
		char *token;
		if (!m->scoped)
			tlua_manifest_bad(m, "permissions: needs an api: line");
		for (token = strtok(perms, ", \t"); token; token = strtok(NULL, ", \t"))
		{
			int api = 0;
			const tlua_permission_t *p = tlua_findperm_api(token, &api);
			if (!p)
				tlua_manifest_bad(m, "unknown permission '%s'", token);
			else if (api > m->api)
				tlua_manifest_bad(m, "permission %s needs api: %d", token, api);
			else
				m->requested |= p->bit;
		}
	}
	if (m->id[0] && strcmp(m->id, m->name))
		tlua_manifest_bad(m, "id '%s' does not match the folder name '%s'", m->id, m->name);
	if (m->realm && m->realmdecl == TLUA_REALM_SERVER && m->realm == &tlua_client)
		tlua_manifest_bad(m, "this is a server mod: it belongs in plugins/");
	if (m->realm && m->realmdecl == TLUA_REALM_CLIENT && m->realm == &tlua_server)
		tlua_manifest_bad(m, "this is a client mod: it belongs in mods/");
	for (i = 0; i < TLUA_NUMPERMS; i++)
		if (m->reasons[i][0] && !(m->requested & tlua_permissions[i].bit) && m->scoped)
			m->reasons[i][0] = 0; // a reason for something not requested says nothing
	// every mod gets the standard tier; a legacy one (no "api:") nothing more
	m->requested |= TLUA_TIER_STANDARD;
	if (!m->scoped)
		m->requested = TLUA_TIER_STANDARD;
	if (m->manifest_error)
		m->requested = 0;
}

static void tlua_readinfo(tlua_mod_t *m)
{
	char dir[1024], rel[64];
	long n;
	m->title[0] = m->version[0] = m->author[0] = m->description[0] = 0;
	m->id[0] = m->homepage[0] = m->updateurl[0] = m->manifest_why[0] = 0;
	m->httphosts[0] = 0;
	m->icon[0] = 0;
	m->nomanifest = m->declares = false;
	memset(m->reasons, 0, sizeof(m->reasons));
	m->numsettings = 0;
	m->api = 0;
	m->realmdecl = TLUA_REALM_ANY;
	m->scoped = m->manifest_error = false;
	m->requested = 0;
	tlua_dir(m->realm, dir, sizeof(dir));
	dpsnprintf(rel, sizeof(rel), "%s/mod.txt", m->name);
	n = tlua_slurp(dir, rel);
	if (n < 0)
	{
		if (n == -1 && errno == ENOENT)
		{
			// a folder with only init.lua (copied in by hand while trying something):
			// it declares nothing, so it runs with the standard tier and nothing more
			// can ever be granted to it; a package needs a mod.txt (touch_lua_pkg.c)
			m->requested = TLUA_TIER_STANDARD;
			m->nomanifest = true;
		}
		else tlua_manifest_bad(m, "mod.txt is unreadable, over 1 MB, or not a regular file");
		return;
	}
	TLua_ParseManifest(m, tlua_filebuf);
}

// the cvars of the settings mod.txt declares, created with their defaults
// (archived: they are the player's choices)
static void tlua_settings_cvars(tlua_mod_t *m)
{
	int i;
	for (i = 0; i < m->numsettings; i++)
	{
		tlua_setting_t *st = &m->settings[i];
		char name[96], desc[160];
		cvar_t *v;
		dpsnprintf(name, sizeof(name), "%s%s_%s", m->realm->cmdprefix, m->name, st->name);
		v = Cvar_FindVar(&cvars_all, name, ~0);
		if (v)
		{
			// the player's value (from config.cfg, or set before the mod was
			// seen) stays; Cvar_Get would put the default back
			if (v->flags & CF_ALLOCATED)
				v->flags |= CF_ARCHIVE;
			continue;
		}
		if (Cmd_Exists(cmd_local, name))
			continue;
		dpsnprintf(desc, sizeof(desc), "setting of Lua %s %s: %s", TLUA_NOUN(m->realm), m->name, st->label);
		Cvar_Get(&cvars_all, name, st->def, m->realm->cmdflags | CF_ARCHIVE, desc);
	}
}

static void tlua_grantpath(tlua_mod_t *m, char *dir, size_t dirsize, char *name, size_t namesize)
{
	tlua_statedir(m->realm, dir, dirsize);
	dp_strlcat(dir, "grants/", dirsize);
	dpsnprintf(name, namesize, "%s.txt", m->name);
}

// "v1 <hex>" / "legacy <hex>": only the optional tiers in it count (bits 0-11
// were the API 1/2 standard scopes, which are always on now); a malformed file
// grants nothing beyond standard
static void tlua_readgrants(tlua_mod_t *m)
{
	char dir[1024], name[64], path[1100], line[128], extra, kind[16];
	unsigned mask;
	m->granted = TLUA_TIER_STANDARD;
	tlua_grantpath(m, dir, sizeof(dir), name, sizeof(name));
	dpsnprintf(path, sizeof(path), "%s%s", dir, name);
	if (tlua_readstate(path, line, sizeof(line)) < 0)
		return;
	if (sscanf(line, "%15s %x %c", kind, &mask, &extra) == 2 && (!strcmp(kind, "v1") || !strcmp(kind, "legacy")) && !(mask & ~((1u << 15) - 1)))
		m->granted |= mask & TLUA_PERM_OPTIONAL;
}

// luastate/<realm>/seen/<id>.txt: "<hex>", the tiers the player was shown
// (on turning the mod on, or deciding in its permission switches). Tiers a
// mod declares beyond these are new: they stay off and the UIs ask about
// them. No file (installs from before): what was granted counts as seen.
static void tlua_seenpath(tlua_mod_t *m, char *dir, size_t dirsize, char *name, size_t namesize)
{
	tlua_statedir(m->realm, dir, dirsize);
	dp_strlcat(dir, "seen/", dirsize);
	dpsnprintf(name, namesize, "%s.txt", m->name);
}

static void tlua_readseen(tlua_mod_t *m)
{
	char dir[1024], name[64], path[1100], line[64];
	unsigned mask;
	tlua_seenpath(m, dir, sizeof(dir), name, sizeof(name));
	dpsnprintf(path, sizeof(path), "%s%s", dir, name);
	if (tlua_readstate(path, line, sizeof(line)) > 0 && sscanf(line, "%x", &mask) == 1)
		m->seen = (mask & TLUA_PERM_OPTIONAL) | TLUA_TIER_STANDARD;
	else
		m->seen = m->granted | TLUA_TIER_STANDARD;
}

static void tlua_markseen(tlua_mod_t *m)
{
	char dir[1024], name[64], text[32];
	m->seen = m->requested | TLUA_TIER_STANDARD;
	tlua_seenpath(m, dir, sizeof(dir), name, sizeof(name));
	dpsnprintf(text, sizeof(text), "%x\n", m->seen & TLUA_PERM_OPTIONAL);
	tlua_writestate(dir, name, text, true);
}

// tiers it declares that the player has not been shown yet
static unsigned tlua_unseen(const tlua_mod_t *m)
{
	return m->requested & TLUA_PERM_OPTIONAL & ~m->seen;
}

static void tlua_tierlist(unsigned mask, char *out, size_t size)
{
	int i;
	out[0] = 0;
	for (i = 0; tlua_permissions[i].name; i++)
		if (mask & tlua_permissions[i].bit)
		{
			if (out[0])
				dp_strlcat(out, ",", size);
			dp_strlcat(out, tlua_permissions[i].name, size);
		}
}

static void tlua_readenabled(tlua_realm_t *r)
{
	char path[2048], buf[TLUA_MAX_MODS * 64 + 1024], *line, *next;
	int i;
	for (i = 0; i < r->nummods; i++)
		r->mods[i].enabled = false;
	tlua_dir(r, path, sizeof(path));
	dp_strlcat(path, "enabled.txt", sizeof(path));
	if (tlua_readstate(path, buf, sizeof(buf)) < 0)
		return;
	for (line = buf; line; line = next)
	{
		tlua_mod_t *m;
		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		line[strcspn(line, "\r \t")] = 0;
		if ((m = TLua_Find(r, line)))
			m->enabled = true;
	}
}

// mods/enabled.txt stays next to the folders (the launcher reads and writes
// it too); it is written through a temporary file and rename, never through
// a symlink, and no mod can write there (tlua_open_confined)
static void tlua_writeenabled(tlua_realm_t *r)
{
	char dir[1024], text[TLUA_MAX_MODS * (TLUA_NAME_LEN + 2) + 1];
	int i;
	text[0] = 0;
	tlua_dir(r, dir, sizeof(dir));
	for (i = 0; i < r->nummods; i++)
		if (r->mods[i].enabled)
		{
			dp_strlcat(text, r->mods[i].name, sizeof(text));
			dp_strlcat(text, "\n", sizeof(text));
		}
	if (!tlua_writestate(dir, "enabled.txt", text, false))
		Con_Printf(CON_WARN "Lua: could not write %senabled.txt\n", dir);
}

static const char *tlua_statename(const tlua_mod_t *m)
{
	if (m->state == TLUA_RUNNING)
		return "running";
	if (m->state == TLUA_ERROR)
		return "error";
	if (m->crashed)
		return "unfinished"; // did not finish loading last time
	return m->enabled ? "enabled" : "off";
}

static void tlua_detail_add(char *buf, size_t size, const char *key, const char *value)
{
	char clean[1024], *p;
	dp_strlcat(buf, key, size);
	dp_strlcat(buf, "\t", size);
	// fields of a perm, setting or command line are tab separated: keep the
	// tabs, every other control character goes
	dp_strlcpy(clean, value, sizeof(clean));
	for (p = clean; *p; p++)
		if (*p != '\t' && ((unsigned char)*p < 32 || *p == 127))
			*p = ' ';
	dp_strlcat(buf, clean, size);
	dp_strlcat(buf, "\n", size);
}

static qbool tlua_fromurl(tlua_mod_t *m);

// _lua_detail: the mod _lua_select names, for the Mods dialog's detail view.
// key<TAB>value lines; perm, setting and command lines have more fields.
static void tlua_publish_detail(void)
{
	static char buf[32768];
	char sel[64], line[1024];
	const char *name;
	tlua_realm_t *r;
	tlua_mod_t *m;
	int i, j;
	if (!tlua_inited || cls.state == ca_dedicated)
		return;
	tlua_detail_dirty = false;
	dp_strlcpy(sel, lua_select.string, sizeof(sel));
	if (!strncmp(sel, "mod ", 4))
		r = &tlua_client, name = sel + 4;
	else if (!strncmp(sel, "plugin ", 7))
		r = &tlua_server, name = sel + 7;
	else
		r = NULL, name = "";
	buf[0] = 0;
	if (!r || !(m = TLua_Find(r, name)))
	{
		if (strcmp(lua_detail.string, ""))
			Cvar_SetQuick(&lua_detail, "");
		return;
	}
	tlua_detail_add(buf, sizeof(buf), "name", m->name);
	tlua_detail_add(buf, sizeof(buf), "realm", r->kind);
	tlua_detail_add(buf, sizeof(buf), "state", tlua_statename(m));
	tlua_detail_add(buf, sizeof(buf), "title", m->title[0] ? m->title : m->name);
	tlua_detail_add(buf, sizeof(buf), "version", m->version);
	tlua_detail_add(buf, sizeof(buf), "author", m->author);
	tlua_detail_add(buf, sizeof(buf), "homepage", m->homepage);
	tlua_detail_add(buf, sizeof(buf), "description", m->description);
	tlua_detail_add(buf, sizeof(buf), "api", m->manifest_error ? "invalid" : m->api ? va(line, sizeof(line), "%d", m->api) : "legacy");
	tlua_detail_add(buf, sizeof(buf), "manifest", m->manifest_why);
	tlua_detail_add(buf, sizeof(buf), "error", m->error);
	tlua_detail_add(buf, sizeof(buf), "enabled", m->enabled ? "1" : "0");
	tlua_detail_add(buf, sizeof(buf), "memory", m->L ? va(line, sizeof(line), "%i KB", (int)(m->mem >> 10)) : "");
	// installed from a link: engine_full needs a second confirmation (tlua_permission_f)
	tlua_detail_add(buf, sizeof(buf), "fromlink", tlua_fromurl(m) ? "1" : "0");
	// declared x granted: what it declares ("nothing": no mod.txt, standard only),
	// the declared tiers not decided yet (an update added them), what it used
	{
		char list[128];
		tlua_tierlist(m->requested, list, sizeof(list));
		tlua_detail_add(buf, sizeof(buf), "declares", m->nomanifest ? "nothing" : list);
		tlua_tierlist(tlua_unseen(m), list, sizeof(list));
		tlua_detail_add(buf, sizeof(buf), "unseen", list);
		if (m->icon[0] && !m->manifest_error)
		{
			dpsnprintf(line, sizeof(line), "mod:%s/%s", m->name, m->icon);
			tlua_detail_add(buf, sizeof(buf), "icon", line);
		}
		for (i = 0; tlua_permissions[i].name; i++)
			if (m->tiercalls[i])
			{
				dpsnprintf(line, sizeof(line), "%s\t%u calls", tlua_permissions[i].name, m->tiercalls[i]);
				tlua_detail_add(buf, sizeof(buf), "used", line);
			}
		if (m->hosts[0])
			tlua_detail_add(buf, sizeof(buf), "hosts", m->hosts);
		if (m->engcmds_used)
			tlua_detail_add(buf, sizeof(buf), "engine_commands", va(line, sizeof(line), "%d", m->engcmds_used));
	}
	for (i = 0; tlua_permissions[i].name; i++)
	{
		unsigned bit = tlua_permissions[i].bit;
		if (!(m->requested & bit))
			continue;
		dpsnprintf(line, sizeof(line), "%s\t%d\t%s", tlua_permissions[i].name, (m->granted & bit) ? 1 : 0,
			m->reasons[i][0] ? m->reasons[i] : tlua_permissions[i].help);
		tlua_detail_add(buf, sizeof(buf), "perm", line);
	}
	for (i = 0; r->events[i]; i++)
		if (m->eventmask & (1ull << i))
			tlua_detail_add(buf, sizeof(buf), "event", r->events[i]);
	for (i = 0; i < TLua_Hooks_CountRealm(r); i++)
		if (m->hookmask[i >> 5] & (1u << (i & 31)))
			tlua_detail_add(buf, sizeof(buf), "hook", TLua_Hooks_NameRealm(r, i));

	for (i = 0; i < tlua_numcmds; i++)
		if (tlua_cmds[i].realm == r && !strcmp(tlua_cmds[i].mod, m->name))
			tlua_detail_add(buf, sizeof(buf), "command", tlua_cmds[i].full);
	for (i = 0; i < m->numsettings; i++)
	{
		tlua_setting_t *st = &m->settings[i];
		char cv[96];
		dpsnprintf(cv, sizeof(cv), "%s%s_%s", r->cmdprefix, m->name, st->name);
		dpsnprintf(line, sizeof(line), "%s\t%s\t%s\t%s\t%s", cv, st->type, Cvar_VariableString(&cvars_all, cv, ~0), st->args, st->label);
		tlua_detail_add(buf, sizeof(buf), "setting", line);
	}
	for (j = 0; j < m->logcount; j++)
		tlua_detail_add(buf, sizeof(buf), "log", m->log[(m->lognext - m->logcount + j + TLUA_LOGLINES) % TLUA_LOGLINES]);
	if (strcmp(lua_detail.string, buf))
		Cvar_SetQuick(&lua_detail, buf);
}

static void tlua_updatestatus(tlua_realm_t *r)
{
	char buf[8192];
	int i;
	tlua_detail_dirty = true;
	if (!r->status || (r == &tlua_client && cls.state == ca_dedicated))
		return; // a dedicated server has no client cvars
	buf[0] = 0;
	for (i = 0; i < r->nummods; i++)
	{
		tlua_mod_t *m = &r->mods[i];
		char line[1024];
		char icon[128] = "";
		// the 6th field: the package's icon as a UI bridge image ref, only when it has one
		if (m->icon[0] && !m->manifest_error)
			dpsnprintf(icon, sizeof(icon), "mod:%s/%s", m->name, m->icon);
		dpsnprintf(line, sizeof(line), "%s\t%s\t%s\t%s\t%s\t%s\n", m->name, tlua_statename(m), m->title[0] ? m->title : m->name,
			m->error[0] ? m->error : m->manifest_why, m->description, icon);
		dp_strlcat(buf, line, sizeof(buf));
	}
	Cvar_SetQuick(r->status, buf);
	if (r == &tlua_client)
	{
		char perms[8192];
		perms[0] = 0;
		for (i = 0; i < r->nummods; i++)
		{
			tlua_mod_t *m = &r->mods[i];
			char requested[256] = "", granted[256] = "", line[640];
			int j;
			for (j = 0; tlua_permissions[j].name; j++)
			{
				unsigned bit = tlua_permissions[j].bit;
				if (m->requested & bit)
				{
					if (requested[0]) dp_strlcat(requested, ",", sizeof(requested));
					dp_strlcat(requested, tlua_permissions[j].name, sizeof(requested));
					if (m->granted & bit)
					{
						if (granted[0]) dp_strlcat(granted, ",", sizeof(granted));
						dp_strlcat(granted, tlua_permissions[j].name, sizeof(granted));
					}
				}
			}
			{
				char unseen[128];
				tlua_tierlist(tlua_unseen(m), unseen, sizeof(unseen));
				// 5th field: declared tiers the player has not decided on yet (new after an update)
				dpsnprintf(line, sizeof(line), "%s\t%s\t%s\t%s\t%s\n", m->name,
					m->manifest_error ? "invalid" : m->nomanifest ? "none" : m->scoped ? "v1" : "legacy", requested, granted, unseen);
			}
			dp_strlcat(perms, line, sizeof(perms));
		}
		Cvar_SetQuick(&cl_lua_permissions, perms);
	}
}

void TLua_UpdateStatus(tlua_realm_t *r) { tlua_updatestatus(r); }

tlua_mod_t *TLua_Find(tlua_realm_t *r, const char *name)
{
	int i;
	for (i = 0; i < r->nummods; i++)
		if (!strcmp(r->mods[i].name, name))
			return &r->mods[i];
	return NULL;
}

static void tlua_stop(tlua_mod_t *m)
{
	if (m->L && m->state == TLUA_RUNNING)
	{
		tlua_nothrottle = true;
		tlua_fire_one(m, "unload", NULL, NULL, NULL);
		tlua_nothrottle = false;
	}
	m->killreason[0] = 0;
	tlua_close(m);
	m->state = TLUA_OFF;
}

void TLua_Stop(tlua_mod_t *m) { tlua_stop(m); }
void TLua_Start(tlua_mod_t *m) { tlua_start(m); }
void TLua_Fail(tlua_mod_t *m, const char *msg) { tlua_fail(m, msg); }
void TLua_WriteEnabled(tlua_realm_t *r) { tlua_writeenabled(r); }
qbool TLua_ReservedName(tlua_realm_t *r, const char *name) { return tlua_reservedname(r, name); }

static int tlua_cmpmod(const void *a, const void *b)
{
	return strcmp(((const tlua_mod_t *)a)->name, ((const tlua_mod_t *)b)->name);
}

// a mod folder: a real directory (not a symlink) holding a regular init.lua
// (not a FIFO, which used to block the scan forever, nor a symlink)
static qbool tlua_ismodfolder(const char *dir, const char *name)
{
	char rel[128];
	int fd;
	if (!tlua_isrealdir(dir, name))
		return false;
	dpsnprintf(rel, sizeof(rel), "%s/init.lua", name);
	fd = tlua_open_confined(dir, rel, TLUA_RD, false);
	if (fd < 0)
		return false;
	close(fd);
	return true;
}

// A running mod's lua_State points at its tlua_mod_t (extraspace, allocator):
// after entries moved, point them at their new place
static void tlua_fixptrs(tlua_realm_t *r)
{
	int i;
	for (i = 0; i < r->nummods; i++)
	{
		tlua_mod_t *m = &r->mods[i];
		if (!m->L)
			continue;
		*(tlua_mod_t **)lua_getextraspace(m->L) = m;
		lua_setallocf(m->L, tlua_alloc, m);
	}
}

void TLua_Rescan(tlua_realm_t *r)
{
	char dir[1024];
	stringlist_t list;
	int i, j;
	if (TLua_Busy())
		return; // entries move below: never while a call into a mod runs
	tlua_dir(r, dir, sizeof(dir));
	stringlistinit(&list);
	listdirectory(&list, dir, "");
	// drop folders that went away (uninstalled, deleted); a running one is
	// stopped first
	for (i = 0; i < r->nummods; i++)
	{
		tlua_mod_t *m = &r->mods[i];
		qbool found = false;
		for (j = 0; j < list.numstrings; j++)
			if (!strcmp(list.strings[j], m->name))
				found = true;
		if (found && tlua_ismodfolder(dir, m->name))
			continue;
		if (m->L)
			tlua_stop(m);
		memmove(&r->mods[i], &r->mods[i + 1], sizeof(r->mods[0]) * (size_t)(r->nummods - i - 1));
		r->nummods--;
		i--;
	}
	for (j = 0; j < list.numstrings; j++)
	{
		const char *name = list.strings[j];
		tlua_mod_t *m;
		if (!TLua_ValidModName(name) || TLua_Find(r, name))
			continue;
		if (!tlua_ismodfolder(dir, name))
			continue;
		if (r->nummods >= TLUA_MAX_MODS)
		{
			Con_Printf(CON_WARN "Lua: more than %d %ss, %s ignored\n", TLUA_MAX_MODS, TLUA_NOUN(r), name);
			continue;
		}
		m = &r->mods[r->nummods++];
		memset(m, 0, sizeof(*m));
		dp_strlcpy(m->name, name, sizeof(m->name));
		dpsnprintf(m->dir, sizeof(m->dir), "%s%s/", dir, name);
		m->realm = r;
		m->owncvars = -1;
	}
	stringlistfreecontents(&list);
	// alphabetical order is the load order
	qsort(r->mods, (size_t)r->nummods, sizeof(r->mods[0]), tlua_cmpmod);
	tlua_fixptrs(r);
	for (i = 0; i < r->nummods; i++)
	{
		tlua_mod_t *m = &r->mods[i];
		tlua_readinfo(m);
		tlua_readgrants(m);
		tlua_readseen(m);
		m->reserved = tlua_reservedname(r, m->name);
		if (m->reserved && m->state != TLUA_RUNNING && !m->error[0])
			dp_strlcpy(m->error, "the name is reserved (it clashes with the engine's commands or settings)", sizeof(m->error));
		if (!m->reserved && !m->manifest_error)
			tlua_settings_cvars(m);
	}
	tlua_readenabled(r);
	tlua_updatestatus(r);
	TLua_UI_SyncManifest(r);
}

void TLua_LoadAll(tlua_realm_t *r)
{
	int i;
	if (TLua_Busy())
		return;
	r->loaded = true;
	TLua_Rescan(r);
	tlua_check_marker(r);
	if (!r->enable->integer)
	{
		tlua_updatestatus(r);
		return;
	}
	for (i = 0; i < r->nummods; i++)
		if (r->mods[i].enabled && !r->mods[i].L)
			tlua_start(&r->mods[i]);
	tlua_updatestatus(r);
}

void TLua_UnloadAll(tlua_realm_t *r, const char *why)
{
	int i, n = 0;
	if (TLua_Busy())
		return;
	for (i = 0; i < r->nummods; i++)
	{
		if (r->mods[i].L)
			n++;
		tlua_stop(&r->mods[i]);
	}
	if (n && why)
		Con_DPrintf("Lua: %d %s(s) stopped (%s)\n", n, TLUA_NOUN(r), why);
	r->loaded = false;
	tlua_updatestatus(r);
}

static tlua_realm_t *tlua_realmofcmd(cmd_state_t *cmd)
{
	return !strncmp(Cmd_Argv(cmd, 0), "sv_", 3) ? &tlua_server : &tlua_client;
}

// A server can put any command in a client's console (stufftext, its CSQC's
// localcmd), and the engine runs it like typed input: a change to mods or
// grants (either realm: a client's server mods run when it hosts a game later) is
// refused while it may come from one (TLua_FromServer), and from inside a
// call into a mod.
static qbool tlua_remote_refused(cmd_state_t *cmd)
{
	if (cmd->source != src_local || TLua_FromServer())
	{
		TLua_Say("%s: not while connected to a server this game does not host, or while text it sent is pending; disconnect first", Cmd_Argv(cmd, 0));
		return true;
	}
	if (TLua_Busy())
	{
		Con_Printf("%s: not from inside a Lua call\n", Cmd_Argv(cmd, 0));
		return true;
	}
	return false;
}

qbool TLua_RemoteRefused(cmd_state_t *cmd) { return tlua_remote_refused(cmd); }

static void tlua_list_f(cmd_state_t *cmd)
{
	tlua_realm_t *r = tlua_realmofcmd(cmd);
	char dir[1024];
	int i;
	if (TLua_Busy())
		return;
	TLua_Rescan(r);
	tlua_dir(r, dir, sizeof(dir));
	Con_Printf("Lua %ss in %s (master switch %s %s):\n", TLUA_NOUN(r), dir, r->enable->name, r->enable->integer ? "on" : "off");
	for (i = 0; i < r->nummods; i++)
	{
		tlua_mod_t *m = &r->mods[i];
		char kb[32] = "";
		if (m->state == TLUA_RUNNING)
			dpsnprintf(kb, sizeof(kb), " (%i KB)", (int)(m->mem >> 10));
		Con_Printf("  %-20s %-8s %s%s\n", m->name, tlua_statename(m), m->title[0] ? m->title : "", kb);
		if (m->error[0])
			Con_Printf("      ^1%s^7\n", m->error);
	}
	if (!r->nummods)
		Con_Printf("  none: put a folder with an init.lua there (docs/MODDING.md)\n");
}

static tlua_mod_t *tlua_argmod(cmd_state_t *cmd, tlua_realm_t *r)
{
	tlua_mod_t *m;
	if (Cmd_Argc(cmd) < 2)
	{
		Con_Printf("usage: %s <name>\n", Cmd_Argv(cmd, 0));
		return NULL;
	}
	TLua_Rescan(r);
	m = TLua_Find(r, Cmd_Argv(cmd, 1));
	if (!m)
		Con_Printf("%s: no %s named %s\n", Cmd_Argv(cmd, 0), TLUA_NOUN(r), Cmd_Argv(cmd, 1));
	return m;
}

static qbool tlua_parsetiers(tlua_mod_t *m, const char *list_in, qbool grant, unsigned *out);
static qbool tlua_engine_ok(tlua_mod_t *m, unsigned adding, qbool confirmed, const char *how);
static qbool tlua_setgrants(tlua_mod_t *m, unsigned mask);

// lua_mod_enable <mod> [with <tiers>|all|none [confirm]]
// "with" is the turn-on screen's answer: the player saw what the mod declares
// and chose which of its optional tiers to give; they are granted, everything
// it declares counts as seen, and the mod starts. Without "with" the mod
// starts with what it was granted before; tiers it declares that the player
// was never shown stay off, and the message (and _lua_detail's "unseen")
// says so, so the UIs can ask.
static void tlua_enable_f(cmd_state_t *cmd)
{
	tlua_realm_t *r = tlua_realmofcmd(cmd);
	tlua_mod_t *m;
	char marked[TLUA_NAME_LEN + 32];
	if (tlua_remote_refused(cmd) || !(m = tlua_argmod(cmd, r)))
		return;
	if (Cmd_Argc(cmd) >= 4 && !strcmp(Cmd_Argv(cmd, 2), "with"))
	{
		unsigned mask;
		char how[160];
		if (m->manifest_error)
		{
			TLua_Say("%s: its manifest is invalid", m->name);
			return;
		}
		// keep: settle the declared tiers as they are now (a turn-on screen whose
		// switches already granted what the player chose)
		if (!strcmp(Cmd_Argv(cmd, 3), "keep"))
			mask = m->granted & m->requested & TLUA_PERM_OPTIONAL;
		else if (!tlua_parsetiers(m, Cmd_Argv(cmd, 3), true, &mask))
			return;
		dpsnprintf(how, sizeof(how), "%s %s with %s confirm", Cmd_Argv(cmd, 0), m->name, Cmd_Argv(cmd, 3));
		if (!tlua_engine_ok(m, mask, Cmd_Argc(cmd) >= 5 && !strcmp(Cmd_Argv(cmd, 4), "confirm"), how))
			return;
		if (!tlua_setgrants(m, mask))
			return;
		TLua_Log(m, "turned on with %s", Cmd_Argv(cmd, 3));
	}
	else if (Cmd_Argc(cmd) > 2)
	{
		Con_Printf("usage: %s <name> [with <tier>[,<tier>...]|all|none|keep [confirm]]\n", Cmd_Argv(cmd, 0));
		return;
	}
	// turning on a mod the crash marker names is the user's retry
	tlua_marker_get(r, marked, sizeof(marked));
	if (!strcmp(marked, m->name))
		tlua_marker_clear(r);
	m->enabled = true;
	m->error[0] = 0;
	m->crashed = false;
	tlua_writeenabled(r);
	if (r->loaded && r->enable->integer && !m->L)
		tlua_start(m);
	tlua_updatestatus(r);
	TLua_Log(m, "turned on");
	{
		char missing[256], unseen[128];
		tlua_missing(m, missing, sizeof(missing));
		tlua_tierlist(tlua_unseen(m), unseen, sizeof(unseen));
		if (unseen[0])
			TLua_Say("Lua %s %s: %s; it declares %s, not decided yet and off (%s_permission %s grant|decline %s)", TLUA_NOUN(r), m->name, tlua_statename(m), unseen,
				r == &tlua_server ? "sv_lua_mod" : "lua_mod", m->name, unseen);
		else if (missing[0])
			TLua_Say("Lua %s %s: %s; off by your choice: %s", TLUA_NOUN(r), m->name, tlua_statename(m), missing);
		else
			TLua_Say("Lua %s %s: %s", TLUA_NOUN(r), m->name, tlua_statename(m));
	}
}

static void tlua_disable_f(cmd_state_t *cmd)
{
	tlua_realm_t *r = tlua_realmofcmd(cmd);
	tlua_mod_t *m;
	if (tlua_remote_refused(cmd) || !(m = tlua_argmod(cmd, r)))
		return;
	m->enabled = false;
	tlua_writeenabled(r);
	tlua_stop(m);
	m->error[0] = 0;
	m->crashed = false;
	tlua_updatestatus(r);
	TLua_Log(m, "turned off");
	TLua_Say("Lua %s %s: off", TLUA_NOUN(r), m->name);
}

static void tlua_reload_f(cmd_state_t *cmd)
{
	tlua_realm_t *r = tlua_realmofcmd(cmd);
	int i;
	if (tlua_remote_refused(cmd))
		return;
	if (Cmd_Argc(cmd) >= 2)
	{
		tlua_mod_t *m = tlua_argmod(cmd, r);
		if (!m)
			return;
		tlua_stop(m);
		m->error[0] = 0;
		TLua_Log(m, "reloaded");
		if (m->enabled && r->loaded && r->enable->integer)
			tlua_start(m);
		tlua_updatestatus(r);
		TLua_Say("Lua %s %s: %s", TLUA_NOUN(r), m->name, tlua_statename(m));
		return;
	}
	for (i = 0; i < r->nummods; i++)
	{
		tlua_stop(&r->mods[i]);
		r->mods[i].error[0] = 0;
	}
	if (r->loaded)
		TLua_LoadAll(r);
	else
		TLua_Rescan(r);
}

static void tlua_info_f(cmd_state_t *cmd)
{
	tlua_realm_t *r = tlua_realmofcmd(cmd);
	tlua_mod_t *m;
	int i;
	char vabuf[64];
	if (TLua_Busy() || !(m = tlua_argmod(cmd, r)))
		return;
	Con_Printf("%s %s: %s\n  folder: %s\n", TLUA_NOUN(r), m->name, tlua_statename(m), m->dir);
	if (m->title[0])
		Con_Printf("  title: %s\n", m->title);
	if (m->version[0])
		Con_Printf("  version: %s\n", m->version);
	if (m->author[0])
		Con_Printf("  author: %s\n", m->author);
	if (m->description[0])
		Con_Printf("  %s\n", m->description);
	if (m->homepage[0])
		Con_Printf("  homepage: %s\n", m->homepage);
	Con_Printf("  api: %s\n", m->manifest_error ? "invalid manifest" : m->api ? va(vabuf, sizeof(vabuf), "%d", m->api) : "legacy (no api: line)");
	if (m->manifest_why[0])
		Con_Printf("  ^1manifest: %s^7\n", m->manifest_why);
	if (m->L)
		Con_Printf("  memory: %i KB of %i MB\n", (int)(m->mem >> 10), (int)(m->memlimit >> 20));
	if (m->error[0])
		Con_Printf("  ^1error: %s^7\n", m->error);
	for (i = 0; m->realm->events[i]; i++)
		if (*tlua_maskof(m) & (1ull << i))
			Con_Printf("  listens to: %s\n", m->realm->events[i]);
	for (i = 0; i < tlua_numcmds; i++)
		if (tlua_cmds[i].realm == r && !strcmp(tlua_cmds[i].mod, m->name))
			Con_Printf("  command: %s%s\n", tlua_cmds[i].full, tlua_cmds[i].fromserver ? " (a server may run it)" : "");
	for (i = 0; i < TLua_Hooks_CountRealm(r); i++)
		if (m->hookmask[i >> 5] & (1u << (i & 31)))
			Con_Printf("  hook: %s\n", TLua_Hooks_NameRealm(r, i));
	for (i = 0; i < m->numsettings; i++)
		Con_Printf("  setting: %s%s_%s (%s, %s)\n", r->cmdprefix, m->name, m->settings[i].name, m->settings[i].type, m->settings[i].label);
	for (i = 0; tlua_permissions[i].name; i++)
		if (m->requested & tlua_permissions[i].bit)
			Con_Printf("  permission %s: %s - %s\n", tlua_permissions[i].name, (m->granted & tlua_permissions[i].bit) ? "^2granted^7" : "^3not granted^7",
				m->reasons[i][0] ? m->reasons[i] : tlua_permissions[i].help);
	if (m->numtimers)
		Con_Printf("  timers and coroutines: %d\n", m->numtimers);
	{
		char list[128];
		tlua_tierlist(m->requested, list, sizeof(list));
		Con_Printf("  declares: %s\n", m->nomanifest ? "nothing (no mod.txt): standard only, nothing more can be granted" : list);
		tlua_tierlist(tlua_unseen(m), list, sizeof(list));
		if (list[0])
			Con_Printf("  ^3not decided yet (off): %s^7\n", list);
		for (i = 0; tlua_permissions[i].name; i++)
			if (m->tiercalls[i])
				Con_Printf("  used %s: %u calls\n", tlua_permissions[i].name, m->tiercalls[i]);
		if (m->hosts[0])
			Con_Printf("  hosts contacted: %s\n", m->hosts);
		if (m->engcmds_used)
			Con_Printf("  engine commands run: %d\n", m->engcmds_used);
		if (m->icon[0] && !m->manifest_error)
		{
			char ref[128];
			unsigned char *px;
			dpsnprintf(ref, sizeof(ref), "mod:%s/%s", m->name, m->icon);
			px = TLua_ModImageBGRA(ref);
			if (px)
			{
				Con_Printf("  icon: %s (%dx%d)\n", m->icon, image_width, image_height);
				Mem_Free(px);
			}
			else
				Con_Printf("  ^1icon: %s cannot be read (a .png/.tga up to 256x256 and 256 KB)^7\n", m->icon);
		}
		else
			Con_Printf("  icon: none\n");
	}
}

// lua_mod_log <name> [lines]: the mod's recent print() lines, errors and
// manager notes
static void tlua_log_f(cmd_state_t *cmd)
{
	tlua_realm_t *r = tlua_realmofcmd(cmd);
	tlua_mod_t *m;
	int j, n;
	if (TLua_Busy() || !(m = tlua_argmod(cmd, r)))
		return;
	n = Cmd_Argc(cmd) >= 3 ? atoi(Cmd_Argv(cmd, 2)) : TLUA_LOGLINES;
	n = bound(1, n, m->logcount);
	if (!m->logcount)
	{
		Con_Printf("Lua %s %s: no output yet\n", TLUA_NOUN(r), m->name);
		return;
	}
	Con_Printf("Lua %s %s, last %d line(s):\n", TLUA_NOUN(r), m->name, n);
	for (j = m->logcount - n; j < m->logcount; j++)
		Con_Printf("  %s\n", m->log[(m->lognext - m->logcount + j + TLUA_LOGLINES) % TLUA_LOGLINES]);
}

// lua / lua_help: every manager command with one line of help
static void tlua_help_f(cmd_state_t *cmd)
{
	static const char *const help[][2] = {
		{"lua_mods", "list client mods with their state"},
		{"lua_mod_info <name>", "details: version, permissions with reasons, events, hooks, commands, settings"},
		{"lua_mod_log <name> [n]", "the mod's recent output and errors"},
		{"lua_mod_enable <name>", "turn on (nothing is granted by this)"},
		{"lua_mod_disable <name>", "turn off"},
		{"lua_mod_reload [name]", "restart one or all, clearing errors"},
		{"lua_mod_permission <name> [grant|revoke <scope>]", "show or change permissions"},
		{"lua_mod_install <path|https-url>[#sha256=<hex>]", "review a package (.zip or folder); nothing is enabled or granted"},
		{"lua_install_confirm / lua_install_cancel", "finish or drop the reviewed install"},
		{"lua_install_status", "what the pending install is doing"},
		{"lua_mod_update <name> [path|url]", "fetch the mod again from where it came from (or the given source) and review it"},
		{"lua_mod_uninstall <name> [keepdata]", "remove the mod's folder, grants and record (keepdata keeps data/)"},
		{"sv_lua_mods, sv_lua_mod_*", "the same for server mods, the ones that run in games you host (info, log, enable, disable, reload, permission, install, update, uninstall; the old sv_lua_plugin_* names still work)"},
		{"sv_lua_hooks [all]", "mutator hooks server mods can use, and which are subscribed"},
		{"cl_lua_enable / sv_lua_enable", "master switches"},
		{"lua_memory_mb, lua_instructions, lua_time_ms", "limits per call"},
		{"lua_hook_ms, lua_hook_frame_ms", "limits of hook calls (per call, per server mod per frame)"},
	};
	size_t i;
	(void)cmd;
	Con_Printf("Lua mods: client mods run on this device, server mods in games you host (docs/PLUGIN_STANDARD.md)\n");
	Con_Printf("^2How to make a mod: https://github.com/dixonSolutions/Godnotic/blob/main/docs/MODDING.md^7\n");
	for (i = 0; i < sizeof(help) / sizeof(help[0]); i++)
		Con_Printf("  ^3%s^7\n      %s\n", help[i][0], help[i][1]);
	Con_Printf("^2How to make a mod: https://github.com/dixonSolutions/Godnotic/blob/main/docs/MODDING.md^7\n");
}

// Was this mod installed from a link (luastate/<realm>/sources/<id>.txt holds
// an http(s) URL)? Then Full engine access needs a second confirmation.
static qbool tlua_fromurl(tlua_mod_t *m)
{
	char path[1100], buf[300];
	tlua_statedir(m->realm, path, sizeof(path));
	dp_strlcat(path, "sources/", sizeof(path));
	dp_strlcat(path, m->name, sizeof(path));
	dp_strlcat(path, ".txt", sizeof(path));
	if (tlua_readstate(path, buf, sizeof(buf)) <= 0)
		return false;
	return !strncmp(buf, "https://", 8) || !strncmp(buf, "http://", 7);
}

// Grants are a local user choice, never supplied by a Lua mod or a server.
// Tiers (docs/PLUGIN_STANDARD.md): standard is on with the mod; gamerules,
// internet and engine_full are switches. "all" never includes engine_full,
// and for a mod installed from a link engine_full needs "confirm" as well,
// given within a minute of the first grant (which prints the warning).
static char tlua_engine_pending[TLUA_NAME_LEN + 16];
static double tlua_engine_pending_time;

// Parses "<tier>[,<tier>...]" or "all" (every declared optional tier but
// engine_full) into a mask of declared tiers. grant: a tier the mod does not
// declare is refused with a message (false).
static qbool tlua_parsetiers(tlua_mod_t *m, const char *list_in, qbool grant, unsigned *out)
{
	char list[256], *tok;
	unsigned mask = 0;
	if (!strcmp(list_in, "all"))
	{
		*out = grant ? (m->requested & TLUA_PERM_OPTIONAL & ~TLUA_TIER_ENGINE) : TLUA_PERM_OPTIONAL;
		return true;
	}
	if (!strcmp(list_in, "none"))
	{
		*out = 0;
		return true;
	}
	dp_strlcpy(list, list_in, sizeof(list));
	for (tok = strtok(list, ","); tok; tok = strtok(NULL, ","))
	{
		const tlua_permission_t *p = tlua_findperm(tok);
		if (!p)
		{
			TLua_Say("unknown tier %s (standard, gamerules, internet, engine_full)", tok);
			return false;
		}
		if (grant && !(m->requested & p->bit))
		{
			if (m->nomanifest)
				TLua_Say("%s declares nothing (no mod.txt): it runs with the standard tier only, and nothing more can be granted; give it a mod.txt with permissions: %s", m->name, p->name);
			else
				TLua_Say("%s does not declare the %s tier (permissions: in its mod.txt), so it cannot be granted", m->name, p->name);
			return false;
		}
		mask |= p->bit;
	}
	*out = mask & TLUA_PERM_OPTIONAL; // standard comes with turning the mod on
	return true;
}

// engine_full for a mod installed from a link needs a second, explicit
// confirmation (within a minute); true when the grant may go ahead
static qbool tlua_engine_ok(tlua_mod_t *m, unsigned adding, qbool confirmed, const char *how)
{
	char key[TLUA_NAME_LEN + 16];
	if (!(adding & TLUA_TIER_ENGINE) || (m->granted & TLUA_TIER_ENGINE))
		return true;
	dpsnprintf(key, sizeof(key), "%s %s", m->realm->kind, m->name);
	Con_Printf(CON_WARN "Full engine access for %s: it can run any console command and change any setting except passwords and keys, as if you typed it.\n", m->name);
	if (tlua_fromurl(m) && (!confirmed || strcmp(tlua_engine_pending, key) || host.realtime - tlua_engine_pending_time > 60))
	{
		dp_strlcpy(tlua_engine_pending, key, sizeof(tlua_engine_pending));
		tlua_engine_pending_time = host.realtime;
		TLua_Say("%s was installed from a link: to give it full engine access, confirm with %s", m->name, how);
		return false;
	}
	tlua_engine_pending[0] = 0;
	return true;
}

// stores the player's grants (always within what the mod declares), marks
// every declared tier as seen, and takes effect at once: the next call checks
static qbool tlua_setgrants(tlua_mod_t *m, unsigned mask)
{
	char dir[1024], file[64], text[32];
	mask &= m->requested & TLUA_PERM_OPTIONAL;
	tlua_grantpath(m, dir, sizeof(dir), file, sizeof(file));
	dpsnprintf(text, sizeof(text), "%s %x\n", m->scoped ? "v1" : "legacy", mask);
	if (!tlua_writestate(dir, file, text, true))
	{
		TLua_Say("could not save permissions");
		return false;
	}
	m->granted = mask | TLUA_TIER_STANDARD;
	tlua_markseen(m);
	if (m->realm == &tlua_server)
		TLua_Hooks_Changed();
	return true;
}

// lua_mod_permission <mod>                         what it declares and has
// lua_mod_permission <mod> grant|revoke <tiers>|all [confirm]
// lua_mod_permission <mod> decline <tiers>|all     seen, left off (the new ones after an update)
static void tlua_permission_f(cmd_state_t *cmd)
{
	tlua_realm_t *r = tlua_realmofcmd(cmd);
	tlua_mod_t *m;
	const char *action, *name;
	char text[32], how[160];
	unsigned mask;
	int i;
	if (Cmd_Argc(cmd) == 2)
	{
		if (TLua_Busy() || !(m = tlua_argmod(cmd, r))) return;
		Con_Printf("Lua %s %s, API %s%s; %s:\n", TLUA_NOUN(r), m->name, m->api ? va(text, sizeof(text), "%d", m->api) : "legacy",
			m->manifest_error ? " (invalid manifest)" : "", m->nomanifest ? "declares nothing (no mod.txt): standard only" : "it declares");
		for (i = 0; tlua_permissions[i].name; i++)
		{
			unsigned bit = tlua_permissions[i].bit;
			if (!(m->requested & bit))
				continue;
			Con_Printf("  %-12s %s%s - %s\n", tlua_permissions[i].name, tlua_has(m, bit) ? "^2on^7" : "off",
				(tlua_unseen(m) & bit) ? " ^3(new: not decided yet)^7" : "", m->reasons[i][0] ? m->reasons[i] : tlua_permissions[i].help);
		}
		return;
	}
	if (tlua_remote_refused(cmd) || !(m = tlua_argmod(cmd, r))) return;
	if (Cmd_Argc(cmd) != 4 && Cmd_Argc(cmd) != 5)
	{
		Con_Printf("usage: %s <name> [grant|revoke|decline <tier>[,<tier>...]|all [confirm]]\n", Cmd_Argv(cmd, 0));
		return;
	}
	action = Cmd_Argv(cmd, 2); name = Cmd_Argv(cmd, 3);
	if (strcmp(action, "grant") && strcmp(action, "revoke") && strcmp(action, "decline"))
	{
		TLua_Say("unknown action %s (grant, revoke or decline)", action); return;
	}
	if (m->manifest_error && !strcmp(action, "grant"))
	{
		TLua_Say("%s: its manifest is invalid, nothing can be granted", m->name); return;
	}
	if (!tlua_parsetiers(m, name, !strcmp(action, "grant"), &mask))
		return;
	dpsnprintf(how, sizeof(how), "%s %s grant engine_full confirm", Cmd_Argv(cmd, 0), m->name);
	if (!strcmp(action, "grant") && !tlua_engine_ok(m, mask, Cmd_Argc(cmd) == 5 && !strcmp(Cmd_Argv(cmd, 4), "confirm"), how))
		return;
	if (!strcmp(action, "decline"))
		mask = m->granted & ~mask; // seen, and off
	else
		mask = !strcmp(action, "grant") ? m->granted | mask : m->granted & ~mask;
	if (!tlua_setgrants(m, mask))
		return;
	TLua_Log(m, "%s %s", !strcmp(action, "grant") ? "granted" : !strcmp(action, "revoke") ? "revoked" : "declined", name);
	// a grant is what a mod stopped by "permission denied" waited for: start it again
	if (!strcmp(action, "grant") && m->enabled && m->state == TLUA_ERROR && r->loaded && r->enable->integer)
	{
		tlua_stop(m);
		m->error[0] = 0;
		tlua_start(m);
		TLua_Say("Lua %s %s: granted %s, restarted: %s", TLUA_NOUN(r), m->name, name, tlua_statename(m));
	}
	else
		TLua_Say("Lua %s %s: %s %s", TLUA_NOUN(r), m->name, action, name);
	tlua_updatestatus(r);
}

// ---------------------------------------------------------------------------
// Engine entry points

// The limit cvars and the master switches: a change while it may come from a
// server (stufftext "lua_memory_mb 64", a server CSQC's cvar_set) is put back.
// They are archived, so such a change used to outlive the connection.
typedef struct tlua_guard_s { cvar_t *var; char trusted[64]; } tlua_guard_t;
static tlua_guard_t tlua_guards[8];
static int tlua_numguards;

static void tlua_guard_cb(cvar_t *var)
{
	static qbool restoring;
	int i;
	if (restoring)
		return;
	for (i = 0; i < tlua_numguards; i++)
		if (tlua_guards[i].var == var)
			break;
	if (i == tlua_numguards)
		return;
	if (strcmp(var->string, tlua_guards[i].trusted) && TLua_FromServer())
	{
		Con_Printf(CON_WARN "%s: a change while connected to a server this game does not host was refused (disconnect first)\n", var->name);
		restoring = true;
		Cvar_SetQuick(var, tlua_guards[i].trusted);
		restoring = false;
		return;
	}
	dp_strlcpy(tlua_guards[i].trusted, var->string, sizeof(tlua_guards[i].trusted));
}

void TLua_Guard(cvar_t *var);
void TLua_Guard(cvar_t *var)
{
	if (tlua_numguards >= (int)(sizeof(tlua_guards) / sizeof(tlua_guards[0])))
		return;
	tlua_guards[tlua_numguards].var = var;
	dp_strlcpy(tlua_guards[tlua_numguards].trusted, var->string, sizeof(tlua_guards[0].trusted));
	tlua_numguards++;
	Cvar_RegisterCallback(var, tlua_guard_cb);
}

// Host_Shutdown calls host.hook.SV_Shutdown (sv_main.c sets it) after the
// client is down: at quit, every mod and plugin gets its "unload" event (to
// save its state) before the server shuts down. The same hook also runs when
// a join is cancelled; then host.state is not host_shutdown and only the
// server's own shutdown runs.
static void (*tlua_sv_shutdown_next)(void);
static void tlua_shutdown_hook(void)
{
	if (host.state == host_shutdown && !TLua_Busy())
	{
		TLua_UnloadAll(&tlua_client, "quit");
		TLua_UnloadAll(&tlua_server, "quit");
	}
	if (tlua_sv_shutdown_next)
		tlua_sv_shutdown_next();
}

// Registers a Lua cvar the first time the Lua code runs, which is after the
// config (and a command line "set") may already have created it as a plain
// allocated cvar. Cvar_RegisterVariable's "replace an allocated cvar" path
// leaves the hash table pointing at the freed one (DarkPlaces notes it is
// unused, since engine cvars register before the config), which crashed the
// next lookup. So: keep the value, delete the allocated cvar the way "unset"
// does, register, and put the value back.
void TLua_RegisterCvar(cvar_t *var);
void TLua_RegisterCvar(cvar_t *var)
{
	cvar_t *old = Cvar_FindVar(&cvars_all, var->name, ~0);
	char saved[1024], line[256];
	qbool had = old && (old->flags & CF_ALLOCATED);
	if (had)
	{
		dp_strlcpy(saved, old->string, sizeof(saved));
		dpsnprintf(line, sizeof(line), "unset %s", var->name);
		Cmd_ExecuteString(cmd_local, line, strlen(line), src_local, false);
		if (Cvar_FindVar(&cvars_all, var->name, ~0))
			had = false; // could not be removed (an autocvar): leave it alone
	}
	if (!had && old)
		return;
	Cvar_RegisterVariable(var);
	if (had)
		Cvar_SetQuick(var, saved);
}

void TLua_InitOnce(void)
{
	static qbool done;
	if (done)
		return;
	done = true;
	tlua_inited = true;
	TLua_RegisterCvar(&sv_lua_status);
	TLua_RegisterCvar(&lua_select);
	TLua_RegisterCvar(&lua_detail);
	TLua_RegisterCvar(&lua_last);
	TLua_RegisterCvar(&lua_memory_mb);
	TLua_RegisterCvar(&lua_instructions);
	TLua_RegisterCvar(&lua_time_ms);
	TLua_RegisterCvar(&cl_lua_enable);
	TLua_RegisterCvar(&sv_lua_enable);
	TLua_RegisterCvar(&cl_lua_status);
	TLua_RegisterCvar(&cl_lua_permissions);
	TLua_Guard(&lua_memory_mb);
	TLua_Guard(&lua_instructions);
	TLua_Guard(&lua_time_ms);
	TLua_Guard(&cl_lua_enable);
	TLua_Guard(&sv_lua_enable);
	tlua_client.enable = &cl_lua_enable;
	tlua_client.status = &cl_lua_status;
	tlua_server.enable = &sv_lua_enable;
	tlua_server.status = &sv_lua_status;
	tlua_sv_shutdown_next = host.hook.SV_Shutdown;
	host.hook.SV_Shutdown = tlua_shutdown_hook;
	TLua_CL_Setup();
	TLua_SV_Setup();

	Cmd_AddCommand(CF_CLIENT, "lua_mods", tlua_list_f, "list the Lua client mods in <userdir>/mods and their state");
	Cmd_AddCommand(CF_CLIENT, "lua_mod_enable", tlua_enable_f, "turn a Lua client mod on (kept in mods/enabled.txt)");
	Cmd_AddCommand(CF_CLIENT, "lua_mod_disable", tlua_disable_f, "turn a Lua client mod off");
	Cmd_AddCommand(CF_CLIENT, "lua_mod_reload", tlua_reload_f, "restart one Lua client mod, or all of them (clears errors)");
	Cmd_AddCommand(CF_CLIENT, "lua_mod_permission", tlua_permission_f, "inspect or change a Lua client mod's permissions");
	Cmd_AddCommand(CF_CLIENT, "lua_mod_info", tlua_info_f, "show a Lua client mod's details, error, events and commands");
	// server mods (games you host); the code and the plugins/ folder still say plugin
	Cmd_AddCommand(CF_SERVER, "sv_lua_mods", tlua_list_f, "list the Lua server mods (games you host) in <userdir>/plugins and their state");
	Cmd_AddCommand(CF_SERVER, "sv_lua_mod_enable", tlua_enable_f, "turn a Lua server mod on (kept in plugins/enabled.txt)");
	Cmd_AddCommand(CF_SERVER, "sv_lua_mod_disable", tlua_disable_f, "turn a Lua server mod off");
	Cmd_AddCommand(CF_SERVER, "sv_lua_mod_reload", tlua_reload_f, "restart one Lua server mod, or all of them (clears errors)");
	Cmd_AddCommand(CF_SERVER, "sv_lua_mod_permission", tlua_permission_f, "inspect or change a Lua server mod's permissions");
	Cmd_AddCommand(CF_SERVER, "sv_lua_mod_info", tlua_info_f, "show a Lua server mod's details, error, events and commands");
	Cmd_AddCommand(CF_CLIENT, "lua_mod_log", tlua_log_f, "show a Lua client mod's recent output and errors");
	Cmd_AddCommand(CF_SERVER, "sv_lua_mod_log", tlua_log_f, "show a Lua server mod's recent output and errors");
	// old names of the server mod commands, kept so scripts and configs go on working
	Cmd_AddCommand(CF_SERVER, "sv_lua_plugins", tlua_list_f, "old name of sv_lua_mods");
	Cmd_AddCommand(CF_SERVER, "sv_lua_plugin_enable", tlua_enable_f, "old name of sv_lua_mod_enable");
	Cmd_AddCommand(CF_SERVER, "sv_lua_plugin_disable", tlua_disable_f, "old name of sv_lua_mod_disable");
	Cmd_AddCommand(CF_SERVER, "sv_lua_plugin_reload", tlua_reload_f, "old name of sv_lua_mod_reload");
	Cmd_AddCommand(CF_SERVER, "sv_lua_plugin_permission", tlua_permission_f, "old name of sv_lua_mod_permission");
	Cmd_AddCommand(CF_SERVER, "sv_lua_plugin_info", tlua_info_f, "old name of sv_lua_mod_info");
	Cmd_AddCommand(CF_SERVER, "sv_lua_plugin_log", tlua_log_f, "old name of sv_lua_mod_log");
	Cmd_AddCommand(CF_CLIENT | CF_SERVER, "lua", tlua_help_f, "list the Lua mod manager commands (client mods and server mods)");
	Cmd_AddCommand(CF_CLIENT | CF_SERVER, "lua_help", tlua_help_f, "list the Lua mod manager commands (client mods and server mods)");
	TLua_Hooks_Setup();
	TLua_Pkg_Setup();
	TLua_API_Setup();
}

static char tlua_lastselect[64];

// the client frame calls this first, before anything else of the Lua code
void TLua_FrameStart(void)
{
	tlua_taint_frame();
	TLua_Pkg_Frame();
	TLua_UI_Frame();
	if (tlua_detail_dirty || strcmp(lua_select.string, tlua_lastselect))
	{
		dp_strlcpy(tlua_lastselect, lua_select.string, sizeof(tlua_lastselect));
		tlua_publish_detail();
	}
}
