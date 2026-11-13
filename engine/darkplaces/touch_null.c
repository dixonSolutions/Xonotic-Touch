/*
 * touch_null.c -- what the dedicated server build (make sv-release) lacks
 * (Godnotic).
 *
 * The client build gets these from vid_sdl.c and snd_main.c. Touch code in
 * OBJ_COMMON (touch_hud.c, touch_lua_cl.c, clvm_cmds.c, touch_loadtime.c)
 * refers to them, so a server binary links vid_null.c and snd_null.c plus
 * this file. Nothing here runs: a dedicated server has no fingers and no
 * sound.
 */
#include "quakedef.h"
#include "touch_loadtime.h"
#include "touch_warm.h"

#define TOUCH_NULL_FINGERS 11
float multitouch[TOUCH_NULL_FINGERS][3];

void VID_GetTouchFinger(int slot, int *active, float *nx, float *ny)
{
	if (active)
		*active = 0;
	if (nx)
		*nx = 0;
	if (ny)
		*ny = 0;
}

qbool TouchSnd_PreloadName(const char *name)
{
	return false;
}

int TouchSnd_WarmName(const char *name)
{
	return 0;
}
