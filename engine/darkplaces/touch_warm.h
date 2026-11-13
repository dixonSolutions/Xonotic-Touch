#ifndef TOUCH_WARM_H
#define TOUCH_WARM_H

// Godnotic: prepare the next local game while the menu is idle (touch_warm.c).

void TouchWarm_Init(void);   // cl_screen.c CL_Screen_Init
void TouchWarm_Frame(void);  // cl_screen.c, after every drawn frame
void TouchWarm_Input(void);  // keys.c Key_Event: the player is using the menu

// helpers in the lazy loaders' files: 1 loaded now, 0 already loaded, -1 none
int TouchModel_WarmName(const char *name);  // touch_loadmodel.h
int TouchSnd_WarmName(const char *name);    // touch_loadsnd.h
// touch_preload.h: the entries the next game on map would preload
int TouchPreload_ForEach(const char *map, void (*fn)(char kind, const char *name, void *ctx), void *ctx);

// touch_preload.h: a persistent picture loaded during a map load, and its cost
void TouchWarm_PicLoaded(const char *name, unsigned int cachepicflags, double seconds);

// fs.c (touch_assets_fs.h): fs_zstd_lazy
void Touch_FS_Init(void);

#endif
