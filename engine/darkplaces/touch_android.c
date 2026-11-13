/*
Godnotic: the Android app in the background (Home, the app switcher,
another app on top, the screen turned off).

SDL's default is to block the main thread inside its event pump from
onPause / onStop until the app returns. Nothing read or sent a packet
meanwhile, so a server dropped the player after net_messagetimeout (30 s in
Xonotic), and a join, its curl downloads and the map load stood still.

With SDL_HINT_ANDROID_BLOCK_ON_PAUSE 0 (TouchAndroid_Init) the main loop keeps
running. SDL then unbinds the GL context in the pump after the pause, and the
window surface goes away; vid_hidden keeps CL_UpdateScreen from drawing and
the frame rate falls to cl_maxidlefps. The network, curl and the QC carry on.
So that a map that finishes downloading can still load (its textures go to
GL), the context is bound again without a surface where EGL allows it
(EGL_KHR_surfaceless_context); where it does not, the load waits for the
return (TouchAndroid_HoldMapLoad). SDL binds the context to the new window
surface on the return, before the engine hears of it.

With nothing to keep going -- no server, no join, no download -- and in a
local game, the main thread waits for the return as it always did: a local
match is paused while the player is away, and an idle menu costs nothing.
A LAN game the player hosts on purpose (started from the Host screen) or a
local game another human has joined keeps running instead, like a
connection: the players on it would otherwise time out. touch_netstate then
says "hosting", and XonoticActivity holds a Wi-Fi lock and a multicast lock
(LanLocks.java) for as long as it does. A bot match from Create or
Singleplayer pauses whatever its number of player slots
(TouchLAN_KeepRunningInBackground in touch_lan_logic.h).

<userdir>/data/touch_netstate tells the Java side what the engine is doing
(XonoticActivity.watchNetState), which keeps the process alive with a
foreground service while there is something to keep alive.
*/

#include "quakedef.h"
#include "touch_android.h"

#ifdef __ANDROID__
#include <SDL.h>
#include <EGL/egl.h>
#include "libcurl.h"
#include "touch_hud.h"
#include "touch_lan.h"
#include "touch_lan_logic.h"
#include "touch_net.h"

static qbool away;
static EGLDisplay away_display = EGL_NO_DISPLAY;
static EGLContext away_context = EGL_NO_CONTEXT;
static qbool away_surfaceless;   // EGL can bind the context without a surface
static qbool load_held, load_resume;
static qbool netstate_busy;      // connected, joining or downloading
void CL_ResumeHeldLoad(void);    // cl_parse.c

void TouchAndroid_Init(void)
{
	SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
}

void TouchAndroid_Background(qbool goaway)
{
	if (goaway == away)
		return;
	away = goaway;
	vid_hidden = goaway;
	TouchNet_Away(goaway); // spectate after a while away, reconnect checks
	if (goaway)
	{
		const char *ext;
		// Still current here: SDL unbinds it in a later pump.
		away_display = eglGetCurrentDisplay();
		away_context = eglGetCurrentContext();
		ext = away_display != EGL_NO_DISPLAY ? eglQueryString(away_display, EGL_EXTENSIONS) : NULL;
		away_surfaceless = ext && strstr(ext, "EGL_KHR_surfaceless_context");
		Con_Printf("Android: in the background%s\n", netstate_busy ? ", staying connected" : "");
		// The fingers and keys held when the app left never send their release.
		TouchHUD_ReleaseAll();
		Key_ReleaseAll();
	}
	else
	{
		Con_Printf("Android: back in the foreground\n");
		away_context = EGL_NO_CONTEXT;
		if (load_held)
		{
			load_held = false;
			load_resume = true;
		}
	}
}

qbool TouchAndroid_HoldMapLoad(void)
{
	// Nothing bound: no context to give the map's textures to.
	if (!away || eglGetCurrentContext() != EGL_NO_CONTEXT)
		return false;
	if (!load_held)
		Con_Printf("Android: in the background; the map loads on return\n");
	load_held = true;
	return true;
}

// Once SDL has unbound the context, bind it again without a surface.
static void rebind_context(void)
{
	if (!away || away_context == EGL_NO_CONTEXT || eglGetCurrentContext() != EGL_NO_CONTEXT)
		return;
	if (away_surfaceless && eglMakeCurrent(away_display, EGL_NO_SURFACE, EGL_NO_SURFACE, away_context))
		Con_DPrintf("Android: GL context kept without a surface\n");
	else
		Con_Printf("Android: no GL context in the background; a map load waits for the return\n");
	away_context = EGL_NO_CONTEXT; // once per stay
}

// <userdir>/data/touch_netstate: a kind (connected, joining, downloading,
// idle) and a line for the notification. Written only when it changes.
static void publish_netstate(void)
{
	static char last[256] = "-";
	char state[256], addr[128] = "";
	qfile_t *f;
	qbool remote = !sv.active && !cls.demoplayback;
	if (remote && cls.state == ca_connected && cls.netcon)
		LHNETADDRESS_ToString(&cls.netcon->peeraddress, addr, sizeof(addr), true);
	else if (cls.connect_trying)
		LHNETADDRESS_ToString(&cls.connect_address, addr, sizeof(addr), true);
	netstate_busy = true;
	if (TouchNet_ReconnectState())
		dpsnprintf(state, sizeof(state), "joining\n%s\n", TouchNet_ReconnectState());
	else if (remote && (cls.connect_trying || (cls.state == ca_connected && cls.signon < SIGNONS)))
		dpsnprintf(state, sizeof(state), "joining\nJoining %s%s\n", addr, Curl_Running() ? ", downloading" : "");
	else if (remote && cls.state == ca_connected)
		dpsnprintf(state, sizeof(state), "connected\nConnected to %s%s%s\n", addr, cl.worldbasename[0] ? " - " : "", cl.worldbasename);
	else if (sv.active && !cls.demoplayback && TouchLAN_KeepRunningInBackground(1, TouchLAN_HostingOnPurpose(), TouchLAN_OtherHumans(), 0))
	{
		// A LAN game hosted from the Host screen, or a local game another
		// player joined (touch_lan.c): it must keep running, and the Java
		// side holds Wi-Fi and multicast locks while it does.
		char portstr[16] = "";
		cvar_t *port = Cvar_FindVar(&cvars_all, "touch_lan_port", ~0);
		if (port && port->integer > 0)
			dpsnprintf(portstr, sizeof(portstr), " on port %d", port->integer);
		dpsnprintf(state, sizeof(state), "hosting\nHosting a LAN game%s%s%s\n", portstr, cl.worldbasename[0] ? " - " : "", cl.worldbasename);
	}
	else if (Curl_Running())
		dp_strlcpy(state, "downloading\nDownloading\n", sizeof(state));
	else
	{
		dp_strlcpy(state, "idle\n\n", sizeof(state));
		netstate_busy = false;
	}
	if (Curl_Running())
	{
		// a third line for the notification: the download's size, speed and time left
		// (once a second: the file is not rewritten every frame)
		static char line[256];
		static double line_time;
		if (host.realtime - line_time >= 1 || !line[0])
		{
			int n = 0;
			char addinfo[128];
			const char *more = NULL;
			Curl_downloadinfo_t *info = Curl_GetDownloadInfo(&n, &more, addinfo, sizeof(addinfo));
			line[0] = 0;
			if (info && n > 0)
			{
				dpsnprintf(line, sizeof(line), "Downloading %s", info[0].filename);
				TouchNet_CurlText(line, sizeof(line), 0, info[0].filename, info[0].queued, info[0].progress, info[0].speed);
			}
			if (info)
				Z_Free(info);
			line_time = host.realtime;
		}
		if (line[0])
		{
			dp_strlcat(state, line, sizeof(state));
			dp_strlcat(state, "\n", sizeof(state));
		}
	}
	if (!strcmp(state, last))
		return;
	f = FS_OpenRealFile("touch_netstate", "wb", false);
	if (!f)
		return;
	FS_Print(f, state);
	FS_Close(f);
	dp_strlcpy(last, state, sizeof(last));
}

// Away with nothing to keep going, or in a local game: wait for the return.
// The events stay queued for the next frame's loop, which hears the return.
static void wait_while_idle(void)
{
	SDL_Event event;
	if (!away)
		return;
	if (sv.active && !cls.demoplayback
		? TouchLAN_KeepRunningInBackground(1, TouchLAN_HostingOnPurpose(), TouchLAN_OtherHumans(), 0)
		: TouchLAN_KeepRunningInBackground(0, 0, 0, netstate_busy))
		return; // a server, a join, a download, a LAN game we host or one others joined: keep going
	Con_Printf(sv.active ? "Android: local game paused in the background\n" : "Android: idle in the background\n");
	while (host.state == host_active)
	{
		SDL_PumpEvents(); // also lets SDL unbind the context the surface needs
		if (SDL_PeepEvents(&event, 1, SDL_PEEKEVENT, SDL_QUIT, SDL_QUIT) > 0
			|| SDL_PeepEvents(&event, 1, SDL_PEEKEVENT, SDL_APP_TERMINATING, SDL_APP_TERMINATING) > 0
			|| SDL_PeepEvents(&event, 1, SDL_PEEKEVENT, SDL_APP_DIDENTERFOREGROUND, SDL_APP_DIDENTERFOREGROUND) > 0)
			break;
		SDL_Delay(250);
	}
}

void TouchAndroid_AfterEvents(void)
{
	if (away)
		vid_hidden = true; // whatever window events came in meanwhile
	publish_netstate();
	wait_while_idle();
	rebind_context();
	if (load_resume)
	{
		load_resume = false;
		CL_ResumeHeldLoad();
	}
}
#endif
