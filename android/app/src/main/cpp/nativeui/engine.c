/* Called only on the engine thread. The bridge is optional for older AndySU
 * hosts: their frozen manifest never starts the new Application and their
 * native library directory may not contain libnativeui.so. */
#include "quakedef.h"
#include <dlfcn.h>
#include <GLES2/gl2.h>

#include "touch_frontend.h"

typedef int (*nativeui_pop_fn)(char *, size_t);

/* The front-end bridge's sink (touch_frontend.c): only a bridge built with it
 * (nativeui_fe_version) gets one. An engine swapped in under an older resident
 * bridge, or a bridge under an older engine, simply has no native front end,
 * and the QuakeC menu stays in charge. */
static void NativeUI_AttachFrontend(void *bridge)
{
    int (*version)(void) = (int (*)(void))dlsym(bridge, "nativeui_fe_version");
    touchfe_sink_t sink;
    if (!version || version() < TOUCHFE_PROTOCOL)
        return;
    sink.put = (void (*)(const char *, const char *, int))dlsym(bridge, "nativeui_fe_put");
    sink.beat_age_ms = (long long (*)(void))dlsym(bridge, "nativeui_fe_beat_age_ms");
    sink.cancel_requested = (int (*)(void))dlsym(bridge, "nativeui_fe_cancel_requested");
    if (sink.put && sink.beat_age_ms && sink.cancel_requested)
        TouchFE_SetSink(&sink);
}
typedef void (*nativeui_publish_fn)(const char *);
void NativeUI_Frame(void)
{
    static nativeui_pop_fn pop;
    static nativeui_publish_fn publish, publish_permissions;
    static unsigned retry;
    static char command[8192];
    cvar_t *mods;
    if (!pop || !publish || !publish_permissions)
    {
        void *bridge;
        if (retry++ % 60) return;
        bridge = dlopen("libnativeui.so", RTLD_NOW | RTLD_NOLOAD);
        if (!bridge) return;
        pop = (nativeui_pop_fn)dlsym(bridge, "nativeui_pop");
        publish = (nativeui_publish_fn)dlsym(bridge, "nativeui_publish");
        publish_permissions = (nativeui_publish_fn)dlsym(bridge, "nativeui_publish_permissions");
        if (!pop || !publish || !publish_permissions) { dlclose(bridge); return; }
        /* Keep this reference: JNI may use the resident bridge throughout a swap. */
        NativeUI_AttachFrontend(bridge);
    }
    while (pop(command, sizeof command))
        Cbuf_AddText(cmd_local, command);
    mods = Cvar_FindVar(&cvars_all, "_cl_lua_mods", ~0);
    publish(mods ? mods->string : "");
    mods = Cvar_FindVar(&cvars_all, "_cl_lua_permissions", ~0);
    publish_permissions(mods ? mods->string : "");
}

/* Before each buffer swap until the first frame that shows something: the
 * engine's first swaps are cleared black, and lifting the Android boot
 * picture over those is the black gap this is here to close. A row across
 * the middle of the back buffer is read until it has a lit pixel (the
 * loading picture's banner, or the menu). */
void NativeUI_Presented(void)
{
    static int done, frames;
    static unsigned char row[4 * 2048];
    void *bridge;
    void (*presented)(void);
    int w, i, lit = 0;
    if (done)
        return;
    w = vid.mode.width < 2048 ? vid.mode.width : 2048;
    if (w > 0 && vid.mode.height > 0)
    {
        glReadPixels(0, vid.mode.height / 2, w, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
        for (i = 0; i < w && !lit; i++)
            lit = row[4 * i] > 24 || row[4 * i + 1] > 24 || row[4 * i + 2] > 24;
    }
    if (!lit && ++frames < 900)
        return;
    done = 1;
    bridge = dlopen("libnativeui.so", RTLD_NOW | RTLD_NOLOAD);
    if (!bridge)
        return; /* an old host that never loaded the bridge: nothing waits */
    presented = (void (*)(void))dlsym(bridge, "nativeui_presented");
    if (presented)
        presented();
}
