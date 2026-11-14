// LD_PRELOAD test shim for scripts/test-touch-controls-live.sh: scripted fingers,
// mouse, keys and console commands for an offscreen DarkPlaces run, through
// the engine's real SDL event path (no code in the engine for it).
// $XT_INJECT lines, T = seconds after the cvar xt_go turns non-zero:
//   T fdown ID X Y          finger down, X Y normalised 0..1
//   T fmove ID X Y
//   T fup ID X Y
//   T glide ID X1 Y1 X2 Y2 DUR [up]   down at X1 Y1, moves to X2 Y2 over DUR s at 60 Hz, then up if "up"
//   T mmove X Y             mouse to window pixel X Y
//   T mdown / T mup         left button
//   T key SYM               SDL keycode, pressed and released
//   T cmd TEXT...           console text (Cbuf_AddText)
// $XT_REL=1 reports the mouse as relative (grabbed).
#define _GNU_SOURCE
#include <SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

typedef struct { double t; int type; int id; float x, y; char text[200]; } ev_t;
#define MAXEV 20000
static ev_t evs[MAXEV];
static int nev = -1, next;
static double t0 = -1;
static int mx, my, mb;

enum { E_FDOWN = 1, E_FMOVE, E_FUP, E_MMOVE, E_MDOWN, E_MUP, E_KEYD, E_KEYU, E_CMD };

static double now(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec + tv.tv_usec / 1e6; }

static void add(double t, int type, int id, float x, float y, const char *text)
{
	if (nev >= MAXEV) return;
	ev_t *e = &evs[nev++];
	memset(e, 0, sizeof(*e));
	e->t = t; e->type = type; e->id = id; e->x = x; e->y = y;
	if (text) snprintf(e->text, sizeof(e->text), "%s", text);
}

static int cmp(const void *a, const void *b)
{
	const ev_t *x = a, *y = b;
	if (x->t != y->t) return x->t < y->t ? -1 : 1;
	return x < y ? -1 : 1;
}

static void load(void)
{
	nev = 0;
	const char *f = getenv("XT_INJECT");
	FILE *fp = f ? fopen(f, "r") : NULL;
	char l[512], w[32];
	if (!fp) return;
	while (fgets(l, sizeof l, fp))
	{
		double t; int id, n; float x, y, x2, y2, dur;
		char *p = l;
		if (l[0] == '#' || sscanf(p, "%lf %31s %n", &t, w, &n) < 2) continue;
		p += n;
		if (!strcmp(w, "fdown") && sscanf(p, "%d %f %f", &id, &x, &y) == 3) add(t, E_FDOWN, id, x, y, 0);
		else if (!strcmp(w, "fmove") && sscanf(p, "%d %f %f", &id, &x, &y) == 3) add(t, E_FMOVE, id, x, y, 0);
		else if (!strcmp(w, "fup") && sscanf(p, "%d %f %f", &id, &x, &y) == 3) add(t, E_FUP, id, x, y, 0);
		else if (!strcmp(w, "glide") && sscanf(p, "%d %f %f %f %f %f", &id, &x, &y, &x2, &y2, &dur) == 6)
		{
			int steps = (int)(dur * 60) + 1, i;
			add(t, E_FDOWN, id, x, y, 0);
			for (i = 1; i <= steps; i++)
				add(t + dur * i / steps, E_FMOVE, id, x + (x2 - x) * i / steps, y + (y2 - y) * i / steps, 0);
			if (strstr(p, "up")) add(t + dur + 0.02, E_FUP, id, x2, y2, 0);
		}
		else if (!strcmp(w, "mmove") && sscanf(p, "%f %f", &x, &y) == 2) add(t, E_MMOVE, 0, x, y, 0);
		else if (!strcmp(w, "mdown")) add(t, E_MDOWN, 0, 0, 0, 0);
		else if (!strcmp(w, "mup")) add(t, E_MUP, 0, 0, 0, 0);
		else if (!strcmp(w, "key") && sscanf(p, "%d", &id) == 1) { add(t, E_KEYD, id, 0, 0, 0); add(t + 0.1, E_KEYU, id, 0, 0, 0); }
		else if (!strcmp(w, "cmd")) { p[strcspn(p, "\n")] = 0; add(t, E_CMD, 0, 0, 0, p); }
	}
	fclose(fp);
	qsort(evs, nev, sizeof(evs[0]), cmp);
}

typedef struct cvar_state_s cvar_state_t;
typedef struct cmd_state_s cmd_state_t;

static float go_value(void)
{
	static float (*vv)(cvar_state_t *, const char *, unsigned);
	static cvar_state_t *cvars;
	if (!vv) { vv = dlsym(RTLD_DEFAULT, "Cvar_VariableValue"); cvars = dlsym(RTLD_DEFAULT, "cvars_all"); }
	return vv && cvars ? vv(cvars, "xt_go", ~0u) : 0;
}

static void console(const char *text)
{
	static void (*add_text)(cmd_state_t *, const char *);
	static cmd_state_t **local;
	char buf[256];
	if (!add_text) { add_text = dlsym(RTLD_DEFAULT, "Cbuf_AddText"); local = dlsym(RTLD_DEFAULT, "cmd_local"); }
	if (!add_text || !local) return;
	snprintf(buf, sizeof buf, "%s\n", text);
	add_text(*local, buf);
}

int SDL_PollEvent(SDL_Event *ev)
{
	static int (*real)(SDL_Event *);
	if (!real) real = dlsym(RTLD_NEXT, "SDL_PollEvent");
	if (nev < 0) load();
	if (t0 < 0 && nev > 0 && go_value() != 0) t0 = now();
	while (t0 >= 0 && next < nev && now() - t0 >= evs[next].t)
	{
		ev_t *e = &evs[next++];
		memset(ev, 0, sizeof(*ev));
		switch (e->type)
		{
		case E_FDOWN: case E_FMOVE: case E_FUP:
			ev->type = e->type == E_FDOWN ? SDL_FINGERDOWN : e->type == E_FMOVE ? SDL_FINGERMOTION : SDL_FINGERUP;
			ev->tfinger.touchId = 1; ev->tfinger.fingerId = e->id; ev->tfinger.x = e->x; ev->tfinger.y = e->y; ev->tfinger.pressure = 1;
			return 1;
		case E_MMOVE:
		{
			int dx = (int)e->x - mx, dy = (int)e->y - my;
			mx = (int)e->x; my = (int)e->y;
			ev->type = SDL_MOUSEMOTION; ev->motion.x = mx; ev->motion.y = my; ev->motion.xrel = dx; ev->motion.yrel = dy; ev->motion.windowID = 1;
			ev->motion.state = mb ? SDL_BUTTON_LMASK : 0;
			return 1;
		}
		case E_MDOWN: case E_MUP:
			mb = e->type == E_MDOWN;
			ev->type = mb ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
			ev->button.x = mx; ev->button.y = my; ev->button.button = SDL_BUTTON_LEFT; ev->button.state = mb; ev->button.clicks = 1; ev->button.windowID = 1;
			return 1;
		case E_KEYD: case E_KEYU:
			ev->type = e->type == E_KEYD ? SDL_KEYDOWN : SDL_KEYUP;
			ev->key.keysym.sym = e->id; ev->key.keysym.scancode = SDL_GetScancodeFromKey(e->id);
			ev->key.state = e->type == E_KEYD; ev->key.windowID = 1;
			return 1;
		case E_CMD:
			console(e->text);
			continue;
		}
	}
	return real(ev);
}

Uint32 SDL_GetMouseState(int *x, int *y) { if (x) *x = mx; if (y) *y = my; return mb ? SDL_BUTTON_LMASK : 0; }
int SDL_GetNumTouchDevices(void) { return getenv("XT_NOTOUCH") ? 0 : 1; }
SDL_TouchID SDL_GetTouchDevice(int i) { (void)i; return 1; }
SDL_TouchDeviceType SDL_GetTouchDeviceType(SDL_TouchID id) { (void)id; return SDL_TOUCH_DEVICE_DIRECT; }
