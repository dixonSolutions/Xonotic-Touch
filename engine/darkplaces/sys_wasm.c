/*
 * Include this BEFORE darkplaces.h because it breaks wrapping
 * _Static_assert. Cloudwalk has no idea how or why so don't ask.
 */
#include <SDL.h>

#include "darkplaces.h"
#include "quakedef.h"
#include "fs.h"
#include "vid.h"

#include <emscripten.h>
#include <emscripten/html5.h>
#include <string.h>


// The canvas fills the page; the page shim (web/shell.js) decides how many
// pixels to render per CSS pixel (devicePixelRatio, capped for phones) and
// returns the framebuffer size here. SDL scales mouse coordinates from CSS
// pixels to window pixels itself, and touch arrives normalised.
EM_JS(int, js_GetViewportWidth, (void), {
	if (Module.xt && Module.xt.renderSize) return Module.xt.renderSize()[0];
	return document.documentElement.clientWidth;
});
EM_JS(int, js_GetViewportHeight, (void), {
	if (Module.xt && Module.xt.renderSize) return Module.xt.renderSize()[1];
	return document.documentElement.clientHeight;
});
static void em_fit_viewport(void)
{
	int w = js_GetViewportWidth(), h = js_GetViewportHeight();
	if (w < 64 || h < 64)
		return; // hidden or collapsed: keep the last good size
	if (w != vid_width.integer)
		Cvar_SetValueQuick(&vid_width, w);
	if (h != vid_height.integer)
		Cvar_SetValueQuick(&vid_height, h);
}

void Sys_EM_FitWindow(SDL_Window *window);
/// The browser window was resized (vid_sdl.c, on SDL_WINDOWEVENT_RESIZED).
/// SDL's own resize handler has just set the canvas to its CSS size, one
/// pixel per CSS pixel: put the cvars and the canvas back at the page's
/// render size. Resizing the canvas raises no browser resize event, so this
/// does not feed back into itself.
void Sys_EM_FitWindow(SDL_Window *window)
{
	int w, h;

	em_fit_viewport();
	SDL_GetWindowSize(window, &w, &h);
	if (w != vid_width.integer || h != vid_height.integer)
		SDL_SetWindowSize(window, vid_width.integer, vid_height.integer);
}

EM_JS(void, js_SetFullscreen, (int on), {
	if (Module.xt && Module.xt.setFullscreen) Module.xt.setFullscreen(!!on);
});
void Sys_EM_SetFullscreen(qbool on)
{
	js_SetFullscreen(on);
}

EM_JS(void, js_MouseGrab, (int on), {
	if (Module.xt && Module.xt.mouseGrab) Module.xt.mouseGrab(!!on);
});
void Sys_EM_MouseGrab(qbool on)
{
	js_MouseGrab(on);
}

// Whether the page has the keyboard: visible and focused. SDL tracks focus
// from window focus/blur events, and a blur with no focus after it (seen on
// a first visit, around fullscreen and browser UI) leaves the engine sure it
// is in the background while the page plainly has focus.
EM_JS(int, js_PageFocused, (void), {
	return (!document.hidden && document.hasFocus()) ? 1 : 0;
});
qbool Sys_EM_PageFocused(void)
{
	return js_PageFocused() != 0;
}

EM_JS(void, js_ScreenKeyboard, (int on), {
	if (Module.xt && Module.xt.screenKeyboard) Module.xt.screenKeyboard(!!on);
});
void Sys_EM_ScreenKeyboard(qbool on)
{
	js_ScreenKeyboard(on);
}

// ---- Called by the page shim ----

void xt_web_fullscreen_changed(int on);
void xt_web_refit(void);
void xt_web_command(const char *text);
void xt_web_save(void);
int xt_web_autosave(void);
void xt_web_text(const char *utf8);
void xt_web_key(int sym);

/// The page entered or left fullscreen (button, F11, Esc, swipe): keep the
/// menu's checkbox in step without asking the page again.
EMSCRIPTEN_KEEPALIVE void xt_web_fullscreen_changed(int on)
{
	if ((vid_fullscreen.integer != 0) != (on != 0))
		Cvar_SetValueQuick(&vid_fullscreen, on ? 1 : 0);
	em_fit_viewport();
}

/// The render scale changed (setting, or the window moved to another screen).
EMSCRIPTEN_KEEPALIVE void xt_web_refit(void)
{
	em_fit_viewport();
}

/// Run console text, e.g. from the page's own buttons or the dev console.
EMSCRIPTEN_KEEPALIVE void xt_web_command(const char *text)
{
	Cbuf_AddText(cmd_local, text);
	Cbuf_AddText(cmd_local, "\n");
}

/// Text from the page's hidden input (soft keyboards send composition, not
/// key events SDL can read): queue it as SDL_TEXTINPUT, as a typed key would.
EMSCRIPTEN_KEEPALIVE void xt_web_text(const char *utf8)
{
	SDL_Event ev;
	size_t len = strlen(utf8), off = 0;
	while (off < len)
	{
		size_t n = len - off;
		if (n > sizeof(ev.text.text) - 1)
		{
			n = sizeof(ev.text.text) - 1;
			while (n > 0 && ((unsigned char)utf8[off + n] & 0xC0) == 0x80)
				n--; // do not split a UTF-8 sequence
			if (n == 0)
				break;
		}
		memset(&ev, 0, sizeof(ev));
		ev.type = SDL_TEXTINPUT;
		memcpy(ev.text.text, utf8 + off, n);
		SDL_PushEvent(&ev);
		off += n;
	}
}

/// A key from the hidden input that has no text: 8 backspace, 13 enter,
/// 27 escape, 9 tab.
EMSCRIPTEN_KEEPALIVE void xt_web_key(int sym)
{
	SDL_Event ev;
	SDL_Keycode k = sym == 8 ? SDLK_BACKSPACE : sym == 13 ? SDLK_RETURN : sym == 27 ? SDLK_ESCAPE : sym == 9 ? SDLK_TAB : 0;
	if (!k)
		return;
	memset(&ev, 0, sizeof(ev));
	ev.type = SDL_KEYDOWN;
	ev.key.state = SDL_PRESSED;
	ev.key.keysym.sym = k;
	ev.key.keysym.scancode = SDL_GetScancodeFromKey(k);
	SDL_PushEvent(&ev);
	ev.type = SDL_KEYUP;
	ev.key.state = SDL_RELEASED;
	SDL_PushEvent(&ev);
}

extern qbool cvar_archive_changed; // cvar.c

/// A hash of one bindmap's key bindings, the part of config.cfg that cvar.c
/// does not track.
static unsigned int em_bindmap_hash(int map)
{
	unsigned int h = 2166136261u;
	const unsigned char *p;
	int key;

	for (key = 0; key < MAX_KEYS; key++)
		if (keybindings[map][key])
		{
			h = (h ^ (unsigned int)key) * 16777619u;
			for (p = (const unsigned char *)keybindings[map][key]; *p; p++)
				h = (h ^ *p) * 16777619u;
		}
	return h;
}
static unsigned int em_bindings_seen[MAX_BINDMAPS], em_bindings_saved[MAX_BINDMAPS];
static int em_bindings_next;
static qbool em_config_known;
static double em_config_savetime;

static void em_save_config(void)
{
	Host_SaveConfig(CONFIGFILENAME);
	cvar_archive_changed = false;
	memcpy(em_bindings_saved, em_bindings_seen, sizeof(em_bindings_saved));
	em_config_known = true;
	em_config_savetime = host.realtime;
}

/// The tab is going away or into the background, where the main loop stops:
/// write config.cfg now and flush it to IndexedDB.
EMSCRIPTEN_KEEPALIVE void xt_web_save(void)
{
	if (host.state == host_active || host.state == host_init)
		em_save_config();
}

/// Called by the page a few times a second while the game runs. Writes
/// config.cfg when an archived cvar or a key binding changed, at most every
/// 2 seconds and never while a map loads, and returns 1 so the page flushes
/// it to IndexedDB at once. The saves on unload alone lose settings: the
/// IndexedDB write they start is async, and a reload or a killed tab ends
/// the page before it lands.
///
/// Bindings are compared by hash, one bindmap per call: the table has a slot
/// for every Unicode key, and a full pass costs most of a millisecond. The
/// first save after startup is unconditional: what changed between reading
/// config.cfg and now is not known.
EMSCRIPTEN_KEEPALIVE int xt_web_autosave(void)
{
	int map;
	qbool changed;

	if (host.state != host_active || host.framecount < 3)
		return 0;
	if (!em_config_known)
	{
		for (map = 0; map < MAX_BINDMAPS; map++)
			em_bindings_seen[map] = em_bindmap_hash(map);
	}
	else
	{
		em_bindings_seen[em_bindings_next] = em_bindmap_hash(em_bindings_next);
		em_bindings_next = (em_bindings_next + 1) % MAX_BINDMAPS;
	}
	if (cls.state == ca_connected && cls.signon < SIGNONS)
		return 0;
	if (em_config_known && host.realtime - em_config_savetime < 2)
		return 0;
	changed = !em_config_known || cvar_archive_changed
		|| memcmp(em_bindings_seen, em_bindings_saved, sizeof(em_bindings_saved));
	if (!changed)
		return 0;
	em_save_config();
	return 1;
}

// =======================================================================
// General routines
// =======================================================================

#ifdef WASM_USER_ADJUSTABLE
EM_JS(char *, js_listfiles, (const char *directory), {
	if(UTF8ToString(directory) == "")
	{
		console.log("listing cwd");
		return stringToNewUTF8(FS.readdir(FS.cwd()).toString());
	}

	try
	{
		return stringToNewUTF8(FS.readdir(UTF8ToString(directory)).toString());
	}
	catch (error)
	{
		return stringToNewUTF8("directory not found");
	}
});
static void em_listfiles_f(cmd_state_t *cmd)
{
	char *output = js_listfiles(Cmd_Argc(cmd) == 2 ? Cmd_Argv(cmd, 1) : "");

	Con_Printf("%s\n", output);
	free(output);
}

EM_JS(char *, js_upload, (const char *todirectory), {
	if (UTF8ToString(todirectory).slice(-1) != "/")
	{
		currentname = UTF8ToString(todirectory) + "/";
	}
	else
	{
		currentname = UTF8ToString(todirectory);
	}

	file_selector.click();
	return stringToNewUTF8("Upload started");
});
static void em_upload_f(cmd_state_t *cmd)
{
	char *output = js_upload(Cmd_Argc(cmd) == 2 ? Cmd_Argv(cmd, 1) : fs_basedir);

	Con_Printf("%s\n", output);
	free(output);
}

EM_JS(char *, js_rm, (const char *path), {
	const mode = FS.lookupPath(UTF8ToString(path)).node.mode;

	if (FS.isFile(mode))
	{
		FS.unlink(UTF8ToString(path));
		return stringToNewUTF8("File removed");
	}

	return stringToNewUTF8(UTF8ToString(path)+" is not a File.");
});
static void em_rm_f(cmd_state_t *cmd)
{
	if (Cmd_Argc(cmd) != 2)
		Con_Printf("No file to remove\n");
	else
	{
		char *output = js_rm(Cmd_Argv(cmd, 1));
		Con_Printf("%s\n", output);
		free(output);
	}
}

EM_JS(char *, js_rmdir, (const char *path), {
	const mode = FS.lookupPath(UTF8ToString(path)).node.mode;
	if (FS.isDir(mode))
	{
		try
		{
			FS.rmdir(UTF8ToString(path));
		}
		catch (error)
		{
			return stringToNewUTF8("Unable to remove directory. Is it not empty?");
		}
		return stringToNewUTF8("Directory removed");
	}

	return stringToNewUTF8(UTF8ToString(path)+" is not a directory.");
});
static void em_rmdir_f(cmd_state_t *cmd)
{
	if (Cmd_Argc(cmd) != 2)
		Con_Printf("No directory to remove\n");
	else
	{
		char *output = js_rmdir(Cmd_Argv(cmd, 1));
		Con_Printf("%s\n", output);
		free(output);
	}
}

EM_JS(char *, js_mkd, (const char *path), {
	try
	{
		FS.mkdir(UTF8ToString(path));
	}
	catch (error)
	{
		return stringToNewUTF8("Unable to create directory. Does it already exist?");
	}
	return stringToNewUTF8(UTF8ToString(path)+" directory was created.");
});
static void em_mkdir_f(cmd_state_t *cmd)
{
	if (Cmd_Argc(cmd) != 2)
		Con_Printf("No directory to create\n");
	else
	{
		char *output = js_mkd(Cmd_Argv(cmd, 1));
		Con_Printf("%s\n", output);
		free(output);
	}
}

EM_JS(char *, js_move, (const char *oldpath, const char *newpath), {
	try
	{
		FS.rename(UTF8ToString(oldpath),UTF8ToString(newpath))
	}
	catch (error)
	{
		return stringToNewUTF8("unable to move.");
	}
	return stringToNewUTF8("File Moved");
});
static void em_mv_f(cmd_state_t *cmd)
{
	if (Cmd_Argc(cmd) != 3)
		Con_Printf("Nothing to move\n");
	else
	{
		char *output = js_move(Cmd_Argv(cmd,1), Cmd_Argv(cmd,2));
		Con_Printf("%s\n", output);
		free(output);
	}
}

static void em_wss_f(cmd_state_t *cmd)
{
	if (Cmd_Argc(cmd) != 3)
		Con_Printf("Not Enough Arguments (Expected URL and subprotocol)\n");
	else
	{
		if(strcmp(Cmd_Argv(cmd,2),"binary") == 0 || strcmp(Cmd_Argv(cmd,2),"text") == 0)
			Con_Printf("Set Websocket URL to %s and subprotocol to %s.\n", Cmd_Argv(cmd,1), Cmd_Argv(cmd,2));
		else
			Con_Printf("subprotocol must be either binary or text\n");
	}
}
#endif // WASM_USER_ADJUSTABLE

// Writes land in MEMFS; the page shim mirrors the userdir to IndexedDB,
// coalescing bursts (saveconfig writes several files in a row).
EM_JS(bool, js_syncFS, (bool populate), {
	if (Module.xt && Module.xt.syncFS) { Module.xt.syncFS(); return true; }
	FS.syncfs(false, function(err) { if (err) console.error('syncfs: ' + err); });
	return true;
});
static void em_savefs_f(cmd_state_t *cmd)
{
	Con_Printf("Saving Files\n");
	js_syncFS(false);
}

// The engine has shut down (menu Quit, the quit command). exit() does not
// end a page whose runtime is kept alive, so Module.onExit never runs: tell
// the page, which offers to start again.
EM_JS(void, js_quit, (void), {
	if (Module.xt && Module.xt.quit) Module.xt.quit();
});
void Sys_SDL_Shutdown(void)
{
	js_syncFS(false);
	SDL_Quit();
	js_quit();
}

// A fatal error: the page shows it over the game (and the last console
// lines) instead of leaving a frozen or black canvas.
EM_JS(void, js_fatal, (const char *title, const char *text), {
	var t = UTF8ToString(title), m = UTF8ToString(text);
	if (Module.xt && Module.xt.fatal) Module.xt.fatal(t, m);
	else console.error(t + ': ' + m);
});
static qbool nocrashdialog = true;
void Sys_SDL_Dialog(const char *title, const char *string)
{
	(void)nocrashdialog;
	js_fatal(title, string);
}

// In a browser the clipboard can only be read asynchronously
// doing this efficiently and cleanly requires JSPI
// enable in makefile.inc with emcc option: -s JSPI
/* TODO: enable this code when JSPI is enabled by default in browsers
EM_ASYNC_JS(char *, js_getclipboard, (void),
{
	try
	{
		const text = await navigator.clipboard.readText();
		return stringToNewUTF8(text);
	}
	catch (err)
	{
		return stringToNewUTF8("clipboard error: ", err);
	}
}); */
EM_JS(char *, js_getclipboard, (void), {
	return stringToNewUTF8("clipboard access requires JSPI which is not currently enabled.");
});
char *Sys_SDL_GetClipboardData (void)
{
	char *data = NULL;
	char *cliptext;

	// SDL_GetClipboardText() does nothing in a browser, see above
	cliptext = js_getclipboard();
	if (cliptext != NULL) {
		size_t allocsize;
		allocsize = min(MAX_INPUTLINE, strlen(cliptext) + 1);
		data = (char *)Z_Malloc (allocsize);
		dp_strlcpy (data, cliptext, allocsize);
		free(cliptext);
	}

	return data;
}

void Sys_SDL_Init(void)
{
	if (SDL_Init(0) < 0)
		Sys_Error("SDL_Init failed: %s\n", SDL_GetError());

	// we don't know which systems we'll want to init, yet...
	// COMMANDLINEOPTION: sdl: -nocrashdialog disables "Engine Error" crash dialog boxes
	if(!Sys_CheckParm("-nocrashdialog"))
		nocrashdialog = false;

}

void Sys_EM_Register_Commands(void)
{
#ifdef WASM_USER_ADJUSTABLE
	Cmd_AddCommand(CF_SHARED, "em_ls", em_listfiles_f, "Lists Files in specified directory defaulting to the current working directory (Emscripten Only)");
	Cmd_AddCommand(CF_SHARED, "em_upload", em_upload_f, "Upload file to specified directory defaulting to basedir (Emscripten Only)");
	Cmd_AddCommand(CF_SHARED, "em_rm", em_rm_f, "Remove a file from game Filesystem (Emscripten Only)");
	Cmd_AddCommand(CF_SHARED, "em_rmdir", em_rmdir_f, "Remove a directory from game Filesystem (Emscripten Only)");
	Cmd_AddCommand(CF_SHARED, "em_mkdir", em_mkdir_f, "Make a directory in game Filesystem (Emscripten Only)");
	Cmd_AddCommand(CF_SHARED, "em_mv", em_mv_f, "Rename or Move an item in game Filesystem (Emscripten only)");
	Cmd_AddCommand(CF_SHARED, "em_wss", em_wss_f, "Set Websocket URL and Protocol (Emscripten Only)");
#endif
	Cmd_AddCommand(CF_SHARED, "em_save", em_savefs_f, "Save file changes to browser (Emscripten Only)");
}

qbool sys_supportsdlgetticks = true;
unsigned int Sys_SDL_GetTicks(void)
{
	return SDL_GetTicks();
}

void Sys_SDL_Delay(unsigned int milliseconds)
{
	SDL_Delay(milliseconds);
}

int main(int argc, char *argv[])
{
	return Sys_Main(argc, argv);
}
