/*
Godnotic UI bridge: internals shared by touch_uib*.c. Not part of the ABI.
*/
#ifndef TOUCH_UIB_INT_H
#define TOUCH_UIB_INT_H

#include "touch_uib.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"
#endif

// ---------------------------------------------------------------- limits (docs 3)
#define UIB_MAX_MESSAGE   (4u << 20)
#define UIB_MAX_STRING    4096
#define UIB_MAX_BODY      8192
#define UIB_MAX_ID        128
#define UIB_MAX_NODES     8192
#define UIB_MAX_CHILDREN  1024
#define UIB_MAX_DEPTH     32
#define UIB_MAX_WINDOW    200
#define UIB_MAX_CONNS     8
// mods (docs 8)
#define UIB_MOD_NODES     256
#define UIB_MOD_DEPTH     8
#define UIB_MOD_STRING    512
#define UIB_MOD_IMAGES    16
#define UIB_MOD_BYTES     (32u << 10)

// ---------------------------------------------------------------- growable buffer
typedef struct uibbuf_s
{
	char *p;
	size_t n, cap;
} uibbuf_t;

void UIBB_Reserve(uibbuf_t *b, size_t more);
void UIBB_Put(uibbuf_t *b, const char *s, size_t len);
void UIBB_Puts(uibbuf_t *b, const char *s);
void UIBB_Putc(uibbuf_t *b, char c);
void UIBB_Printf(uibbuf_t *b, const char *fmt, ...);
// A JSON string literal (quotes included). maxbytes truncates at a UTF-8 boundary;
// strip removes Quake colour codes and maps the Quake private glyphs.
void UIBB_Str(uibbuf_t *b, const char *s, size_t len, size_t maxbytes, int strip);
void UIBB_Num(uibbuf_t *b, double v);
char *UIBB_Detach(uibbuf_t *b, size_t *len);  // NUL-terminated, malloc'd
void UIBB_Free(uibbuf_t *b);

// Display text: colour codes removed, Quake glyphs mapped, into out (NUL-terminated).
size_t UIB_CleanText(const char *in, size_t inlen, char *out, size_t outsize);

// ---------------------------------------------------------------- JSON values
enum { UIBJ_NULL, UIBJ_FALSE, UIBJ_TRUE, UIBJ_NUM, UIBJ_STR, UIBJ_ARR, UIBJ_OBJ };
typedef struct uibj_s
{
	int type;
	double num;
	const char *str;      // UIBJ_STR (NUL-terminated, unescaped)
	size_t len;           // string length; array/object member count
	const char *key;      // member key when inside an object
	struct uibj_s *child; // first member
	struct uibj_s *next;  // next sibling
	const char *raw;      // start of this value in the source text
	size_t rawlen;
} uibj_t;

typedef struct uibjdoc_s uibjdoc_t;
// Parse (depth <= 64). NULL on error, err filled. The values live until UIBJ_Free.
uibjdoc_t *UIBJ_Parse(const char *s, size_t len, char *err, size_t errlen);
uibj_t *UIBJ_Root(uibjdoc_t *d);
void UIBJ_Free(uibjdoc_t *d);
uibj_t *UIBJ_Get(const uibj_t *obj, const char *key);
uibj_t *UIBJ_At(const uibj_t *arr, size_t i);
const char *UIBJ_Str(const uibj_t *obj, const char *key, const char *def);
double UIBJ_Num(const uibj_t *obj, const char *key, double def);
int UIBJ_Bool(const uibj_t *obj, const char *key, int def);
// Value at a dotted path inside one JSON fragment ("primary.label", "actions.1.id").
// Writes the value as display text (string unquoted, number formatted, bool 0/1,
// object/array raw JSON) into out. Returns 1 if found.
int UIBJ_PathText(const char *json, size_t len, const char *path, char *out, size_t outsize);
// Re-serialise a parsed value compactly.
void UIBJ_Write(uibbuf_t *b, const uibj_t *v);

// ---------------------------------------------------------------- store
typedef struct uibprop_s
{
	char *key;
	char *json;
	size_t len;
} uibprop_t;

typedef struct uibnode_s
{
	char *id;
	char type[16];
	char *origin;                 // NULL: core
	uibprop_t *props;
	int nprops, maxprops;
	struct uibnode_s **kids;
	int nkids, maxkids;
	struct uibnode_s *parent;
	int64_t gen;                  // rev at which it last changed
	int mark;                     // build serial that last emitted it
	int isroot;                   // a screen or toast directly under app
	// pending (during a build)
	uibprop_t *pprops;
	int npprops, maxpprops;
	struct uibnode_s **pkids;
	int npkids, maxpkids;
	char ptype[16];
	int building;
	// serialised cache
	char *json;
	size_t jsonlen;
	// binding
	char bindkind;                // 'c' cvar, 'v' vm, 'm' mod, 0 none
	void *bindcvar;               // cvar_t *
	char *bindval;                // cvar string the value prop was made from
	struct uibnode_s *hnext;      // hash chain
	struct uibnode_s *lnext, *lprev; // all nodes
} uibnode_t;

uibnode_t *UIBS_Find(const char *id);
int64_t UIBS_Rev(void);
const char *UIBS_PropJSON(const uibnode_t *n, const char *key, size_t *len);
// Set one prop outside a build (engine-side updates: counts, values); marks the node changed.
void UIBS_SetProp(uibnode_t *n, const char *key, const char *json, size_t len);
int UIBS_NodeCount(void);

// Build API (menu VM builtins and mod pages)
int UIBS_BuildBegin(const char *rootid, const char *type, const char *origin);
int UIBS_Open(const char *key, const char *type);   // child of the open node
int UIBS_Set(const char *key, const char *json, size_t len);  // on the open node
int UIBS_Push(const char *key, const char *json, size_t len); // append to an array prop
void UIBS_Close(void);
void UIBS_BuildEnd(void);
const char *UIBS_OpenId(void);
void UIBS_Remove(const char *id);    // a whole subtree
// Navigation (app props), set by the menu VM
void UIBS_SetNav(const char *stack, const char *modal, int canback, const char *context);
void UIBS_SetBusy(const char *json, size_t len);  // NULL/0: not busy
void UIBS_SetReserved(const char *json, size_t len);  // app.reserved; NULL/0: nothing drawn over the UI
void UIBS_Toast(const char *text, const char *kind, const char *icon, float timeout, const char *actionjson);
void UIBS_Reset(void);

// Snapshot / diff (malloc'd)
char *UIBS_Snapshot(size_t *len, int msgid);
char *UIBS_Diff(int64_t since, size_t *len, int msgid);
// Ends the frame's changes: reconcile app, bump rev if anything changed. 1 if it did.
int UIBS_Commit(void);
void UIBS_WatchTick(void);  // compare bound cvars

// ---------------------------------------------------------------- connections / protocol
void UIB_Broadcast(const char *json, size_t len);   // to every connection
void UIB_SendTo(int conn, const char *json, size_t len);
int UIB_NextMsgId(int conn);
void UIB_SendError(int conn, int64_t re, const char *code, const char *msg);
int UIB_ConnCount(void);
// deliver a parsed message from connection conn (-1: console/loopback)
void UIB_Handle(int conn, const char *json, size_t len);
extern int uib_loopback_print;

// ---------------------------------------------------------------- menu VM side (touch_uib_mvm.c)
// Calls the menu VM's UIB_Event(ev, node, arg, json) if it exists. Returns its result.
float UIBM_Event(const char *ev, const char *node, const char *arg, const char *json);
int UIBM_Ready(void);   // menu VM loaded and describing (UIB_Event exists)
extern int uibm_tick_wanted;

// ---------------------------------------------------------------- sources (touch_uib_src.c)
typedef struct uibrow_s uibrow_t;
// Answer a window request; writes a complete `rows` message (or error) into b.
void UIBSrc_Window(uibbuf_t *b, int msgid, int64_t re, const char *source, int offset, int limit,
                   const char *sort, int desc, const char *filter, int hasfilter);
void UIBSrc_Frame(void);     // poll engine sources (servers) and notify changes
void UIBSrc_Init(void);
void UIBSrc_Reset(void);
// static sources set by the menu VM
void UIBSrc_StaticBegin(const char *source);
void UIBSrc_StaticRow(const char *key, const char *cellsjson, const char *image);
void UIBSrc_StaticEnd(void);
void UIBSrc_Param(const char *source, const char *key, const char *value);
// The QuakeC renderer's window (engine-thread cache)
int UIBSrc_LocalWindow(const char *source, int offset, int limit, const char *sort, int desc, const char *filter);
int UIBSrc_LocalTotal(void);
const char *UIBSrc_LocalKey(int i);
const char *UIBSrc_LocalImage(int i);
const char *UIBSrc_LocalCell(int i, const char *key);
int64_t UIBSrc_Rev(const char *source);
// The bound list nodes' count props follow their source.
void UIBSrc_UpdateCounts(void);

// ---------------------------------------------------------------- images (touch_uib_img.c)
void UIBImg_Request(int conn, int64_t re, const char *ref, int w, int h, int asbytes);
void UIBImg_Frame(void);
void UIBImg_Init(void);
// Resolve a ref to a VFS path the engine can load ("" if none). For the QuakeC renderer.
int UIBImg_Resolve(const char *ref, char *path, size_t pathsize);

// ---------------------------------------------------------------- stream transport (touch_uib_stream.c)
void UIBStream_Init(void);
void UIBStream_Frame(void);
void UIBStream_Shutdown(void);
int UIBStream_Active(void);
int UIBStream_BackendMode(void);

// mods
int UIBMod_Materialize(const char *screenid);   // build a registered mod page as a screen
void UIBMod_Event(const char *mod_id, const char *json, size_t len);

double UIB_Time(void);
void *UIB_CvarFind(const char *name);
const char *UIB_CvarString(void *cvar);
void UIB_LocalPost(const char *json, size_t len);
const char *UIB_LastCaps(void);
void UIB_Touched(void);
size_t UIBMod_List(char *out, size_t size);
uibnode_t *UIBS_FirstNode(void);
int UIBImg_Thumb(const char *ref, int w, int h, char *out, size_t outsize);

#endif
