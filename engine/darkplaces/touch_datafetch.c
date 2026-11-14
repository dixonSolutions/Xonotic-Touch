/*
 * touch_datafetch.c -- the game's half of the progressive data download
 * (Godnotic; docs/DATA_FETCH.md).
 *
 * The app (Android service, Linux loader, web page) downloads the split data
 * set while the game runs. It tells the game how far it is and which packs
 * have landed; the game mounts them (fs_mountpack, touch_fs_mount.h), tells
 * the app which map the player is waiting for, and holds a "map" command or
 * a join back until that map is here.
 *
 * Host to game: the status file data/touch/data-status.txt (where QuakeC's
 * fopen and the updater's status file live), read once a second, or the same
 * fields as arguments of touch_data_status where the host has a command
 * channel. Game to host: data/touch/data-request.txt ("want <map> <n>"), or
 * Module.xt.dataWant(map) on the web.
 *
 * Nothing here blocks: a missing host means an empty state and every command
 * behaves as it always did.
 */
#include "quakedef.h"
#include "touch_datafetch.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define TOUCH_DATA_STATUS "data/touch/data-status.txt"
#define TOUCH_DATA_REQUEST "data/touch/data-request.txt"
#define TOUCH_DATA_STATUS_MAX 65536

static cvar_t touch_data_state = {CF_CLIENT | CF_READONLY, "touch_data_state", "", "progressive data download: \"\" (no downloader), fetching, paused, waiting, error, complete or legacy (Godnotic, docs/DATA_FETCH.md)"};
static cvar_t touch_data_set = {CF_CLIENT | CF_READONLY, "touch_data_set", "", "id of the data set being downloaded"};
static cvar_t touch_data_done_kb = {CF_CLIENT | CF_READONLY, "touch_data_done_kb", "0", "KiB of the data set installed"};
static cvar_t touch_data_total_kb = {CF_CLIENT | CF_READONLY, "touch_data_total_kb", "0", "KiB of the whole data set"};
static cvar_t touch_data_want = {CF_CLIENT | CF_READONLY, "touch_data_want", "", "map the downloader is fetching ahead of the rest"};
static cvar_t touch_data_want_done_kb = {CF_CLIENT | CF_READONLY, "touch_data_want_done_kb", "0", "KiB installed of what touch_data_want still needed"};
static cvar_t touch_data_want_total_kb = {CF_CLIENT | CF_READONLY, "touch_data_want_total_kb", "0", "KiB touch_data_want still needed when it was asked for"};
static cvar_t touch_data_message = {CF_CLIENT | CF_READONLY, "touch_data_message", "", "what the downloader says about a wait or an error"};
static cvar_t touch_data_pending = {CF_CLIENT | CF_READONLY, "touch_data_pending", "", "command that runs when the map it needs has been downloaded (touch_data_cancel drops it)"};
static cvar_t touch_data_pending_map = {CF_CLIENT | CF_READONLY, "touch_data_pending_map", "", "the map touch_data_pending waits for"};

static char *touchdata_last;        // the status file as last parsed
static double touchdata_nextpoll;
static int touchdata_requests;
static stringlist_t touchdata_mounted; // packs already handed to fs_mountpack
static xcommand_t touchdata_map_orig, touchdata_changelevel_orig;
static qbool touchdata_wrapped;

static qbool TouchData_Active(void)
{
	const char *s = touch_data_state.string;
	return !strcmp(s, "fetching") || !strcmp(s, "paused") || !strcmp(s, "waiting") || !strcmp(s, "error");
}

/// a map of the set whose pack is still to come: its mapinfo is in the core, its bsp is not here
static qbool TouchData_MapQueued(const char *map)
{
	char name[MAX_QPATH];
	if (!map[0] || strchr(map, '/') || strchr(map, '\\') || strstr(map, "..") || strlen(map) > 48)
		return false;
	if (!TouchData_Active())
		return false;
	dpsnprintf(name, sizeof(name), "maps/%s.bsp", map);
	if (FS_FileExists(name))
		return false;
	dpsnprintf(name, sizeof(name), "maps/%s.mapinfo", map);
	return FS_FileExists(name) != NULL;
}

static qbool TouchData_PackName(const char *name)
{
	size_t len = strlen(name);
	const char *p;
	if (len < 5 || len >= MAX_QPATH || strcasecmp(name + len - 4, ".pk3"))
		return false;
	for (p = name; *p; p++)
		if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '_' || *p == '-'))
			return false;
	return true;
}

static void TouchData_Mount(const char *name)
{
	unsigned int i;
	char line[MAX_QPATH + 32];
	if (!TouchData_PackName(name))
		return;
	for (i = 0; i < (unsigned int)touchdata_mounted.numstrings; i++)
		if (!strcmp(touchdata_mounted.strings[i], name))
			return;
	stringlistappend(&touchdata_mounted, name);
	dpsnprintf(line, sizeof(line), "\nfs_mountpack %s\n", name);
	Cbuf_AddText(cmd_local, line);
}

/// one "key value" of the status, from the file or from the command
static void TouchData_Field(const char *key, const char *value)
{
	if (!strcmp(key, "state"))
	{
		if (!strcmp(value, "fetching") || !strcmp(value, "paused") || !strcmp(value, "waiting") || !strcmp(value, "error") || !strcmp(value, "complete") || !strcmp(value, "legacy") || !value[0])
			Cvar_SetQuick(&touch_data_state, value);
	}
	else if (!strcmp(key, "set"))
		Cvar_SetQuick(&touch_data_set, value);
	else if (!strcmp(key, "done_kb"))
		Cvar_SetValueQuick(&touch_data_done_kb, max(0, atof(value)));
	else if (!strcmp(key, "total_kb"))
		Cvar_SetValueQuick(&touch_data_total_kb, max(0, atof(value)));
	else if (!strcmp(key, "want"))
		Cvar_SetQuick(&touch_data_want, value);
	else if (!strcmp(key, "want_done_kb"))
		Cvar_SetValueQuick(&touch_data_want_done_kb, max(0, atof(value)));
	else if (!strcmp(key, "want_total_kb"))
		Cvar_SetValueQuick(&touch_data_want_total_kb, max(0, atof(value)));
	else if (!strcmp(key, "message"))
		Cvar_SetQuick(&touch_data_message, value);
	else if (!strcmp(key, "landed"))
		TouchData_Mount(value);
	// anything else: a newer host, ignored
}

/// a status is a snapshot: what it does not say is not so
static void TouchData_Begin(void)
{
	Cvar_SetQuick(&touch_data_want, "");
	Cvar_SetValueQuick(&touch_data_want_done_kb, 0);
	Cvar_SetValueQuick(&touch_data_want_total_kb, 0);
	Cvar_SetQuick(&touch_data_message, "");
}

static void TouchData_ParseFile(char *text)
{
	char *line, *next, *value, *end;
	qbool known = false;

	for (line = text; line && *line; line = next)
	{
		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		end = line + strlen(line);
		while (end > line && (end[-1] == '\r' || end[-1] == ' '))
			*--end = 0;
		value = strchr(line, ' ');
		if (value)
			*value++ = 0;
		else
			value = end;
		if (!strcmp(line, "format"))
		{
			if (atoi(value) != 1)
				return; // a format this build does not read
			known = true;
			TouchData_Begin();
			continue;
		}
		if (known)
			TouchData_Field(line, value);
	}
}

static void TouchData_Poll(void)
{
	fs_offset_t size;
	char *text = (char *)FS_LoadFile(TOUCH_DATA_STATUS, tempmempool, true, &size);

	if (!text)
		return;
	if (size > 0 && size < TOUCH_DATA_STATUS_MAX && (!touchdata_last || strcmp(touchdata_last, text)))
	{
		if (touchdata_last)
			Mem_Free(touchdata_last);
		touchdata_last = (char *)Mem_Alloc(zonemempool, (size_t)size + 1);
		memcpy(touchdata_last, text, (size_t)size + 1);
		TouchData_ParseFile(text);
	}
	Mem_Free(text);
}

static void TouchData_Status_f(cmd_state_t *cmd)
{
	int i;
	if (Cmd_Argc(cmd) < 3)
	{
		Con_Printf("touch_data_state \"%s\" set \"%s\" %i of %i KiB, want \"%s\" %i of %i KiB, pending \"%s\"\n", touch_data_state.string, touch_data_set.string, touch_data_done_kb.integer, touch_data_total_kb.integer, touch_data_want.string, touch_data_want_done_kb.integer, touch_data_want_total_kb.integer, touch_data_pending.string);
		return;
	}
	for (i = 1; i + 1 < Cmd_Argc(cmd); i += 2)
		if (!strcmp(Cmd_Argv(cmd, i), "state"))
		{
			TouchData_Begin();
			break;
		}
	for (i = 1; i + 1 < Cmd_Argc(cmd); i += 2)
		TouchData_Field(Cmd_Argv(cmd, i), Cmd_Argv(cmd, i + 1));
}

#ifdef __EMSCRIPTEN__
EM_JS(void, touchdata_js_want, (const char *map), {
	if (Module.xt && Module.xt.dataWant)
		Module.xt.dataWant(UTF8ToString(map));
});
#endif

static void TouchData_Want(const char *map)
{
#ifdef __EMSCRIPTEN__
	touchdata_js_want(map);
#else
	qfile_t *f = FS_OpenRealFile(TOUCH_DATA_REQUEST ".tmp", "wb", true);
	char from[MAX_OSPATH], to[MAX_OSPATH];
	if (!f)
		return;
	FS_Printf(f, "want %s %i\n", map, ++touchdata_requests);
	FS_Close(f);
	// by rename, so the host never reads half a line
	dpsnprintf(from, sizeof(from), "%s" TOUCH_DATA_REQUEST ".tmp", fs_gamedir);
	dpsnprintf(to, sizeof(to), "%s" TOUCH_DATA_REQUEST, fs_gamedir);
	if (rename(from, to))
		Con_DPrintf("touch_data_want: could not write %s\n", to);
#endif
}

static void TouchData_Want_f(cmd_state_t *cmd)
{
	const char *map;
	if (Cmd_Argc(cmd) != 2)
	{
		Con_Printf("usage: touch_data_want <map>  (ask the data downloader to fetch this map's packs first)\n");
		return;
	}
	map = Cmd_Argv(cmd, 1);
	if (TouchData_MapQueued(map))
		TouchData_Want(map);
}

static void TouchData_SetPending(const char *map, const char *line)
{
	Cvar_SetQuick(&touch_data_pending, line);
	Cvar_SetQuick(&touch_data_pending_map, map);
	TouchData_Want(map);
}

static void TouchData_Cancel_f(cmd_state_t *cmd)
{
	Cvar_SetQuick(&touch_data_pending, "");
	Cvar_SetQuick(&touch_data_pending_map, "");
}

/// true: the map is still downloading; the command is kept and runs when it is here
static qbool TouchData_Deferred(cmd_state_t *cmd)
{
	char line[MAX_INPUTLINE];
	if (Cmd_Argc(cmd) != 2 || !TouchData_MapQueued(Cmd_Argv(cmd, 1)))
		return false;
	dpsnprintf(line, sizeof(line), "%s \"%s\"", Cmd_Argv(cmd, 0), Cmd_Argv(cmd, 1));
	TouchData_SetPending(Cmd_Argv(cmd, 1), line);
	Con_Printf("Godnotic: map %s is still downloading; it starts when it is here\n", Cmd_Argv(cmd, 1));
	return true;
}

static void TouchData_Map_f(cmd_state_t *cmd)
{
	if (!TouchData_Deferred(cmd))
		touchdata_map_orig(cmd);
}

static void TouchData_Changelevel_f(cmd_state_t *cmd)
{
	if (!TouchData_Deferred(cmd))
		touchdata_changelevel_orig(cmd);
}

/// Wrap "map" and "changelevel" in the command list (the way sys_wasm.c does
/// for the web page's own hook), so no upstream command changes.
static void TouchData_Wrap(void)
{
#ifndef __EMSCRIPTEN__ // the page's needMap hook does this on the web
	cmd_function_t *f;
	for (f = cmd_local->engine_functions; f; f = f->next)
	{
		if (!strcmp(f->name, "map") && f->function != TouchData_Map_f)
		{
			touchdata_map_orig = f->function;
			f->function = TouchData_Map_f;
		}
		else if (!strcmp(f->name, "changelevel") && f->function != TouchData_Changelevel_f)
		{
			touchdata_changelevel_orig = f->function;
			f->function = TouchData_Changelevel_f;
		}
	}
#endif
	touchdata_wrapped = true;
}

qbool TouchData_JoinNeedsMap(const char *worldmodel, char *text, size_t textsize)
{
	char map[MAX_QPATH], line[256];
	const char *base;
	size_t len;

	if (!worldmodel || strncmp(worldmodel, "maps/", 5) || !cls.netcon)
		return false;
	base = worldmodel + 5;
	len = strlen(base);
	if (len < 5 || len >= sizeof(map) || strcmp(base + len - 4, ".bsp"))
		return false;
	memcpy(map, base, len - 4);
	map[len - 4] = 0;
	if (!TouchData_MapQueued(map))
		return false;
	dpsnprintf(line, sizeof(line), "connect \"%s\"", cls.netcon->address);
	TouchData_SetPending(map, line);
	dpsnprintf(text, textsize, "Godnotic: map %s is still downloading. Joining when it is here.\n", map);
	return true;
}

qbool TouchData_JoinPending(void)
{
	return !strncmp(touch_data_pending.string, "connect ", 8);
}

int TouchData_MapState(const char *map, float *progress)
{
	char name[MAX_QPATH];
	if (progress)
		*progress = -1;
	dpsnprintf(name, sizeof(name), "maps/%s.bsp", map);
	if (FS_FileExists(name))
		return 0;
	if (!TouchData_Active())
		return 2;
	if (progress && !strcmp(map, touch_data_want.string) && touch_data_want_total_kb.value > 0)
		*progress = bound(0, touch_data_want_done_kb.value / touch_data_want_total_kb.value, 1);
	return 1;
}

unsigned TouchData_Generation(void)
{
	unsigned g = (unsigned)Cvar_VariableValue(&cvars_all, "fs_mountpack_count", ~0);
	const char *p;
	g = g * 31 + (TouchData_Active() ? 1 : 0);
	for (p = touch_data_want.string; *p; p++)
		g = g * 31 + (unsigned char)*p;
	return g * 31 + (unsigned)(touch_data_want_done_kb.integer / 256);
}

void TouchData_Frame(void)
{
	char name[MAX_QPATH], line[MAX_INPUTLINE];

	if (cls.state == ca_dedicated)
		return;
	if (!touchdata_wrapped)
		TouchData_Wrap();
	if (host.realtime >= touchdata_nextpoll)
	{
		touchdata_nextpoll = host.realtime + 1;
		TouchData_Poll();
	}
	if (touch_data_pending.string[0])
	{
		dpsnprintf(name, sizeof(name), "maps/%s.bsp", touch_data_pending_map.string);
		if (FS_FileExists(name))
		{
			dpsnprintf(line, sizeof(line), "\n%s\n", touch_data_pending.string);
			TouchData_Cancel_f(NULL);
			Cbuf_AddText(cmd_local, line);
		}
	}
}

void TouchData_Init(void)
{
	Cvar_RegisterVariable(&touch_data_state);
	Cvar_RegisterVariable(&touch_data_set);
	Cvar_RegisterVariable(&touch_data_done_kb);
	Cvar_RegisterVariable(&touch_data_total_kb);
	Cvar_RegisterVariable(&touch_data_want);
	Cvar_RegisterVariable(&touch_data_want_done_kb);
	Cvar_RegisterVariable(&touch_data_want_total_kb);
	Cvar_RegisterVariable(&touch_data_message);
	Cvar_RegisterVariable(&touch_data_pending);
	Cvar_RegisterVariable(&touch_data_pending_map);
	Cmd_AddCommand(CF_CLIENT, "touch_data_status", TouchData_Status_f, "the data downloader's state as \"key value\" pairs: state, set, done_kb, total_kb, want, want_done_kb, want_total_kb, message, landed (Godnotic, docs/DATA_FETCH.md); without arguments prints it");
	Cmd_AddCommand(CF_CLIENT, "touch_data_want", TouchData_Want_f, "ask the data downloader to fetch a map's packs before the rest");
	Cmd_AddCommand(CF_CLIENT, "touch_data_cancel", TouchData_Cancel_f, "drop the command that waits for a map to download");
}
