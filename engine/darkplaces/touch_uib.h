/*
Godnotic UI bridge: the menu VM describes every screen once as a tree of semantic nodes;
this store keeps it, turns it into JSON snapshots and keyed diffs, and hands it to
renderers: the Rust core on Android (in process, through the queue functions below),
the GTK app on Linux (touch_uib_stream.c), and the QuakeC renderer (touch_uib_mvm.c).

Protocol, node types and threading rules: docs/UI_BRIDGE.md. This header is a plain C
ABI: opaque handles, UTF-8 buffers with explicit lengths, engine-owned buffers released
with TouchUIB_Free.
*/
#ifndef TOUCH_UIB_H
#define TOUCH_UIB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TOUCHUIB_PROTOCOL 1
#define TOUCHUIB_SCHEMA "1.0"

typedef struct touchuib_conn_s touchuib_conn_t;

/* ---- thread-safe: any thread, once the engine has started ---- */

/* 1 when the bridge is up (the engine finished Host_Init), else 0. */
int TouchUIB_Available(void);
/* A new renderer connection. name is for logs ("android", "linux"). NULL on failure
   (bridge not up, or 8 connections already open). */
touchuib_conn_t *TouchUIB_Open(const char *name, size_t name_len);
/* Queue one renderer message (one JSON object, no newline needed). The engine copies it
   and handles it at the start of its next frame. 0 ok, -1 closed, -2 queue full (256
   messages or 8 MiB: retry later), -3 larger than 4 MiB. */
int TouchUIB_Post(touchuib_conn_t *c, const char *json, size_t len);
/* Next engine message, or NULL. Owned by the caller: release with TouchUIB_Free (any
   thread). NUL-terminated; *len (if not NULL) excludes the NUL. */
char *TouchUIB_Take(touchuib_conn_t *c, size_t *len);
void TouchUIB_Free(char *buf);
/* An fd that is readable while messages wait (eventfd on Linux/Android; -1 elsewhere).
   Read 8 bytes to reset it, then Take until NULL. Owned by the connection. */
int TouchUIB_NotifyFd(touchuib_conn_t *c);
/* Or a callback, run on the engine thread right after messages were queued. It must not
   block; it may call TouchUIB_Take/TouchUIB_Free or signal another thread. NULL clears. */
void TouchUIB_SetNotify(touchuib_conn_t *c, void (*fn)(void *ctx), void *ctx);
/* Close: the handle is invalid afterwards; pending messages are dropped. */
void TouchUIB_Close(touchuib_conn_t *c);

/* ---- engine thread only ---- */

char *TouchUIB_SnapshotJSON(size_t *len);
char *TouchUIB_DiffJSON(int64_t since_rev, size_t *len);  /* NULL if since_rev is too old */
int64_t TouchUIB_Rev(void);

/* Mods (the Lua API). Page JSON: {"title": "...", "nodes": [{"id": "general", "type":
   "section", "props": {...}, "children": ["general/range"]}, ...]}, ids relative to the
   page, the first node the page's root content. 0 ok, < 0 refused (err says why). */
int TouchUIB_ModSetPage(const char *mod_id, const char *page_key, const char *json, size_t len,
                        char *err, size_t errlen);
void TouchUIB_ModRemove(const char *mod_id, const char *page_key /* NULL: every page */);
int TouchUIB_ModSetValue(const char *mod_id, const char *key, const char *json_value, size_t len);
typedef void (*touchuib_mod_event_fn)(const char *mod_id, const char *ev_json, size_t len, void *ctx);
void TouchUIB_ModSetEventHandler(touchuib_mod_event_fn fn, void *ctx);

/* ---- engine integration (engine thread) ---- */

void TouchUIB_Init(void);        /* Host_InitLocal: cvars, commands, -uibridge */
void TouchUIB_Frame(void);       /* Host_Frame, after Cbuf_Frame */
void TouchUIB_Shutdown(void);    /* Host_Shutdown, before CL_Shutdown */
/* 1 while a native renderer is attached (the menu VM then describes but does not draw). */
int TouchUIB_NativeAttached(void);

#ifdef __cplusplus
}
#endif

/* One loading presentation. True while a front end shows its own loading card
   (it sets _touch_loading_card 1 when the card goes up and 0 when it leaves) and
   a join or a map load is in fact under way: the engine then draws its loading
   picture and bar without the status line, the download lines and the notify
   lines, which the card carries. The console keeps the whole log. */
int TouchUIB_LoadingCardUp(void);

/* One copy over play. Con_MaskPrint asks this for each line that opens with a
   chat-box control character (lead: 1 a player speaking, 3 the game's own line,
   such as "connected", "is now playing" or a frag). Unless
   touch_chat_system_lines is 1, a game line leaves the chat feed and the notify
   lines and stays in the console: the loading card, the kill feed and the
   scoreboard already show it. Player chat is untouched. */
unsigned TouchUIB_ChatboxMask(char lead, unsigned mask);

#endif
