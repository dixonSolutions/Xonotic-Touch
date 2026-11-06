/* Engine-internal declarations for the AndySU loader ABI (andysu_engine.c). Included by the few
 * engine files that take part in a live swap, always under #ifdef ANDYSU_SWAP. */
#ifndef ANDYSU_ENGINE_H
#define ANDYSU_ENGINE_H

#include "andysu_swap.h"

extern const andysu_loader_api *andysu_loader;
extern int andysu_swapping; /* a swap was requested; the engine is shutting down for it */
extern int andysu_leave;    /* Sys_Main's loop ends */
extern int andysu_in_trial; /* running as the new engine of a swap that is not healthy yet */

/* Once per frame, from Sys_Frame: poll the loader, count frames, report health. */
void Andysu_FrameHook(void);
/* End of Host_Init: restore what the previous engine saved (map / connect), test hooks. */
void Andysu_RestoreState(void);
/* Sys_Error, after the window is gone: during a trial give up and return to the loader. */
void Andysu_AbortStart(const char *why);
/* zone.c */
void Mem_FreeAllPools(void);

#endif
