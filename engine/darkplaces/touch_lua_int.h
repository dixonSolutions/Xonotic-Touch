/*
Godnotic: shared internals of the Lua sandbox (touch_lua.c) and its two
realms, client mods (touch_lua_cl.c) and server mods (touch_lua_sv.c; the
code calls them plugins).
*/
#ifndef TOUCH_LUA_INT_H
#define TOUCH_LUA_INT_H

#include "touch_lua54/lua.h"
#include "touch_lua54/lauxlib.h"

/*
Permission tiers (API 3, docs/PLUGIN_STANDARD.md "Tiers"). A player understands
four switches, not a wall of scopes:

  standard     everything a normal mod does; on as soon as the mod is turned on
  gamerules    change the game: hook outputs, players, entities, the match (server)
  internet     HTTPS to the hosts the manifest names
  engine_full  any console command, any cvar that is not private

The bits keep the positions the API 1/2 grant files used ("v1 <hex mask>" in
luastate/<realm>/grants/), so files written before tiers mean the same: bit 12
was hooks_write and is Game rules; bits 0-11 were the standard scopes, which are
now always granted. The old scope names stay valid in mod.txt and in
lua_mod_permission and map onto these (tlua_scope_aliases in touch_lua.c).
*/
#define TLUA_TIER_STANDARD  (1u << 0)
#define TLUA_TIER_GAMERULES (1u << 12)
#define TLUA_TIER_INTERNET  (1u << 13)
#define TLUA_TIER_ENGINE    (1u << 14)
#define TLUA_NUMPERMS 4                  // entries of tlua_permissions[]
#define TLUA_PERM_ALL (TLUA_TIER_STANDARD | TLUA_TIER_GAMERULES | TLUA_TIER_INTERNET | TLUA_TIER_ENGINE)
// the switches a player turns on (standard needs none)
#define TLUA_PERM_OPTIONAL (TLUA_TIER_GAMERULES | TLUA_TIER_INTERNET | TLUA_TIER_ENGINE)
// the old scope names, as the code checks them
#define TLUA_PERM_EVENTS    TLUA_TIER_STANDARD
#define TLUA_PERM_COMMANDS  TLUA_TIER_STANDARD
#define TLUA_PERM_CVAR_READ TLUA_TIER_STANDARD
#define TLUA_PERM_CVAR_WRITE TLUA_TIER_STANDARD
#define TLUA_PERM_EXEC      TLUA_TIER_STANDARD
#define TLUA_PERM_STORAGE   TLUA_TIER_STANDARD
#define TLUA_PERM_DISPLAY   TLUA_TIER_STANDARD
#define TLUA_PERM_INPUT     TLUA_TIER_STANDARD
#define TLUA_PERM_PLAYERS   TLUA_TIER_STANDARD
#define TLUA_PERM_MESSAGES  TLUA_TIER_STANDARD
#define TLUA_PERM_STATE     TLUA_TIER_STANDARD
#define TLUA_PERM_HOOKS     TLUA_TIER_STANDARD
#define TLUA_PERM_HOOKS_WRITE TLUA_TIER_GAMERULES
#define TLUA_PERM_API1      TLUA_TIER_STANDARD
#define TLUA_PERM_LEGACY_DEFAULT TLUA_TIER_STANDARD

#define TLUA_MAX_MODS 32
#define TLUA_NAME_LEN 32
#define TLUA_MAX_FILE (1024 * 1024)      // init.lua, a required file, a load() chunk
#define TLUA_MAX_PATTERN_SUBJECT (1024 * 1024)
#define TLUA_SUFFIX_LEN 32               // the <name> of lua_<mod>_<name>
#define TLUA_MAX_MODCMDS 32              // console commands per mod
#define TLUA_MAX_MODCVARS 64             // own cvars per mod
#define TLUA_API_MAX 3                   // newest "api:" level this engine runs
#define TLUA_MAX_SETTINGS 32             // setting.<name> lines in mod.txt
#define TLUA_LOGLINES 40                 // recent output kept per mod (lua_mod_log)
#define TLUA_LOGLEN 200
#define TLUA_MAX_HOOKS 256               // mutator hooks the bridge can carry
#define TLUA_HOOKWORDS (TLUA_MAX_HOOKS / 32)
#define TLUA_MAX_TIMERS 256              // xt.after / xt.every / xt.spawn per mod

// what mod.txt says a package is for ("realm:")
#define TLUA_REALM_ANY 0
#define TLUA_REALM_CLIENT 1
#define TLUA_REALM_SERVER 2
#define TLUA_REALM_BOTH 3

typedef struct tlua_setting_s
{
	char name[TLUA_SUFFIX_LEN + 1];  // the cvar is <prefix><mod>_<name>
	char type[8];                   // bool, number (int, float), choice, text, color, key
	char def[64];
	char args[128];                 // number: min max [step]; choice: the options
	char label[64];
	char desc[160];                 // setting.<name>.desc
	char group[32];                 // setting.<name>.group: a section of its settings page
} tlua_setting_t;

typedef enum tlua_state_e
{
	TLUA_OFF,       // disabled by the user (or never enabled)
	TLUA_RUNNING,
	TLUA_ERROR      // enabled, but stopped by an error or a limit
} tlua_state_t;

struct tlua_realm_s;

typedef struct tlua_mod_s
{
	char name[TLUA_NAME_LEN + 1];
	char dir[1024];                 // absolute, ends in '/'
	char title[64];
	char version[32];
	char author[64];
	char description[256];
	char id[TLUA_NAME_LEN + 1];     // "id:" (must equal the folder name when given)
	char homepage[160];
	char updateurl[256];            // "update:" where lua_mod_update looks by default
	int api;                        // 0: legacy (no "api:"), else the level
	int realmdecl;                  // TLUA_REALM_*
	char reasons[TLUA_NUMPERMS][96]; // "reason.<scope>:" one line each
	char manifest_why[128];         // why the manifest was refused
	tlua_setting_t settings[TLUA_MAX_SETTINGS];
	int numsettings;
	qbool enabled;                  // the user's choice (enabled.txt)
	qbool crashed;                  // the crash marker named it: did not finish loading
	unsigned requested, granted;
	qbool manifest_error, scoped;
	qbool reserved;                 // the name clashes with the engine's (never started)
	tlua_state_t state;
	char error[512];
	lua_State *L;
	size_t mem, memlimit;
	long long insns;                // instructions left in the current call
	qbool killed;
	char killreason[160];
	int prints;                     // print() lines left in the current call
	unsigned long long eventmask;   // bit per realm event with a handler (up to 64)
	unsigned hookmask[TLUA_HOOKWORDS]; // bit per mutator hook with a handler (touch_lua_hooks.c)
	double hookspent;               // seconds spent in hook calls this host frame
	unsigned hookframe;             // host.framecount hookspent belongs to
	double timerdue;                // realtime of the earliest timer, 0 when none
	int numtimers;
	char log[TLUA_LOGLINES][TLUA_LOGLEN]; // recent print() lines, errors, manager notes
	int lognext, logcount;
	int execs;                      // xt.exec calls left in the current call
	int owncvars;                   // own cvars that exist (-1: not counted yet)
	struct tlua_realm_s *realm;
	// API 3 (touch_lua_api.c)
	char httphosts[256];            // "http:" hosts the manifest names, comma separated
	double cpu_window, cpu_last;    // seconds spent in calls: this second, the last full one
	unsigned calls_window, calls_last;
	unsigned throttled_window, throttled_last; // calls skipped by the per-frame budget
	double window_start;
	double framespent;              // seconds in calls this host frame (lua_frame_ms)
	unsigned spentframe;            // host.framecount framespent belongs to
	size_t mempeak;
	int engcmds;                    // xt.engine.cmd left in the current call
	int depth;                      // nesting of calls into this mod (re-entrancy)
	// declared x granted (docs/PLUGIN_STANDARD.md, section 13)
	qbool nomanifest;               // a folder with only init.lua: standard, and nothing can be granted
	qbool declares;                 // mod.txt has a permissions: line
	unsigned seen;                  // the tiers the player was shown (luastate/<realm>/seen/<id>.txt)
	char icon[64];                  // icon: in mod.txt (a .png/.tga of the package), "" when none
	// what it actually used, since it started (lua_mod_info, _lua_detail, lua_mod_top)
	unsigned tiercalls[TLUA_NUMPERMS];
	char hosts[192];                // hosts xt.http reached, comma separated
	int engcmds_used;
} tlua_mod_t;

typedef struct tlua_realm_s
{
	const char *kind;               // "mod" / "plugin" (machine-readable; users see TLUA_NOUN)
	const char *subdir;             // "mods" / "plugins"
	const char *cmdprefix;          // commands and cvars of a mod: <prefix><mod>_<name>, <name> without '_'
	unsigned cmdflags;              // CF_CLIENT / CF_SERVER
	const char *const *events;      // NULL terminated
	tlua_mod_t mods[TLUA_MAX_MODS];
	int nummods;
	qbool loaded;
	struct cvar_s *enable;          // master switch
	struct cvar_s *status;          // read-only summary for the menu
	void (*openapi)(lua_State *L);  // adds realm functions to the xt table on the stack top
	qbool (*cvar_writable)(const char *name);
	qbool (*command_allowed)(const char *word, const char *line); // word: the command, lower case
} tlua_realm_t;

extern tlua_realm_t tlua_client, tlua_server;

// What users read for a realm's packages: "mod" (client mods) or "server mod".
// kind, the plugins/ folder, _lua_select and the "realm" detail field keep
// saying "plugin": the menu and existing installs read those.
#define TLUA_NOUN(r) ((r) == &tlua_server ? "server mod" : "mod")

// The mod whose code is running now (NULL between calls).
extern tlua_mod_t *tlua_current;

// True while a call into a mod is under way, so an engine path that re-enters
// the Lua code from inside it (a loading screen redraw, a key read during a
// load) must leave it alone. A call left behind by Host_AbortCurrentFrame
// (a longjmp through it, in an earlier host frame) is cleaned up here and the
// answer is false.
qbool TLua_Busy(void);

// True when the command or cvar change being handled may have come from a
// server: while connected to one this engine does not host (or playing a
// demo), and until text such a server put in the command buffer has run.
qbool TLua_FromServer(void);

// touch_lua_vm.c asks this from the pattern matcher, and charges each element
// the table library reads with TouchLua_WorkStep.
int TouchLua_PatternAbort(void);
int TouchLua_WorkStep(lua_State *L);

typedef struct tlua_permission_s { const char *name; unsigned bit; int api; const char *help; } tlua_permission_t;
extern const tlua_permission_t tlua_permissions[]; // NULL terminated

// Realm management.
void TLua_Rescan(tlua_realm_t *r);
void TLua_LoadAll(tlua_realm_t *r);
void TLua_UnloadAll(tlua_realm_t *r, const char *why);
tlua_mod_t *TLua_Find(tlua_realm_t *r, const char *name);
qbool TLua_AnyHandler(tlua_realm_t *r, const char *event);

// Fires an event on every running mod of the realm. push() pushes the
// arguments and returns how many; result() sees each handler's single return
// value at the stack top (may be NULL). Both run inside a protected call.
typedef int (*tlua_push_f)(lua_State *L, void *ud);
typedef void (*tlua_result_f)(lua_State *L, tlua_mod_t *m, void *ud);
void TLua_Fire(tlua_realm_t *r, const char *event, tlua_push_f push, tlua_result_f result, void *ud);

// Helpers for realm API functions.
tlua_mod_t *TLua_Self(lua_State *L);
void TLua_Require(lua_State *L, unsigned permission);
void TLua_SetField(lua_State *L, const char *k, lua_CFunction f);
const char *TLua_CleanText(const char *in, char *out, size_t outsize, qbool allownewline);

// Runs f(L) with ud under the mod's limits, with a time limit of seconds
// (0: lua_time_ms). Returns true when it finished; on an error or a limit the
// mod is stopped and marked.
qbool TLua_Run(tlua_mod_t *m, lua_CFunction f, void *ud, double seconds);
qbool TLua_Has(tlua_mod_t *m, unsigned permission);
void TLua_Stop(tlua_mod_t *m);       // runs "unload", closes the state, state off
void TLua_Start(tlua_mod_t *m);
void TLua_Fail(tlua_mod_t *m, const char *msg);
void TLua_UpdateStatus(tlua_realm_t *r);
void TLua_WriteEnabled(tlua_realm_t *r);
void TLua_RealmDir(tlua_realm_t *r, char *out, size_t outsize);  // <userdir>/mods/
void TLua_StateDir(tlua_realm_t *r, char *out, size_t outsize);  // <userdir>/luastate/mods/
qbool TLua_ValidModName(const char *name);
qbool TLua_ReservedName(tlua_realm_t *r, const char *name);
// a line in the mod's recent output (lua_mod_log, the Mods dialog's log pane)
void TLua_Log(tlua_mod_t *m, const char *fmt, ...) DP_FUNC_PRINTF(2);
// the manager's last message, for the Mods dialog (_lua_last)
void TLua_Say(const char *fmt, ...) DP_FUNC_PRINTF(1);
// refuses (and says why) a manager change that may come from a server
qbool TLua_RemoteRefused(struct cmd_state_s *cmd);
// mod.txt as text: parses it into m (manifest fields, permissions, settings)
void TLua_ParseManifest(tlua_mod_t *m, char *text);
// file helpers, confined below a trusted base folder (see touch_lua.c)
#define TLUA_RD 0
#define TLUA_WR 1
#define TLUA_DIR 2
int TLua_OpenConfined(const char *base, const char *rel, int mode, qbool mkdirs);
long TLua_ReadState(const char *path, char *buf, size_t bufsize);
qbool TLua_WriteState(const char *dir, const char *name, const char *text, qbool durable);

// Packages (touch_lua_pkg.c): install, update, uninstall, downloads.
void TLua_Pkg_Setup(void);
void TLua_Pkg_Frame(void);
void TLua_Pkg_Forget(tlua_realm_t *r, const char *name); // grants, source record

// Mutator hooks (touch_lua_hooks.c), server realm.
void TLua_Hooks_OpenAPI(lua_State *L);
void TLua_Hooks_Changed(void);       // a subscription or a grant changed
int TLua_Hooks_Count(void);
const char *TLua_Hooks_Name(int i);
void TLua_Hooks_Setup(void);

// Timers and coroutines (xt.after, xt.every, xt.spawn, xt.sleep)
void TLua_RunTimers(tlua_realm_t *r);

// API 3 (touch_lua_api.c and the files it names): registration of the
// generated table, the Lua prelude, the shared modules, accounting
void TLua_API_Open(lua_State *L, tlua_mod_t *m);   // xt table on the stack top
void TLua_API_Setup(void);
void TLua_API_Frame(void);                         // once per host frame (client or server)
void TLua_API_ModStopped(tlua_mod_t *m);           // a mod's state is about to close
qbool TLua_API_Throttled(tlua_mod_t *m);           // over lua_frame_ms this frame: skip the call
void TLua_API_Account(tlua_mod_t *m, double seconds, qbool skipped);
const char *TLua_ReadModFile(lua_State *L, tlua_mod_t *m, const char *rel, size_t *outlen); // NULL when missing
qbool TLua_CvarSecret(const char *name);
qbool TLua_OwnCvar(tlua_mod_t *m, const char *name);
qbool TLua_CleanValue(const char *s);
// a C caller's call into a mod that is not an event: fn(L, ud) under the limits
qbool TLua_CallRef(tlua_mod_t *m, int ref, int (*push)(lua_State *L, void *ud), void *ud, void (*result)(lua_State *L, void *ud));
// registry references to Lua functions a mod handed to the engine (callbacks)
int TLua_Ref(lua_State *L, int idx);
void TLua_Unref(tlua_mod_t *m, int ref);
// the offline-only rule (touch_weapons.h): true when open; why says why not
qbool TLua_Offline(const char **why);
// a mod's icon decoded to BGRA (image_width/image_height set), or NULL:
// ref is "mod:<id>/<file>" and file must be the icon its mod.txt names
unsigned char *TLua_ModImageBGRA(const char *ref);
// usage notes from the API (touch_lua_api.c)
void TLua_NoteHost(tlua_mod_t *m, const char *host);

// copies the first n bytes of src (or less to fit) and ends the string: a prefix
// copy, cut on purpose, which dp_strlcpy would report as a truncation warning
static inline void TLua_CopyN(char *dst, size_t dstsize, const char *src, size_t n)
{
	if (!dstsize)
		return;
	if (n >= dstsize)
		n = dstsize - 1;
	memcpy(dst, src, n);
	dst[n] = 0;
}

// the client: whose CSQC runs (0 none, 1 foreign, 2 this game's)
int TLua_CL_CsqcKind(void);
// the realm modules API 3 adds
int TLua_Hooks_Subscribe(lua_State *L);
int TLua_Hooks_Names(lua_State *L);
int TLua_Hooks_CountRealm(tlua_realm_t *r);
const char *TLua_Hooks_NameRealm(tlua_realm_t *r, int i);
void TLua_CL_CsqcHello(void);
void TLua_Chan_Setup(void);
qbool TLua_Chan_SendPage(int player, const char *pageid, const char *json);
qbool TLua_Chan_ClosePage(int player, const char *pageid);
void TLua_Chan_Frame(void);
void TLua_Chan_ModStopped(tlua_mod_t *m);
void TLua_UI_Setup(void);
void TLua_UI_Frame(void);
void TLua_UI_ModStopped(tlua_mod_t *m);
void TLua_UI_SyncManifest(tlua_realm_t *r);
void TLua_SVAPI_Setup(void);
void TLua_SVAPI_Frame(void);
void TLua_SVAPI_ModStopped(tlua_mod_t *m);
qbool TLua_SVAPI_ChatLine(int player, const char *text);
qbool TLua_SVAPI_ClientCommand(int player, const char *line);
void TLua_CLAPI_Setup(void);
void TLua_CLAPI_ModStopped(tlua_mod_t *m);

// re-entrancy: true while any mod's code runs (hooks and events caused by a
// mod's own API call are not delivered: docs/PLUGIN_STANDARD.md)
#define TLua_InCall() (tlua_current != NULL)

// Cvars and commands, on the first hook call.
void TLua_InitOnce(void);
// Start of each client frame (the server-text bookkeeping of TLua_FromServer).
void TLua_FrameStart(void);

// Realm setup (touch_lua_cl.c / touch_lua_sv.c).
void TLua_CL_Setup(void);
void TLua_SV_Setup(void);
// The client frame stops the server's plugins once the local server is gone.
void TLua_SV_CheckStopped(void);

// Is the client drawing the 2D pass for mods now (xt.draw only works then).
extern qbool tlua_cl_drawing;

#endif
