/*
Godnotic native front end mode. See touch_nativefe.h and docs/LAUNCHER.md.

The desktop app starts the engine (through packaging/start.sh) with:

  GODNOTIC_FRONTEND=native     this mode; anything else (or unset) leaves the
                               engine exactly as it was
  GODNOTIC_FE_RESULT=<path>    where to write how the session ended
  GODNOTIC_FE_WATCH_PID=<pid>  optional: quit when that process (the app) is gone
  GODNOTIC_FE_SECRET_FD=<n>    optional: an inherited pipe holding one line,
                               "join\t<address>\t<password>" or "host\t<password>".
                               Read once at start and closed; the variable is
                               removed from the environment. The password never
                               appears in argv, the environment or a log.

and the action as ordinary command-line commands (+connect <address>, or the
Host screen's cvars and +map). Command-line commands run with the startup
configs, before video starts, so the game opens straight into the join or the
map and host.c never queues the main menu (it does only when nothing is
connecting and no server runs).

The session ends when the game leaves it: the player picks Leave or
Disconnect in the in-game menu, the server kicks or drops the player, a join
fails or is refused, or a hosted/local game stops. The engine then writes
the result file and quits instead of showing the main menu:

  kind=left|kicked|connect_failed|password_required|password_wrong|error|quit
  reason=<the engine's connect status, e.g. "Disconnect: You were kicked">
  server=<address>

Quit from the in-game menu (or closing the window) writes kind=quit at exit.
A crash writes nothing; the app sees the signal.

GODNOTIC_FRONTEND=manager runs the Lua mod manager for the app without a
match: the app starts the engine with an offscreen video driver and
`+fe_manager "<commands>"`. After the first frames (when the mod manager has
registered its commands) the commands run, each between "@@FE-BEGIN <n>" and
"@@FE-END <n>" lines on the console (stdout); then, once a pending install
has finished downloading or checking, "@@FE-STATE <json string>" carries the
install review (_lua_install) and the engine quits.

Console commands (both modes): fe_mark <text> prints "@@FE <text>".
*/

#include "quakedef.h"
#include "cl_screen.h"
#include "touch_nativefe.h"

#ifndef WIN32
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#endif

enum { FE_OFF, FE_NATIVE, FE_MANAGER };

static int fe_mode = FE_OFF;
static char fe_result_path[1024];
static qbool fe_result_written;
static qbool fe_seen_session;     // connected, connecting or a server ran at some point
static qbool fe_seen_signon;      // got all the way into a match
static int fe_idle_frames;
static double fe_idle_since;
static double fe_start_time;
static int fe_password_kind;      // 1 required, 2 wrong (TouchLAN_Rejected)
static char fe_server[128];
static char fe_manager_cmds[4096];
static int fe_manager_stage;
static int fe_frames;
static int fe_watch_pid;          // the app that started us; when it is gone, so are we
static double fe_watch_next;

qbool TouchNativeFE_Active(void)
{
	return fe_mode == FE_NATIVE;
}

static void fe_write_result(const char *kind, const char *reason)
{
	char tmp[1100], clean[256];
	FILE *f;
	size_t i, n = 0;
	if (fe_result_written || !fe_result_path[0])
		return;
	fe_result_written = true;
	// one line per value: no newlines, no colour codes left half
	for (i = 0; reason && reason[i] && n < sizeof(clean) - 1; i++)
		clean[n++] = (reason[i] == '\n' || reason[i] == '\r') ? ' ' : reason[i];
	clean[n] = 0;
	dpsnprintf(tmp, sizeof(tmp), "%s.tmp", fe_result_path);
	f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "kind=%s\nreason=%s\nserver=%s\n", kind, clean, fe_server);
	fclose(f);
	rename(tmp, fe_result_path);
}

static void fe_atexit(void)
{
	// Quit from the in-game menu, the window closed, or Sys_Error (the app sees its exit status).
	fe_write_result("quit", "");
}

static void fe_end(const char *kind, const char *reason)
{
	Con_DPrintf("native front end: session ended (%s)\n", kind);
	fe_write_result(kind, reason);
	Cbuf_AddText(cmd_local, "\nquit\n");
}

static void fe_set_private(const char *name, const char *value)
{
	cvar_t *c = Cvar_FindVar(&cvars_all, name, ~0);
	if (c)
		Cvar_SetQuick(c, value);
}

// The secret: one line, read once from the inherited fd, which is then closed.
static void fe_read_secret(void)
{
#ifndef WIN32
	char buf[1024], *a, *b;
	const char *s = getenv("GODNOTIC_FE_SECRET_FD");
	int fd, n = 0, r;
	if (!s || !*s)
		return;
	fd = atoi(s);
	unsetenv("GODNOTIC_FE_SECRET_FD");
	if (fd < 3)
		return;
	while (n < (int)sizeof(buf) - 1 && (r = (int)read(fd, buf + n, sizeof(buf) - 1 - n)) > 0)
		n += r;
	close(fd);
	buf[n] = 0;
	buf[strcspn(buf, "\r\n")] = 0;
	a = strchr(buf, '\t');
	if (a)
	{
		*a++ = 0;
		if (!strcmp(buf, "host"))
		{
			// touch_lan_host_password_apply (on the command line, before +map) makes it the server's.
			fe_set_private("touch_lan_host_password", a);
		}
		else if (!strcmp(buf, "join") && (b = strchr(a, '\t')))
		{
			*b++ = 0;
			// touch_lan.c answers the server's challenge with a proof of it, for this server only.
			fe_set_private("_cl_touch_lan_password_server", a);
			fe_set_private("_cl_touch_lan_password", b);
		}
	}
	memset(buf, 0, sizeof(buf));
#endif
}

static void fe_mark_f(cmd_state_t *cmd)
{
	Con_Printf("@@FE %s\n", Cmd_Args(cmd));
}

static void fe_manager_f(cmd_state_t *cmd)
{
	if (fe_mode != FE_MANAGER || Cmd_Argc(cmd) < 2)
	{
		Con_Printf("fe_manager \"<commands>\": only with GODNOTIC_FRONTEND=manager\n");
		return;
	}
	dp_strlcpy(fe_manager_cmds, Cmd_Argv(cmd, 1), sizeof(fe_manager_cmds));
}

void TouchNativeFE_Init(void)
{
	const char *m = getenv("GODNOTIC_FRONTEND"), *r = getenv("GODNOTIC_FE_RESULT");
	Cmd_AddCommand(CF_CLIENT, "fe_mark", fe_mark_f, "native front end: print a marker line for the app");
	Cmd_AddCommand(CF_CLIENT, "fe_manager", fe_manager_f, "native front end manager mode: the commands to run once the game is up");
	if (!m || cls.state == ca_dedicated)
		return;
	if (!strcmp(m, "native"))
		fe_mode = FE_NATIVE;
	else if (!strcmp(m, "manager"))
		fe_mode = FE_MANAGER;
	else
		return;
	if (r && *r)
		dp_strlcpy(fe_result_path, r, sizeof(fe_result_path));
	if (fe_result_path[0])
		remove(fe_result_path);
	fe_read_secret();
	{
		const char *w = getenv("GODNOTIC_FE_WATCH_PID");
		fe_watch_pid = w ? atoi(w) : 0;
	}
	atexit(fe_atexit);
	fe_start_time = Sys_DirtyTime();
	Con_DPrintf("native front end: %s mode\n", fe_mode == FE_NATIVE ? "native" : "manager");
}

qbool TouchNativeFE_PasswordRejected(qbool wrong, const char *address)
{
	if (fe_mode != FE_NATIVE)
		return false;
	fe_password_kind = wrong ? 2 : 1;
	if (address && *address)
		dp_strlcpy(fe_server, address, sizeof(fe_server));
	return true;
}

// Prints a string as JSON (quotes, backslashes and control characters escaped).
static void fe_print_json_string(const char *prefix, const char *s)
{
	char out[16384];
	size_t n = 0;
	out[n++] = '"';
	for (; *s && n < sizeof(out) - 8; s++)
	{
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = c; }
		else if (c == '\n') { out[n++] = '\\'; out[n++] = 'n'; }
		else if (c == '\t') { out[n++] = '\\'; out[n++] = 't'; }
		else if (c < 32) { n += dpsnprintf(out + n, sizeof(out) - n, "\\u%04x", c); }
		else out[n++] = c;
	}
	out[n++] = '"';
	out[n] = 0;
	Con_Printf("%s%s\n", prefix, out);
}

static void fe_manager_frame(void)
{
	cvar_t *st;
	fe_frames++;
	// The Lua manager registers its commands on its first client frame; give it a few.
	if (fe_manager_stage == 0 && fe_frames >= 3)
	{
		char *p = fe_manager_cmds, *semi, mark[64];
		int n = 0;
		fe_manager_stage = 1;
		// one command per ';'-separated part, each between markers (no quoting of ';' needed: paths may not hold one)
		while (p && *p)
		{
			semi = strchr(p, ';');
			if (semi)
				*semi = 0;
			while (*p == ' ')
				p++;
			if (*p)
			{
				dpsnprintf(mark, sizeof(mark), "\nfe_mark BEGIN %d\n", n);
				Cbuf_AddText(cmd_local, mark);
				Cbuf_AddText(cmd_local, p);
				dpsnprintf(mark, sizeof(mark), "\nfe_mark END %d\n", n);
				Cbuf_AddText(cmd_local, mark);
				n++;
			}
			p = semi ? semi + 1 : NULL;
		}
		return;
	}
	if (fe_manager_stage == 1)
	{
		// wait while an install downloads or is checked
		st = Cvar_FindVar(&cvars_all, "_lua_install", ~0);
		if (st && (strstr(st->string, "state\tdownloading") || strstr(st->string, "state\tchecking"))
			&& Sys_DirtyTime() - fe_start_time < 120)
			return;
		fe_print_json_string("@@FE-STATE ", st ? st->string : "");
		fe_manager_stage = 2;
		fe_write_result("quit", "");
		Cbuf_AddText(cmd_local, "\nquit\n");
	}
}

void TouchNativeFE_Frame(void)
{
	qbool busy;
	double now;
	if (fe_mode == FE_OFF || fe_result_written)
		return;
#ifndef WIN32
	// Parent-death watchdog: the app quit or crashed without closing us (once a second).
	if (fe_watch_pid > 1 && Sys_DirtyTime() > fe_watch_next)
	{
		fe_watch_next = Sys_DirtyTime() + 1.0;
		if (kill(fe_watch_pid, 0) != 0 && errno == ESRCH)
		{
			fe_end("quit", "the app is gone");
			return;
		}
	}
#endif
	if (fe_mode == FE_MANAGER)
	{
		fe_manager_frame();
		return;
	}
	now = Sys_DirtyTime();
	busy = cls.state == ca_connected || cls.connect_trying || sv.active || cls.demoplayback;
	if (busy)
	{
		fe_seen_session = true;
		if (cls.signon == SIGNONS)
			fe_seen_signon = true;
		if (cls.netcon && cls.netcon->address[0] && LHNETADDRESS_GetAddressType(&cls.netcon->peeraddress) != LHNETADDRESSTYPE_LOOP)
			dp_strlcpy(fe_server, cls.netcon->address, sizeof(fe_server));
		fe_idle_frames = 0;
		return;
	}
	// A password refusal ends it at once: the app asks and starts again.
	if (fe_password_kind)
	{
		fe_end(fe_password_kind == 2 ? "password_wrong" : "password_required", cl_connect_status);
		return;
	}
	// Disconnected: a map change or a reconnect passes through this for a moment,
	// so it must last a few frames and a quarter second before it counts.
	if (!fe_idle_frames++)
		fe_idle_since = now;
	if (fe_idle_frames < 3 || now - fe_idle_since < 0.25)
		return;
	if (!fe_seen_session)
	{
		// Nothing ever started: a map that does not exist, a bad address.
		if (now - fe_start_time > 2)
			fe_end("error", cl_connect_status[0] ? cl_connect_status : "Nothing to start");
		return;
	}
	if (!fe_seen_signon)
		fe_end("connect_failed", cl_connect_status);
	else if (!strncmp(cl_connect_status, "Disconnect: ", 12))
		fe_end("kicked", cl_connect_status);
	else
		fe_end("left", cl_connect_status);
}
