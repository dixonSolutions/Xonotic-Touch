/*
 * Include this BEFORE darkplaces.h because it breaks wrapping
 * _Static_assert. Cloudwalk has no idea how or why so don't ask.
 */
#include <SDL.h>

#include "darkplaces.h"

#ifdef WIN32
#ifdef _MSC_VER
#pragma comment(lib, "sdl2.lib")
#pragma comment(lib, "sdl2main.lib")
#endif
#endif


// =======================================================================
// General routines
// =======================================================================

void Sys_SDL_Shutdown(void)
{
	SDL_Quit();
}

// Sys_Error early in startup might screw with automated
// workflows or something if we show the dialog by default.
static qbool nocrashdialog = true;
void Sys_SDL_Dialog(const char *title, const char *string)
{
	if(!nocrashdialog)
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, string, NULL);
}

char *Sys_SDL_GetClipboardData (void)
{
	char *data = NULL;
	char *cliptext;

	cliptext = SDL_GetClipboardText();
	if (cliptext != NULL) {
		size_t allocsize;
		allocsize = min(MAX_INPUTLINE, strlen(cliptext) + 1);
		data = (char *)Z_Malloc (allocsize);
		dp_strlcpy (data, cliptext, allocsize);
		SDL_free(cliptext);
	}

	return data;
}

void Sys_SDL_Init(void)
{
	// we don't know which systems we'll want to init, yet...
	if (SDL_Init(0) < 0)
		Sys_Error("SDL_Init failed: %s\n", SDL_GetError());
#ifdef __ANDROID__
	// The game thread renders, simulates and reads input: it should not queue
	// behind background work. Refused quietly where Android does not allow it.
	SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif

	// COMMANDLINEOPTION: sdl: -nocrashdialog disables "Engine Error" crash dialog boxes
	if(!Sys_CheckParm("-nocrashdialog"))
		nocrashdialog = false;
}

#ifdef __ANDROID__
#include <dlfcn.h>
#include <unistd.h>
#include <sys/syscall.h>

/*
 * Android's performance hint (ADPF): tell the scheduler how long a frame may
 * take and how long each one really took, and it moves the game thread to the
 * cores and clocks that hold that pace -- big cores when a frame runs long,
 * little ones when there is slack. Without it a phone guesses from load, which
 * is where frames that are smooth one second and uneven the next come from,
 * on Exynos' big.LITTLE clusters as much as anywhere. Android 13+ (API 33);
 * looked up at run time so older Android simply goes without.
 */
typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;
static APerformanceHintSession *(*qAPerformanceHint_createSession)(APerformanceHintManager *, const int32_t *, size_t, int64_t);
static int (*qAPerformanceHint_updateTargetWorkDuration)(APerformanceHintSession *, int64_t);
static int (*qAPerformanceHint_reportActualWorkDuration)(APerformanceHintSession *, int64_t);
static APerformanceHintSession *perfhint_session;
static int64_t perfhint_target;
static qbool perfhint_tried;

static void Sys_PerfHint_Open(int64_t target)
{
	void *lib;
	APerformanceHintManager *(*getManager)(void);
	APerformanceHintManager *manager;
	int32_t tid = (int32_t)syscall(SYS_gettid);
	perfhint_tried = true;
	lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
	if (!lib)
		return;
	getManager = (APerformanceHintManager *(*)(void))dlsym(lib, "APerformanceHint_getManager");
	qAPerformanceHint_createSession = (APerformanceHintSession *(*)(APerformanceHintManager *, const int32_t *, size_t, int64_t))dlsym(lib, "APerformanceHint_createSession");
	qAPerformanceHint_updateTargetWorkDuration = (int (*)(APerformanceHintSession *, int64_t))dlsym(lib, "APerformanceHint_updateTargetWorkDuration");
	qAPerformanceHint_reportActualWorkDuration = (int (*)(APerformanceHintSession *, int64_t))dlsym(lib, "APerformanceHint_reportActualWorkDuration");
	if (!getManager || !qAPerformanceHint_createSession || !qAPerformanceHint_updateTargetWorkDuration || !qAPerformanceHint_reportActualWorkDuration)
	{
		Con_DPrintf("Performance hint: not on this Android\n");
		return;
	}
	manager = getManager();
	if (manager)
		perfhint_session = qAPerformanceHint_createSession(manager, &tid, 1, target);
	if (perfhint_session)
	{
		perfhint_target = target;
		Con_Printf("Performance hint: session open, %.1f ms a frame\n", target / 1e6);
	}
	else
		Con_Printf("Performance hint: unsupported on this device\n");
}

void Sys_PerfHint_Frame(double work, double target)
{
	int64_t target_ns = (int64_t)(target * 1e9);
	if (!sys_perfhint.integer || target <= 0)
		return;
	if (!perfhint_tried)
		Sys_PerfHint_Open(target_ns);
	if (!perfhint_session)
		return;
	// Retarget only on a real change (a new cl_maxfps or refresh rate): the
	// call is a binder transaction.
	if (target_ns != perfhint_target && (target_ns > perfhint_target * 21 / 20 || target_ns < perfhint_target * 19 / 20))
	{
		perfhint_target = target_ns;
		qAPerformanceHint_updateTargetWorkDuration(perfhint_session, target_ns);
	}
	if (work > 0)
		qAPerformanceHint_reportActualWorkDuration(perfhint_session, (int64_t)(work * 1e9));
}
#endif

qbool sys_supportsdlgetticks = true;
unsigned int Sys_SDL_GetTicks (void)
{
	return SDL_GetTicks();
}
void Sys_SDL_Delay (unsigned int milliseconds)
{
	SDL_Delay(milliseconds);
}

int main(int argc, char *argv[])
{
	return Sys_Main(argc, argv);
}
