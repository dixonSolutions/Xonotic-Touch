/*
 * AndySU / Xonotic Touch: contract between the resident loader (libandysu_xonotic_loader.so) and
 * a swappable engine library (libmain.so, later libmain.<hash8>.so). See ../LIVE_SWAP.md 2.2.
 *
 * Stage 1 (this version): the loader dlopens the engine, calls andysu_engine_v1()->run() and
 * nothing else. The swap-related callbacks exist in the struct so the ABI does not change when
 * swapping arrives; stage 1 loaders return harmless defaults and the engine never calls them.
 */
#ifndef ANDYSU_SWAP_H
#define ANDYSU_SWAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANDYSU_ENGINE_ABI 1

enum { ANDYSU_RUN_QUIT = 0, ANDYSU_RUN_SWAP = 1, ANDYSU_RUN_FAILED = 2 };

typedef struct andysu_loader_api {
    uint32_t abi, size;
    /* Atomic read; the engine polls it once per frame. Stage 1: always 0. */
    int (*swap_requested)(void);
    /* Engine -> next engine state (the loader copies it). Stage 1: no-op. */
    void (*put_state)(const char *p, size_t n);
    /* State left by the previous engine, NULL on a cold start. Stage 1: NULL. */
    const char *(*get_state)(size_t *n);
    /* Commit the running generation (Java: Host.current().markHealthy()). Stage 1: logs only. */
    void (*mark_healthy)(void);
    /* Structured event, one JSON object without newline, to logcat tag AndySU-Event. */
    void (*emit)(const char *json);
    /* Trial only: abandon the start and return to the loader. Stage 1: NULL. */
    void (*abort_start)(const char *why);
} andysu_loader_api;

typedef struct andysu_engine {
    uint32_t abi, size;
    /* Contract with the running Java side (JNI surface); the loader compares it before a swap. */
    uint32_t java_abi;
    const char *build_id;
    /* Runs the engine on the calling (SDL) thread. Stage 1: Sys_Main(), which does not return
     * (the engine exits the process on quit). */
    int (*run)(const andysu_loader_api *loader, int argc, char **argv, int swapped_in);
    /* What XonoticActivity.nativeInputDevices used to deliver by JNI. */
    void (*input_devices)(int touch, int keyboard, int mouse);
} andysu_engine;

/* The one symbol an engine library exports. */
#define ANDYSU_ENGINE_SYMBOL "andysu_engine_v1"
typedef const andysu_engine *(*andysu_engine_fn)(void);

#ifdef __cplusplus
}
#endif
#endif
