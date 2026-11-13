/* Called only on the engine thread. The bridge is optional for older AndySU
 * hosts: their frozen manifest never starts the new Application and their
 * native library directory may not contain libnativeui.so. */
#include "quakedef.h"
#include <dlfcn.h>

typedef int (*nativeui_pop_fn)(char *, size_t);
typedef void (*nativeui_publish_fn)(const char *);
void NativeUI_Frame(void)
{
    static nativeui_pop_fn pop;
    static nativeui_publish_fn publish, publish_permissions;
    static unsigned retry;
    char command[1024];
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
    }
    while (pop(command, sizeof command))
        Cbuf_AddText(cmd_local, command);
    mods = Cvar_FindVar(&cvars_all, "_cl_lua_mods", ~0);
    publish(mods ? mods->string : "");
    mods = Cvar_FindVar(&cvars_all, "_cl_lua_permissions", ~0);
    publish_permissions(mods ? mods->string : "");
}
