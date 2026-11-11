/*
 * Engine side of the loader ABI (andysu_swap.h). Compiled into libmain.so with the rest of
 * darkplaces; libmain exports only andysu_engine_v1 (version script andysu_engine.map).
 *
 * run(): Sys_Main(). When the loader asks for a swap (swap_requested(), polled once per frame by
 * Sys_Frame -> Andysu_FrameHook) the engine saves a small JSON state, leaves the frame loop,
 * runs Host_Shutdown along the swap path (no SDL_Quit, no exit(); FS_Shutdown and all pools
 * freed) and Sys_Main returns ANDYSU_RUN_SWAP.
 */
#include "quakedef.h"
#include "andysu_engine.h"

extern void Andysu_InputDevices(int touch, int keyboard, int mouse); /* vid_touchdetect.c */

const andysu_loader_api *andysu_loader;
int andysu_swapping;
int andysu_leave;
int andysu_in_trial;
static int andysu_swap_count; /* how many swaps led to this engine (carried in the state) */

static int andysu_run(const andysu_loader_api *loader, int argc, char **argv, int swapped_in)
{
	andysu_loader = loader;
	andysu_in_trial = (swapped_in == 1);
	if (loader && loader->emit)
		loader->emit(swapped_in ? "{\"event\":\"engine_entry\",\"build_id\":\"" ANDYSU_BUILD_ID "\",\"swapped\":1}"
		                        : "{\"event\":\"engine_entry\",\"build_id\":\"" ANDYSU_BUILD_ID "\",\"swapped\":0}");
	return Sys_Main(argc, argv);
}

static const andysu_engine andysu_engine_desc = {
	ANDYSU_ENGINE_ABI, sizeof(andysu_engine), 1, ANDYSU_BUILD_ID, andysu_run, Andysu_InputDevices,
};

__attribute__((visibility("default"))) const andysu_engine *andysu_engine_v1(void)
{
	return &andysu_engine_desc;
}

// ------------------------------------------------------------------ state

static void Andysu_CaptureState(void)
{
	char connect[256] = "", map[MAX_QPATH] = "", json[768];
	int menu = (key_dest == key_menu || key_dest == key_menu_grabbed);

	if (sv.active)
		dp_strlcpy(map, sv.worldbasename, sizeof map);
	else if (cls.state == ca_connected && cls.netcon)
		LHNETADDRESS_ToString(&cls.netcon->peeraddress, connect, sizeof connect, true);
	dpsnprintf(json, sizeof json, "{\"v\":1,\"swap_count\":%d,\"from\":\"%s\",\"connect\":\"%s\",\"map\":\"%s\",\"menu\":%d}",
	           andysu_swap_count + 1, ANDYSU_BUILD_ID, connect, map, menu);
	andysu_loader->put_state(json, strlen(json));
	if (andysu_loader->emit)
	{
		char ev[1024];
		dpsnprintf(ev, sizeof ev, "{\"event\":\"engine_state_saved\",\"state\":%s}", json);
		andysu_loader->emit(ev);
	}
}

/* Minimal extraction from our own JSON: the value after "key": (string or integer). */
static qbool Andysu_JsonGet(const char *json, const char *key, char *out, size_t cap)
{
	char pat[64];
	const char *p;
	size_t n = 0;
	dpsnprintf(pat, sizeof pat, "\"%s\":", key);
	p = strstr(json, pat);
	if (!p)
		return false;
	p += strlen(pat);
	if (*p == '"')
	{
		for (p++; *p && *p != '"' && n + 1 < cap; p++)
			out[n++] = *p;
	}
	else
	{
		for (; *p && *p != ',' && *p != '}' && n + 1 < cap; p++)
			out[n++] = *p;
	}
	out[n] = 0;
	return true;
}

void Andysu_RestoreState(void)
{
	size_t len = 0;
	const char *state;
	char v[256], ev[1024];

	if (!andysu_loader)
		return;
#ifdef ANDYSU_FAIL_INIT
	// A deliberately broken engine build (build.sh variant ":fail"): its init always fails.
	Sys_Error("ANDYSU_FAIL_INIT: this engine build is deliberately broken");
#endif
	state = andysu_loader->get_state(&len);
	// TEST HOOK: the loader's test hook sets this to make the new engine fail its start
	// (rollback layer 2); only honoured during a trial.
	if (andysu_in_trial && getenv("ANDYSU_TEST_FAIL_INIT"))
		Sys_Error("ANDYSU test hook: forced failure of the new engine's init");
	if (!state || !len)
		return;
	if (Andysu_JsonGet(state, "swap_count", v, sizeof v))
		andysu_swap_count = atoi(v);
	if (Andysu_JsonGet(state, "connect", v, sizeof v) && v[0])
	{
		dpsnprintf(ev, sizeof ev, "connect %s\n", v);
		Cbuf_AddText(cmd_local, ev);
		Cbuf_Execute(cmd_local->cbuf);
	}
	else if (Andysu_JsonGet(state, "map", v, sizeof v) && v[0])
	{
		dpsnprintf(ev, sizeof ev, "map %s\n", v);
		Cbuf_AddText(cmd_local, ev);
		Cbuf_Execute(cmd_local->cbuf);
	}
	dpsnprintf(ev, sizeof ev, "{\"event\":\"engine_state_restored\",\"build_id\":\"" ANDYSU_BUILD_ID "\",\"state\":%s}", state);
	andysu_loader->emit(ev);
}

// ------------------------------------------------------------------ per frame

void Andysu_FrameHook(void)
{
	static unsigned int frames;
	char ev[160];

	if (!andysu_loader || host.state != host_active)
		return;
	if (andysu_loader->swap_requested() && !andysu_swapping)
	{
		Andysu_CaptureState();
		andysu_swapping = true;
		host.state = host_shutdown; // Sys_Frame shuts down through the swap path
		return;
	}
	frames++;
	if (frames % 120 == 0)
	{
		dpsnprintf(ev, sizeof ev, "{\"event\":\"engine_frames\",\"build_id\":\"" ANDYSU_BUILD_ID "\",\"frames\":%u}", frames);
		andysu_loader->emit(ev);
	}
#ifdef ANDYSU_CRASH_AFTER_START
	// Test variant (build.sh ":crash"): a native crash a few seconds into the engine's life, before it
	// would report healthy.
	if (frames == 60)
		*(volatile int *)0 = 1;
#endif
	// 90 frames show the engine draws. Any more only costs time on slow devices, where a good update
	// can miss the host's watchdog and roll back.
	if (frames == 90 && andysu_in_trial)
	{
		andysu_in_trial = false;
#ifndef ANDYSU_NO_HEALTHY // test variant (build.sh ":nohealthy"): never reports healthy
		andysu_loader->mark_healthy();
#endif
	}
}

void Andysu_AbortStart(const char *why)
{
	if (andysu_in_trial && andysu_loader && andysu_loader->abort_start)
		andysu_loader->abort_start(why);
}
