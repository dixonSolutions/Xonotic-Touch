/*
Godnotic UI bridge: the local stream transport (docs/UI_BRIDGE.md 9.2).

  -uibridge fd:<n>          inherited AF_UNIX socket (one end of a socketpair)
  -uibridge stdio           stdin/stdout (console output goes to stderr)
  -uibridge listen[:<path>] Unix socket, default $XDG_RUNTIME_DIR/godnotic/uibridge.sock (0600 in 0700)

NDJSON both ways, non-blocking, on the engine thread. The connection is an ordinary
TouchUIB_Open client, so the stream and the in-process API behave the same.

Back-end mode (any -uibridge): the menu VM describes but does not draw while a native
renderer is attached, the window is hidden while the app context is "menu" or
"loading" (shown for "game"), and the engine quits cleanly when the front end goes
away (fd/stdio at once; listen after 10 s without a client).
*/
#include "quakedef.h"
#include "touch_uib_int.h"

#if !defined(WIN32) && !defined(__EMSCRIPTEN__)
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#define UIB_STREAM 1
#endif

#if defined(CONFIG_MENU) && defined(__has_include) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
#if __has_include(<SDL.h>)
#include <SDL.h>
#define UIB_SDL_WINDOW 1
#endif
#endif

static struct
{
	int mode;               // 0 off, 1 fd, 2 stdio, 3 listen
	int rfd, wfd, lfd;
	char path[256];
	touchuib_conn_t *conn;
	char *in;
	size_t inlen, incap;
	char *out;
	size_t outlen, outoff, outcap;
	double lost_at;         // listen: when the last client left
	int ever_connected;
	int window_hidden;
	double start_time;
	int first_hello_logged;
} sm = {0, -1, -1, -1};

int UIBStream_BackendMode(void) { return sm.mode != 0; }
int UIBStream_Active(void) { return sm.conn != NULL; }

#ifdef UIB_STREAM
static void nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0)
		fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void attach(int rfd, int wfd)
{
	sm.rfd = rfd;
	sm.wfd = wfd;
	nonblock(rfd);
	if (wfd != rfd)
		nonblock(wfd);
	sm.conn = TouchUIB_Open("linux", 5);
	sm.inlen = sm.outlen = sm.outoff = 0;
	sm.ever_connected = 1;
}

static void detach(const char *why)
{
	Con_DPrintf("uibridge: front end disconnected (%s)\n", why);
	if (sm.conn)
		TouchUIB_Close(sm.conn);
	sm.conn = NULL;
	if (sm.mode == 3)
	{
		if (sm.rfd >= 0)
			close(sm.rfd);
		sm.rfd = sm.wfd = -1;
		sm.lost_at = Sys_DirtyTime();
		return;
	}
	// fd/stdio: the front end owns us; leave cleanly (saves the config)
	sm.rfd = sm.wfd = -1;
	Cbuf_AddText(cmd_local, "\nquit\n");
}

static void read_input(void)
{
	for (;;)
	{
		if (sm.incap - sm.inlen < 65536)
		{
			sm.incap = sm.incap ? sm.incap * 2 : 131072;
			if (sm.incap > UIB_MAX_MESSAGE * 2 + 65536)
			{
				detach("message too large");
				return;
			}
			sm.in = (char *)realloc(sm.in, sm.incap);
		}
		ssize_t n = read(sm.rfd, sm.in + sm.inlen, sm.incap - sm.inlen - 1);
		if (n > 0)
		{
			sm.inlen += n;
			continue;
		}
		if (n == 0)
		{
			detach("end of stream");
			return;
		}
		if (errno == EINTR)
			continue;
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			detach(strerror(errno));
		break;
	}
	size_t start = 0;
	for (size_t i = 0; i < sm.inlen; i++)
		if (sm.in[i] == '\n')
		{
			size_t len = i - start;
			if (len && !(len == 1 && sm.in[start] == '\r'))
			{
				int r = TouchUIB_Post(sm.conn, sm.in + start, len);
				if (r == -3)
					Con_Printf("uibridge: dropped a message over 4 MiB\n");
			}
			start = i + 1;
		}
	memmove(sm.in, sm.in + start, sm.inlen - start);
	sm.inlen -= start;
}

static void write_output(void)
{
	char *m;
	size_t len;
	while ((m = TouchUIB_Take(sm.conn, &len)))
	{
		if (sm.outlen + len + 1 > sm.outcap)
		{
			while (sm.outlen + len + 1 > sm.outcap)
				sm.outcap = sm.outcap ? sm.outcap * 2 : 262144;
			sm.out = (char *)realloc(sm.out, sm.outcap);
		}
		memcpy(sm.out + sm.outlen, m, len);
		sm.outlen += len;
		sm.out[sm.outlen++] = '\n';
		TouchUIB_Free(m);
	}
	while (sm.outoff < sm.outlen)
	{
		ssize_t n = write(sm.wfd, sm.out + sm.outoff, sm.outlen - sm.outoff);
		if (n > 0)
		{
			sm.outoff += n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		detach(n < 0 ? strerror(errno) : "write failed");
		return;
	}
	if (sm.outoff == sm.outlen)
		sm.outoff = sm.outlen = 0;
}

static int open_listen(const char *path)
{
	char dir[256];
	dp_strlcpy(dir, path, sizeof(dir));
	char *slash = strrchr(dir, '/');
	if (slash)
	{
		*slash = 0;
		mkdir(dir, 0700);
		chmod(dir, 0700);
	}
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_un sa;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(sa.sun_path))
	{
		close(fd);
		return -1;
	}
	dp_strlcpy(sa.sun_path, path, sizeof(sa.sun_path));
	struct stat stt;
	if (!lstat(path, &stt) && S_ISSOCK(stt.st_mode))
		unlink(path);
	mode_t old = umask(0177);
	int r = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
	umask(old);
	if (r < 0 || listen(fd, 1) < 0)
	{
		close(fd);
		return -1;
	}
	chmod(path, 0600);
	nonblock(fd);
	return fd;
}
#endif

// The QuakeC renderer's Copy: the menu VM sets _uib_clipboard to the text (a
// cvar_set, so the text is never parsed as a command) and the engine puts it on
// the system clipboard. _uib_clipboard_ok says whether this build can.
static cvar_t uib_clipboard = {CF_CLIENT, "_uib_clipboard", "", "set by the menu: text to put on the system clipboard"};
static cvar_t uib_clipboard_ok = {CF_CLIENT | CF_READONLY, "_uib_clipboard_ok", "0", "1 when this build can write the system clipboard"};
static void UIB_Clipboard_c(cvar_t *var)
{
#ifdef UIB_SDL_WINDOW
	if (var->string[0])
		SDL_SetClipboardText(var->string);
#else
	(void)var;
#endif
}

void UIBStream_Init(void)
{
	Cvar_RegisterVariable(&uib_clipboard);
	Cvar_RegisterCallback(&uib_clipboard, UIB_Clipboard_c);
	Cvar_RegisterVariable(&uib_clipboard_ok);
#ifdef UIB_SDL_WINDOW
	Cvar_SetQuick(&uib_clipboard_ok, "1");
#endif
	int i = Sys_CheckParm("-uibridge");
	if (!i)
		return;
	const char *spec = i + 1 < sys.argc ? sys.argv[i + 1] : "listen";
	sm.start_time = Sys_DirtyTime();
#ifdef UIB_STREAM
	if (!strncmp(spec, "fd:", 3))
	{
		int fd = atoi(spec + 3);
		if (fd < 3 || fcntl(fd, F_GETFD) < 0)
		{
			Con_Printf("uibridge: -uibridge %s: not an open descriptor\n", spec);
			return;
		}
		fcntl(fd, F_SETFD, FD_CLOEXEC);
		sm.mode = 1;
		attach(fd, fd);
	}
	else if (!strcmp(spec, "stdio"))
	{
		sm.mode = 2;
		sys.outfd = 2;  // console output must not mix with the protocol
		attach(0, 1);
	}
	else if (!strncmp(spec, "listen", 6))
	{
		if (spec[6] == ':' && spec[7])
			dp_strlcpy(sm.path, spec + 7, sizeof(sm.path));
		else
		{
			const char *rt = getenv("XDG_RUNTIME_DIR");
			dpsnprintf(sm.path, sizeof(sm.path), "%s/godnotic/uibridge.sock", rt && *rt ? rt : "/tmp");
		}
		sm.lfd = open_listen(sm.path);
		if (sm.lfd < 0)
		{
			Con_Printf("uibridge: cannot listen on %s: %s\n", sm.path, strerror(errno));
			return;
		}
		sm.mode = 3;
		sm.lost_at = Sys_DirtyTime();
		Con_Printf("uibridge: listening on %s\n", sm.path);
	}
	else
		Con_Printf("uibridge: unknown -uibridge %s (fd:<n>, stdio, listen[:<path>])\n", spec);
#else
	Con_Printf("uibridge: the stream transport is not available on this platform\n");
#endif
}

static void window_policy(void)
{
#ifdef UIB_SDL_WINDOW
	if (!sm.mode)
		return;
	uibnode_t *app = UIBS_Find("app");
	size_t l;
	const char *ctx = app ? UIBS_PropJSON(app, "context", &l) : NULL;
	int want_hidden = sm.conn && (!ctx || strncmp(ctx, "\"game\"", 6));
	if (want_hidden == sm.window_hidden)
		return;
	SDL_Window *w = SDL_GL_GetCurrentWindow();
	if (!w)
		return;
	if (want_hidden)
		SDL_HideWindow(w);
	else
	{
		SDL_ShowWindow(w);
		SDL_RaiseWindow(w);
	}
	sm.window_hidden = want_hidden;
#endif
}

void UIBStream_Frame(void)
{
#ifdef UIB_STREAM
	if (!sm.mode)
		return;
	if (sm.mode == 3 && !sm.conn && sm.lfd >= 0)
	{
		int c = accept(sm.lfd, NULL, NULL);
		if (c >= 0)
		{
			fcntl(c, F_SETFD, FD_CLOEXEC);
			attach(c, c);
			Con_DPrintf("uibridge: front end connected\n");
		}
		else if (Sys_DirtyTime() - sm.lost_at > 10.0)
		{
			Con_Printf("uibridge: no front end for 10 s, quitting\n");
			sm.lost_at = 1e30;
			Cbuf_AddText(cmd_local, "\nquit\n");
		}
	}
	if (sm.conn)
	{
		read_input();
		if (sm.conn)
			write_output();
	}
	if (!sm.first_hello_logged && sm.conn && UIB_ConnCount())
	{
		sm.first_hello_logged = 1;
		Con_DPrintf("uibridge: first description sent %.0f ms after the transport started\n", (Sys_DirtyTime() - sm.start_time) * 1000.0);
	}
#endif
	window_policy();
}

void UIBStream_Shutdown(void)
{
#ifdef UIB_STREAM
	if (sm.conn)
	{
		write_output();
		TouchUIB_Close(sm.conn);
		sm.conn = NULL;
	}
	if (sm.lfd >= 0)
	{
		close(sm.lfd);
		unlink(sm.path);
	}
#endif
	sm.mode = 0;
}
