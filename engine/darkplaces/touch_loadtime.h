#ifndef TOUCH_LOADTIME_H
#define TOUCH_LOADTIME_H

struct prvm_prog_s;

// Godnotic: where start-up and map loading spend their time.
//
// Phases are timed always (a clock read per call, nothing printed); with
// touch_loadtime 1 a table is printed when the main menu first shows and when
// a load reaches its first game frame. Times are inclusive: a model load
// contains the image decodes and uploads it does. Nested calls of one phase
// count once (the outermost).

typedef enum touch_lt_phase_e
{
	TLT_FS_INIT,       // FS_Init: game dirs, pk3 mounting and indexing
	TLT_CONFIG,        // startup configs (quake.rc, default.cfg, ...)
	TLT_VIDEO,         // window and GL context
	TLT_SOUND_INIT,    // sound device
	TLT_MENU_INIT,     // MR_Init: menu progs and m_init
	TLT_FONT,          // LoadFont (font files, size setup)
	TLT_PROGS,         // PRVM_Prog_Load (all three VMs)
	TLT_SPAWN,         // SV_SpawnServer
	TLT_CSQC,          // CL_VM_Init (client progs and CSQC_Init)
	TLT_DEFLATE,       // csprogs.dat deflate for downloads
	TLT_MODEL,         // Mod_LoadModel
	TLT_LIGHTMAP,      // q3bsp lightmaps
	TLT_Q3SHADER,      // scripts/*.shader parsing
	TLT_SOUND,         // S_LoadSound (decode)
	TLT_IMAGE,         // loadimagepixelsbgra (file read + decode)
	TLT_DDS,           // R_LoadTextureDDSFile (dds/ktx read + upload)
	TLT_TEXUPLOAD,     // R_UploadFullTexture
	TLT_SHADER,        // GLSL compile and link
	TLT_FILEREAD,      // FS_LoadFile (open, read, inflate)
	TLT_CVARLINK,      // Cvar_Link (new cvar into the sorted list)
	TLT_COUNT
}
touch_lt_phase_t;

typedef enum touch_lt_counter_e
{
	TLC_FS_LOOKUP,     // FS_FindFile calls
	TLC_FS_MISS,       // ... that found nothing
	TLC_FS_DIRSTAT,    // stat() of a loose directory candidate
	TLC_FILES_LOADED,  // FS_LoadFile successes
	TLC_COUNT
}
touch_lt_counter_t;

void TouchLoadTime_Begin(int phase);
void TouchLoadTime_End(int phase);
void TouchLoadTime_Count(int counter);
void TouchLoadTime_AddBytes(long long bytes);

// Times the rest of the enclosing block (GCC/Clang cleanup attribute; other
// compilers just do not time it).
#if defined(__GNUC__) || defined(__clang__)
typedef struct touch_lt_scope_s { int phase; } touch_lt_scope_t;
static inline void TouchLoadTime_ScopeEnd(touch_lt_scope_t *s) { TouchLoadTime_End(s->phase); }
#define TOUCH_LT_CAT2(a, b) a##b
#define TOUCH_LT_CAT(a, b) TOUCH_LT_CAT2(a, b)
#define TOUCH_LT_SCOPE(p) touch_lt_scope_t TOUCH_LT_CAT(touch_lt_scope_, __LINE__) __attribute__((cleanup(TouchLoadTime_ScopeEnd), unused)) = { ((void)TouchLoadTime_Begin(p), (p)) }
#else
#define TOUCH_LT_SCOPE(p) ((void)0)
#endif

void TouchLoadTime_Init(void);

// ft2.c (touch_loadfont.h): render one font size left for later by
// r_font_lazysizes; false when none is left.
qbool TouchFont_LoadPendingOne(void);

// snd_main.c (touch_loadsnd.h) sets this: decode precached sounds not
// played yet within snd_lazyload_budget; called once per host frame in game.
extern void (*touch_snd_loadsome)(void);
extern void (*touch_snd_poll)(void);
// sfx_t flag: precached, not decoded yet (snd_lazyload)
#define TOUCH_SFXFLAG_DEFERRED (1u << 28)

// model_shared.c (touch_loadmodel.h), mod_lazyload: Mod_ForName only
// registers a model inside a precache scope; TouchModel_Ensure loads a model
// registered that way (the model lookups call it); touch_model_loadsome
// loads small ones in the game within mod_lazyload_budget.
void TouchModel_PrecacheScope(int enter);
struct model_s *TouchModel_Ensure(struct model_s *mod);
qbool TouchModel_IsDeferred(struct model_s *mod);
// VM_CL_precache_model after Mod_ForName: a registered model takes a CSQC
// precache slot as a loaded one would; true when it did.
qbool TouchModel_CSQCPrecache(struct prvm_prog_s *prog, struct model_s *m);
extern void (*touch_model_loadsome)(void);

// sbar.c's render module start caches ~112 Quake status bar pictures that
// Xonotic does not have and never draws (its HUD is CSQC). With
// touch_load_lazysbar 1 the start is remembered and run on the engine HUD's
// first draw (Sbar_Draw with drawenginesbar, Sbar_DeathmatchOverlay).
qbool TouchLoad_SbarDefer(void (*start)(void));
void TouchLoad_SbarEnsure(void);

// SV_Prepare_CSQC deflated csprogs.dat (4 MB) at every map start for a
// client that might download it with deflate. touch_load_lazydeflate 1:
// it is deflated when a client first asks for it.
qbool TouchLoad_DeferCSQCDeflate(void);
void TouchLoad_EnsureCSQCDeflated(void);

// The menu precaches every picture of its skin at start (107 on the phone
// packs, each one or two JPEG decodes). touch_load_lazypics 1: a menu
// precache_pic of a file that exists only remembers the name and flags; the
// picture loads, persistent as precached, when first drawn.
qbool TouchLoad_LazyPrecachePic(struct prvm_prog_s *prog, const char *name, unsigned int flags);
unsigned int TouchLoad_PicFlags(const char *path, unsigned int flags);
// After every drawn frame: notices the menu's first frame and a load's first
// game frame.
void TouchLoadTime_Frame(void);
// Once per host frame, with the time the main loop is about to sleep: returns
// the time it may sleep (0 with a local server, 1 ms with a remote one, while
// the handshake runs and touch_load_restless is 1).
double TouchLoadTime_SleepTime(double sleeptime);
// True while a connection or load is in progress and the client frame should
// run every host frame (no frame limiter).
qbool TouchLoadTime_Restless(void);
// CL_Frame's frame limit: 0 (none) while restless.
float TouchLoadTime_MaxFPS(float maxfps);

#endif
