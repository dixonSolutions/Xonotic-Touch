/*
 * Touch text-sheet: keyboard layers, command palette, console/chat/preview layout.
 */
#include "quakedef.h"
#include "touch_ui.h"
#include "touch_shellbtn.h"
#include "draw.h"
#include "ft2.h"
#include "utf8lib.h"
// OBJ_COMMON also goes into the dedicated server, which is built without SDL.
#if defined(__has_include)
# if __has_include(<SDL.h>)
#  include <SDL.h>
#  if SDL_VERSION_ATLEAST(2, 0, 14)
#   define TOUCHUI_SDL_LOCALE 1
#  endif
# endif
#endif

cvar_t touch_kb_height = {CF_CLIENT | CF_ARCHIVE, "touch_kb_height", "0.46", "fraction of sheet height used by the touch keyboard"};
cvar_t touch_kb_gap = {CF_CLIENT | CF_ARCHIVE, "touch_kb_gap", "0.008", "gap between keys as a fraction of sheet width"};
cvar_t touch_kb_opacity = {CF_CLIENT | CF_ARCHIVE, "touch_kb_opacity", "0.92", "opacity of touch keyboard glass plates"};
cvar_t touch_kb_layout = {CF_CLIENT | CF_ARCHIVE, "touch_kb_layout", "0", "0 = qwerty, 1 = compact"};
cvar_t touch_kb_split = {CF_CLIENT | CF_ARCHIVE, "touch_kb_split", "0", "1 = split keyboard under both thumbs in landscape"};
cvar_t touch_kb_minkey_px = {CF_CLIENT | CF_ARCHIVE, "touch_kb_minkey_px", "36", "warn when a key's smaller dimension is below this (device pixels)"};
cvar_t touch_kb_hold_time = {CF_CLIENT | CF_ARCHIVE, "touch_kb_hold_time", "0.45", "seconds a key is held before its accented and special characters pop up (0 = never)"};
cvar_t touch_conui_shade = {CF_CLIENT | CF_ARCHIVE, "touch_conui_shade", "0.62", "console sheet background shade alpha"};
cvar_t touch_conui_palette_file = {CF_CLIENT | CF_ARCHIVE, "touch_conui_palette_file", "touch/console_palette.txt", "file of preset console commands for the COMMANDS tab"};
/* Not archived: touch/xonotic.cfg sets it each start, like scr_loadingscreen_picture. */
cvar_t touch_menu_keyboard = {CF_CLIENT | CF_ARCHIVE, "touch_menu_keyboard", "0", "keyboard for menu text fields on a touch screen: 0 = automatic (the game's own keys for languages written in Latin letters, with no keyboard attached; the system keyboard and its input methods for Chinese, Japanese, Korean, Cyrillic, Greek and others), 1 = the game's own keys, 2 = the system keyboard"};
static cvar_t touch_menu_keys_top = {CF_CLIENT | CF_READONLY, "_touch_menu_keys_top", "0", "top of the engine's keys under a menu text field, as a fraction of the screen height; 0 while they are not shown (touch_menu_sheet.qc reads it)"};
static cvar_t touch_menu_keys_ask = {CF_CLIENT, "_touch_menu_keys_ask", "0", "set by the menu: 1 when a tap lands on a text field (the engine's keys come back after DONE), -1 when Back puts them away"};
cvar_t touch_conback_picture = {CF_CLIENT, "touch_conback_picture", "", "console background picture used instead of gfx/conback when it loads (empty = gfx/conback); a name only this build ships, since downloaded data carries its own gfx/conback"};
#define TOUCHUI_MAX_FINGERS 11

static touchui_tab_t touchui_tab = TOUCHUI_TAB_KEYS;
static touchui_layer_t touchui_layer = TOUCHUI_LAYER_ABC;
static touchui_palette_entry_t touchui_palette[TOUCHUI_MAX_PALETTE];
static int touchui_palette_count;
static unsigned int touchui_typed_mask;
static double touchui_bksp_next;
static qbool touchui_bksp_held;
static qbool touchui_minkey_warned;
static char touchui_palette_loaded_path[MAX_QPATH];
static int touchui_bksp_finger = -1;

/*
 * Keys shown or hidden. With the touch controls on and no keyboard listed the
 * sheet shows its keys, as it always has; otherwise they start hidden and the
 * KEYBOARD toggle in the header opens them. The player's choice holds until
 * a keyboard is plugged in or out or the input mode switches.
 */
static int touchui_keys_override = -1;
static int touchui_keys_override_for;

static float touchui_chat_keys_top = -1;

float TouchUI_ChatKeysTop(void)
{
	return touchui_chat_keys_top;
}

/* Top of the keys or command palette under the console, sheet units; -1 none. */
static float touchui_console_keys_top = -1;
static double touchui_console_keys_time;

/*
 * Console units per dp: from the density Android reports with the insets
 * (_touch_safe_insets), the engine's display reading (vid_touch_dpi), the
 * Linux launcher's measurement, or a phone-sized guess -- the same sources,
 * in the same order, as the menu's size (touch_menu_scale.qc).
 */
static float TouchUI_ConPerDp(void)
{
	float density = 0, l, t, r, b;
	cvar_t *v = Cvar_FindVar(&cvars_all, "_touch_safe_insets", ~0);
	if (v && v->string[0])
		if (sscanf(v->string, "%f %f %f %f %f", &l, &t, &r, &b, &density) < 5)
			density = 0;
	if (density < 0.4f)
	{
		v = Cvar_FindVar(&cvars_all, "vid_touch_dpi", ~0);
		if (v && v->value >= 72)
			density = v->value / 160.0f;
	}
	if (density < 0.4f)
	{
		v = Cvar_FindVar(&cvars_all, "vid_touchscreen_ydpi", ~0);
		if (v && v->value >= 72 && v->value != 160 && v->value != 300)
			density = v->value / 160.0f;
	}
	if (density < 0.4f)
	{
		float shortside = (float)min(vid.mode.width, vid.mode.height);
		density = bound(1.0f, shortside / 420.0f, 4.0f);
	}
	return vid.mode.height > 0 ? density * vid_conheight.value / (float)vid.mode.height : density;
}

/* Gap around each key: touch_kb_gap of the width, at most 3 dp, so six rows of
 * finger-sized keys fit a phone's short side. */
static float TouchUI_KeyGap(float w)
{
	return min(bound(2.0f, touch_kb_gap.value * w, 16.0f), max(2.0f, 3.0f * TouchUI_ConPerDp()));
}

/* The sheet's header row (CLOSE, the tabs, KEYBOARD): 48 dp, as the menu's
 * rows (touch_menu_scale.qc). touch_shellbtn.c keeps the console button under it. */
float TouchUI_SheetHeaderHeight(float h)
{
	float hdr_h = bound(36.0f, h * 0.10f, 64.0f);
	return min(max(hdr_h, 48.0f * TouchUI_ConPerDp()), h * 0.16f);
}

/*
 * Keyboard height: touch_kb_height of the sheet, but never so short that a
 * key is under TOUCHUI_MIN_KEY_DP (nor under touch_kb_minkey_px device
 * pixels), and never so tall that fewer than TOUCHUI_MIN_LOG_DP of console
 * is left above it for the log and the input line.
 */
#define TOUCHUI_MIN_KEY_DP 44.0f
#define TOUCHUI_MIN_LOG_DP 72.0f
static float TouchUI_KeysHeight(float w, float h, float hdr_h, float kb_frac)
{
	float dp = TouchUI_ConPerDp();
	float gap = TouchUI_KeyGap(w);
	float px2con = vid.mode.height > 0 ? vid_conheight.value / (float)vid.mode.height : 1.0f;
	float key = max(TOUCHUI_MIN_KEY_DP * dp, touch_kb_minkey_px.value * px2con);
	float kb_h = max(h * kb_frac, 6.0f * (key + gap * 2.0f));
	return min(kb_h, h - hdr_h - TOUCHUI_MIN_LOG_DP * dp);
}

/* SCR_SetUpToDrawConsole: the console stops above the keys, so the input line
 * stays in sight while typing. */
float TouchUI_ConsoleHeight(float con_h)
{
	if (touchui_console_keys_top <= 0 || host.realtime - touchui_console_keys_time > 0.5)
		return con_h;
	return min(con_h, touchui_console_keys_top);
}

/* Sheet rect of the last layout, to keep the hold popup on screen. */
static float touchui_sheet_x, touchui_sheet_y, touchui_sheet_w, touchui_sheet_h;

/*
 * Hold for special characters, as on iOS. A tap types at once, as before:
 * the character goes in on the press. Held for touch_kb_hold_time on a key
 * with variants, a row of them opens above the key, the key's own character
 * first. Sliding picks one, and releasing replaces the typed character with
 * it. Released on the key's own character it stays; released away from the
 * row it is taken back.
 */
static const struct { char base; const char *variants; } touchui_variant_table[] = {
	{ 'a', "àáâäæãåāąă" }, { 'A', "ÀÁÂÄÆÃÅĀĄĂ" },
	{ 'e', "èéêëēęě" },    { 'E', "ÈÉÊËĒĘĚ" },
	{ 'i', "ìíîïīı" },     { 'I', "ÌÍÎÏĪİ" },
	{ 'o', "òóôöõøœōő" },  { 'O', "ÒÓÔÖÕØŒŌŐ" },
	{ 'u', "ùúûüūůű" },    { 'U', "ÙÚÛÜŪŮŰ" },
	{ 'c', "çćč" },        { 'C', "ÇĆČ" },
	{ 'n', "ñńň" },        { 'N', "ÑŃŇ" },
	{ 's', "ßśšşș" },      { 'S', "ẞŚŠŞȘ" },
	{ 't', "ťțþ" },        { 'T', "ŤȚÞ" },
	{ 'd', "ďđð" },        { 'D', "ĎĐÐ" },
	{ 'r', "řŕ" },         { 'R', "ŘŔ" },
	{ 'g', "ğ" },          { 'G', "Ğ" },
	{ 'y', "ýÿ" },         { 'Y', "ÝŸ" },
	{ 'z', "žźż" },        { 'Z', "ŽŹŻ" },
	{ 'l', "łľ" },         { 'L', "ŁĽ" },
	{ '-', "–—" },
	{ '\'', "‘’" },
	{ '"', "“”" },
	{ '?', "¿" },
	{ '!', "¡" },
	{ '$', "€£¥" },
	{ '.', "…" },
};
#define TOUCHUI_VARIANT_COUNT ((int)(sizeof(touchui_variant_table) / sizeof(touchui_variant_table[0])))
#define TOUCHUI_POPUP_MAX 12

typedef struct touchui_popup_s
{
	qbool open;
	int finger;
	int count;                       /* cells; [0] is the key's own character */
	Uchar ch[TOUCHUI_POPUP_MAX];
	float cx[TOUCHUI_POPUP_MAX];     /* cell left edges */
	float y, cw, chh;                /* row top, cell size */
	float key_y, key_h;              /* the held key, for the slide band */
	int sel;                         /* picked cell, -1 = away from the row */
}
touchui_popup_t;
static touchui_popup_t touchui_popup;

/* Where each finger first pressed a character key, for the hold timer. */
static struct
{
	qbool armed;
	double time;
	int ch;
	float x, y, w, h;
}
touchui_press[TOUCHUI_MAX_FINGERS];

/*
 * Spectator actions (issue #26). While spectating or observing on a server the
 * game prints prompts that name keys: "Press SPACE to join", "Press primary
 * fire to spectate", "next weapon or previous weapon for next or previous
 * player", "drop weapon to change camera mode". On public servers those come
 * from the server's own CSQC, which the touch layer cannot rewrite, and a touch
 * player has none of those keys. The COMMANDS tab leads with the same actions
 * while they apply, running the command each prompt names.
 *
 * Buttons are pressed and released a moment later with defer. A "-fire" run in
 * the same frame as "+fire" clears the press before any packet carries it
 * (KeyUp with no key number resets the button), so the server never saw it.
 */
typedef enum touchui_specmode_e
{
	TOUCHUI_SPEC_NONE = 0,
	TOUCHUI_SPEC_OBSERVING,   /* free-flying observer (spectatee_status -1) */
	TOUCHUI_SPEC_WATCHING,    /* spectating another player */
	TOUCHUI_SPEC_OUT,         /* out of the round (-616), watching others */
	TOUCHUI_SPEC_OUT_FREE     /* out of the round, free-flying */
}
touchui_specmode_t;

typedef struct touchui_specentry_s
{
	const char *label;
	const char *command;
	unsigned int modes;       /* bit per touchui_specmode_t */
}
touchui_specentry_t;

#define TOUCHUI_SPEC_BIT(m) (1u << (m))
static const touchui_specentry_t touchui_spec_entries[] = {
	/* "Press primary fire to spectate" */
	{ "spectate", "+fire\ndefer 0.2 -fire", TOUCHUI_SPEC_BIT(TOUCHUI_SPEC_OBSERVING) | TOUCHUI_SPEC_BIT(TOUCHUI_SPEC_OUT_FREE) },
	/* "Press next weapon or previous weapon for next or previous player" */
	{ "next player", "weapnext", TOUCHUI_SPEC_BIT(TOUCHUI_SPEC_WATCHING) | TOUCHUI_SPEC_BIT(TOUCHUI_SPEC_OUT) },
	{ "prev player", "weapprev", TOUCHUI_SPEC_BIT(TOUCHUI_SPEC_WATCHING) | TOUCHUI_SPEC_BIT(TOUCHUI_SPEC_OUT) },
	/* "Use next weapon or previous weapon to change the speed" */
	{ "fly faster", "weapnext", TOUCHUI_SPEC_BIT(TOUCHUI_SPEC_OBSERVING) },
	{ "fly slower", "weapprev", TOUCHUI_SPEC_BIT(TOUCHUI_SPEC_OBSERVING) },
	/* "Press secondary fire to observe, drop weapon to change camera mode" */
	{ "observe", "+fire2\ndefer 0.2 -fire2", TOUCHUI_SPEC_BIT(TOUCHUI_SPEC_WATCHING) },
	{ "camera", "weapon_drop", TOUCHUI_SPEC_BIT(TOUCHUI_SPEC_WATCHING) | TOUCHUI_SPEC_BIT(TOUCHUI_SPEC_OUT) },
	/* "Press jump to join" */
	{ "join", "join", TOUCHUI_SPEC_BIT(TOUCHUI_SPEC_OBSERVING) | TOUCHUI_SPEC_BIT(TOUCHUI_SPEC_WATCHING) },
};
#define TOUCHUI_SPEC_COUNT ((int)(sizeof(touchui_spec_entries) / sizeof(touchui_spec_entries[0])))

/* Entries shown by the last layout, so a tap runs what was drawn under it. */
static int touchui_spec_shown[TOUCHUI_SPEC_COUNT];
static int touchui_spec_shown_count;

static touchui_specmode_t TouchUI_SpectatorMode(void)
{
	int me = cl.realplayerentity;
	int frags;

	if (cls.state != ca_connected || cls.signon != SIGNONS || cls.demoplayback)
		return TOUCHUI_SPEC_NONE;
	if (cl.intermission || !cl.scores || me < 1 || me > cl.maxclients)
		return TOUCHUI_SPEC_NONE;
	/* Nexuiz-derived games mark spectators -666 and players out of the round
	 * -616 (touch_aim.c, cl_collision.c). A spectator's view is moved to the
	 * player they watch with svc_setview. */
	frags = cl.scores[me - 1].frags;
	/* Xonotic since 0.8.6 marks a player out of the round -669
	 * (FRAGS_PLAYER_OUT_OF_GAME): the same as -616 here. */
	if (frags == -669)
		frags = -616;
	if (frags != -666 && frags != -616)
		return TOUCHUI_SPEC_NONE;
	if (cl.viewentity != me && cl.viewentity >= 1 && cl.viewentity <= cl.maxclients)
		return frags == -666 ? TOUCHUI_SPEC_WATCHING : TOUCHUI_SPEC_OUT;
	return frags == -666 ? TOUCHUI_SPEC_OBSERVING : TOUCHUI_SPEC_OUT_FREE;
}

/* Observing or watching: jump means "join" there, not a hop (touch_hud.c). */
qbool TouchUI_IsSpectating(void)
{
	return TouchUI_SpectatorMode() != TOUCHUI_SPEC_NONE;
}

qbool TouchUI_IsWatching(void)
{
	return TouchUI_SpectatorMode() == TOUCHUI_SPEC_WATCHING;
}

/* As TOUCHSPEC_* (touch_spectate.h): 0 playing, 1 observing, 2 following a
 * player, 3 out of the round and watching, 4 out of the round, free-flying. */
int TouchUI_SpectatorState(void)
{
	switch (TouchUI_SpectatorMode())
	{
	case TOUCHUI_SPEC_OBSERVING: return 1;
	case TOUCHUI_SPEC_WATCHING: return 2;
	case TOUCHUI_SPEC_OUT: return 3;
	case TOUCHUI_SPEC_OUT_FREE: return 4;
	default: return 0;
	}
}

static void TouchUI_UpdateSpectatorEntries(void)
{
	touchui_specmode_t mode = TouchUI_SpectatorMode();
	int i;

	touchui_spec_shown_count = 0;
	if (mode == TOUCHUI_SPEC_NONE)
		return;
	for (i = 0; i < TOUCHUI_SPEC_COUNT; i++)
		if (touchui_spec_entries[i].modes & TOUCHUI_SPEC_BIT(mode))
			touchui_spec_shown[touchui_spec_shown_count++] = i;
}

/* Built-in palette if the file is missing. */
static const struct { const char *label; const char *cmd; } touchui_builtin_palette[] = {
	{ "disconnect", "disconnect" },
	{ "reconnect", "reconnect" },
	{ "screenshot", "screenshot" },
	{ "status", "status" },
	{ "name", "name" },
	{ "timelimit", "timelimit" },
	{ "gg", "say good game" },
	{ "hi", "say hi / good luck" },
	{ "nice", "say :-) / nice one" },
	{ "oops", "say oops" },
	{ "thanks", "say thanks" },
	{ "ready", "ready" },
};

void TouchUI_RegisterCvars(void)
{
	Cvar_RegisterVariable(&touch_kb_height);
	Cvar_RegisterVariable(&touch_kb_gap);
	Cvar_RegisterVariable(&touch_kb_opacity);
	Cvar_RegisterVariable(&touch_kb_layout);
	Cvar_RegisterVariable(&touch_kb_split);
	Cvar_RegisterVariable(&touch_kb_minkey_px);
	Cvar_RegisterVariable(&touch_kb_hold_time);
	Cvar_RegisterVariable(&touch_conui_shade);
	Cvar_RegisterVariable(&touch_conui_palette_file);
	Cvar_RegisterVariable(&touch_conback_picture);
	Cvar_RegisterVariable(&touch_menu_keyboard);
	Cvar_RegisterVariable(&touch_menu_keys_top);
	Cvar_RegisterVariable(&touch_menu_keys_ask);
}

void TouchUI_Init(void)
{
	TouchShell_Init(); // its cvars have to exist before a config or the platform sets them
	touchui_tab = TOUCHUI_TAB_KEYS;
	touchui_layer = TOUCHUI_LAYER_ABC;
	touchui_typed_mask = 0;
	touchui_bksp_held = false;
	touchui_bksp_next = 0;
	touchui_minkey_warned = false;
	touchui_palette_loaded_path[0] = 0;
	TouchUI_ReloadPalette();
}

void TouchUI_Shutdown(void)
{
	touchui_palette_count = 0;
}

touchui_tab_t TouchUI_GetTab(void)
{
	return touchui_tab;
}

touchui_layer_t TouchUI_GetLayer(void)
{
	return touchui_layer;
}

float TouchUI_GetShadeAlpha(void)
{
	return bound(0.0f, touch_conui_shade.value, 1.0f);
}

float TouchUI_GetKeyOpacity(void)
{
	return bound(0.3f, touch_kb_opacity.value, 1.0f);
}

void TouchUI_ResetInputState(void)
{
	touchui_typed_mask = 0;
	touchui_bksp_held = false;
	touchui_bksp_next = 0;
	touchui_bksp_finger = -1;
	touchui_popup.open = false;
	memset(touchui_press, 0, sizeof(touchui_press));
}

/* Touch controls on or off, keyboard listed or not. */
static int TouchUI_KeysContext(void)
{
	return (vid_touchscreen.integer ? 2 : 0) | (VID_HasKeyboard() ? 1 : 0);
}

qbool TouchUI_KeysShown(touchui_sheet_t kind)
{
	int ctx;
	/* The settings preview is a picture of the touch sheet. */
	if (kind == TOUCHUI_SHEET_PREVIEW)
		return true;
	ctx = TouchUI_KeysContext();
	if (touchui_keys_override >= 0 && touchui_keys_override_for != ctx)
		touchui_keys_override = -1;
	if (touchui_keys_override >= 0)
		return touchui_keys_override != 0;
	return ctx == 2; /* touch controls on, no keyboard */
}

/*
 * Menu text fields: the engine's keys instead of the system keyboard
 * (touch_ui.h). One keyboard everywhere, with the hold row for accents, and
 * nothing typed into a field ever reaches the system's input method.
 */
static qbool touchui_menu_dismissed;   /* DONE: away until a tap lands on a field */
static qbool touchui_menu_was_up;
static qbool touchui_layout_menu;      /* TouchUI_LayoutKeyboard lays out the menu's rows */
static qbool touchui_processing_menu;  /* the last layout was the menu keys */
static float touchui_menu_top;         /* top of the menu keys, sheet units */
static unsigned int touchui_menu_seen, touchui_menu_foreign; /* fingers, and those that landed off the keys */

/* Languages whose letters the keys (with the hold row) type: ISO 639 codes. */
static qbool TouchUI_LatinLanguage(const char *code)
{
	static const char *latin[] = {
		"en", "de", "fr", "es", "it", "pt", "nl", "sv", "da", "fi", "nb", "nn", "no", "is",
		"ca", "gl", "eu", "ast", "oc", "pl", "cs", "sk", "hu", "ro", "tr", "hr", "sl", "bs",
		"sq", "et", "id", "ms", "la", "ga", "cy", "eo", "af", "sw", "fil", "tl", NULL
	};
	char lang[8];
	size_t i;
	int k;
	if (!code || !code[0])
		return true; /* unknown: the stock English */
	for (i = 0; i < sizeof(lang) - 1 && code[i] && code[i] != '_' && code[i] != '-' && code[i] != '.'; i++)
		lang[i] = (char)tolower((unsigned char)code[i]);
	lang[i] = 0;
	for (k = 0; latin[k]; k++)
		if (!strcmp(lang, latin[k]))
			return true;
	return false;
}

/* The game's language (prvm_language) and the system's first one both
 * written in Latin letters. A Japanese system with the game in English still
 * gets the system keyboard: names and chat are typed in the system's language. */
static qbool TouchUI_LatinLocale(void)
{
	cvar_t *lang = Cvar_FindVar(&cvars_all, "prvm_language", ~0);
	if (lang && lang->string[0] && strcmp(lang->string, "dump") && !TouchUI_LatinLanguage(lang->string))
		return false;
#ifdef TOUCHUI_SDL_LOCALE
	{
		static double checked = -1000;
		static qbool latin_sys = true;
		if (host.realtime - checked > 10 || host.realtime < checked)
		{
			SDL_Locale *loc = SDL_GetPreferredLocales();
			checked = host.realtime;
			latin_sys = !loc || !loc[0].language || TouchUI_LatinLanguage(loc[0].language);
			if (loc)
				SDL_free(loc);
		}
		return latin_sys;
	}
#else
	return true;
#endif
}

static qbool TouchUI_MenuKeysWanted(void)
{
	if (touch_menu_keyboard.integer == 1)
		return true;
	if (touch_menu_keyboard.integer == 2)
		return false;
	/* A keyboard to type on: no keys over the menu. */
	if (VID_HasKeyboard())
		return false;
	return TouchUI_LatinLocale();
}

qbool TouchUI_MenuKeysUp(void)
{
	keydest_t kd = (key_consoleactive & KEY_CONSOLEACTIVE_USER) ? key_console : key_dest;
	qbool up;
	/* 1: a tap on a field brings them back; -1: Back put them away. */
	if (touch_menu_keys_ask.integer)
	{
		touchui_menu_dismissed = touch_menu_keys_ask.integer < 0;
		Cvar_SetValueQuick(&touch_menu_keys_ask, 0);
	}
	/* The field let go: the next one gets the keys again. */
	if (!vid_touchscreen_showkeyboard.integer)
		touchui_menu_dismissed = false;
	up = (kd == key_menu || kd == key_menu_grabbed) && vid_touchscreen.integer
		&& vid_touchscreen_showkeyboard.integer && !touchui_menu_dismissed && TouchUI_MenuKeysWanted();
	if (!up && touchui_menu_was_up)
	{
		TouchUI_ResetInputState();
		touchui_menu_seen = touchui_menu_foreign = 0;
		touchui_layer = TOUCHUI_LAYER_ABC;
	}
	if (!up && touch_menu_keys_top.value != 0)
		Cvar_SetValueQuick(&touch_menu_keys_top, 0);
	touchui_menu_was_up = up;
	return up;
}

qbool TouchUI_EngineKeysUp(void)
{
	keydest_t keydest = (key_consoleactive & KEY_CONSOLEACTIVE_USER) ? key_console : key_dest;
	/* A menu text field with the engine's keys: no system keyboard over them. */
	if (TouchUI_MenuKeysUp() && !VID_HasKeyboard())
		return true;
	if (keydest != key_console && keydest != key_message)
		return false;
	/* A listed keyboard types through SDL text input, which has to stay on. */
	if (VID_HasKeyboard())
		return false;
	return TouchUI_KeysShown(keydest == key_console ? TOUCHUI_SHEET_CONSOLE : TOUCHUI_SHEET_CHAT);
}

cachepic_t *TouchUI_ConbackPic(cachepic_t *stock, unsigned int flags)
{
	cachepic_t *pic;
	if (!stock || !touch_conback_picture.string[0])
		return stock;
	pic = Draw_CachePic_Flags(touch_conback_picture.string, flags);
	return Draw_IsPicLoaded(pic) ? pic : stock;
}

static void TouchUI_ToggleKeys(void)
{
	touchui_keys_override = TouchUI_KeysShown(TOUCHUI_SHEET_CONSOLE) ? 0 : 1;
	touchui_keys_override_for = TouchUI_KeysContext();
	touchui_popup.open = false;
}

static void TouchUI_AddBuiltinPalette(void)
{
	int i, n = (int)(sizeof(touchui_builtin_palette) / sizeof(touchui_builtin_palette[0]));
	touchui_palette_count = 0;
	for (i = 0; i < n && touchui_palette_count < TOUCHUI_MAX_PALETTE; i++)
	{
		dp_strlcpy(touchui_palette[touchui_palette_count].label, touchui_builtin_palette[i].label, TOUCHUI_LABEL_LEN);
		dp_strlcpy(touchui_palette[touchui_palette_count].command, touchui_builtin_palette[i].cmd, TOUCHUI_CMD_LEN);
		touchui_palette_count++;
	}
}

/* Minimal quoted-token parser: "label" "command" */
static qbool TouchUI_ParseQuotedPair(const char *line, char *label, size_t labellen, char *cmd, size_t cmdlen)
{
	const char *p = line;
	const char *a, *b;
	size_t n;

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '#' || *p == '/' || *p == '\0' || *p == '\n' || *p == '\r')
		return false;
	if (*p != '"')
		return false;
	a = ++p;
	while (*p && *p != '"')
		p++;
	if (*p != '"')
		return false;
	n = (size_t)(p - a);
	if (n >= labellen)
		n = labellen - 1;
	memcpy(label, a, n);
	label[n] = 0;
	p++;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p != '"')
		return false;
	b = ++p;
	while (*p && *p != '"')
		p++;
	if (*p != '"')
		return false;
	n = (size_t)(p - b);
	if (n >= cmdlen)
		n = cmdlen - 1;
	memcpy(cmd, b, n);
	cmd[n] = 0;
	return label[0] != 0 && cmd[0] != 0;
}

void TouchUI_ReloadPalette(void)
{
	fs_offset_t filesize = 0;
	unsigned char *data;
	char *text, *line, *next;
	const char *path = touch_conui_palette_file.string;

	if (!path || !path[0])
		path = "touch/console_palette.txt";

	if (touchui_palette_loaded_path[0] && !strcmp(touchui_palette_loaded_path, path) && touchui_palette_count > 0)
		return;

	TouchUI_AddBuiltinPalette();
	data = FS_LoadFile(path, tempmempool, true, &filesize);
	if (!data || filesize <= 0)
	{
		dp_strlcpy(touchui_palette_loaded_path, path, sizeof(touchui_palette_loaded_path));
		return;
	}

	touchui_palette_count = 0;
	text = (char *)data;
	for (line = text; line && *line; line = next)
	{
		char label[TOUCHUI_LABEL_LEN];
		char cmd[TOUCHUI_CMD_LEN];
		next = strchr(line, '\n');
		if (next)
		{
			*next = 0;
			next++;
		}
		if (TouchUI_ParseQuotedPair(line, label, sizeof(label), cmd, sizeof(cmd)))
		{
			if (touchui_palette_count < TOUCHUI_MAX_PALETTE)
			{
				dp_strlcpy(touchui_palette[touchui_palette_count].label, label, TOUCHUI_LABEL_LEN);
				dp_strlcpy(touchui_palette[touchui_palette_count].command, cmd, TOUCHUI_CMD_LEN);
				touchui_palette_count++;
			}
		}
	}
	Mem_Free(data);
	if (touchui_palette_count == 0)
		TouchUI_AddBuiltinPalette();
	dp_strlcpy(touchui_palette_loaded_path, path, sizeof(touchui_palette_loaded_path));
}

static touchui_item_t *TouchUI_PushItem(touchui_item_t *out, int *n, int maxout,
	float x, float y, float w, float h, const char *label, float th,
	touchui_style_t style, touchui_action_t action, int arg)
{
	touchui_item_t *it;
	if (*n >= maxout)
		return NULL;
	it = &out[*n];
	(*n)++;
	memset(it, 0, sizeof(*it));
	it->x = x;
	it->y = y;
	it->w = w;
	it->h = h;
	it->textheight = th;
	it->style = style;
	it->action = action;
	it->action_arg = arg;
	if (label)
		dp_strlcpy(it->label, label, TOUCHUI_LABEL_LEN);
	return it;
}

static void TouchUI_WarnMinkey(float key_w, float key_h)
{
	float m = touch_kb_minkey_px.value;
	if (m <= 0 || touchui_minkey_warned)
		return;
	if (key_w < m || key_h < m)
	{
		Con_DPrintf("touch_ui: key size %.0fx%.0f below touch_kb_minkey_px %.0f\n", key_w, key_h, m);
		touchui_minkey_warned = true;
	}
}

static const char *TouchUI_LayerRows(touchui_layer_t layer, int row)
{
	static const char *abc[] = { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm./-" };
	static const char *shift[] = { "!@#$%^&*()", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM_:\"" };
	static const char *sym[] = { "1234567890", "-/:;()$&@\"", ".,?!'_+*=", "[]{}#%\\|^~" };
	if (row < 0 || row > 3)
		return "";
	if (layer == TOUCHUI_LAYER_SHIFT)
		return shift[row];
	if (layer == TOUCHUI_LAYER_SYM)
		return sym[row];
	return abc[row];
}

static int TouchUI_LayoutKeyboard(float x, float y, float w, float h,
	touchui_item_t *out, int maxout, int n)
{
	float gap = TouchUI_KeyGap(w);
	float row_h = h / (touchui_layout_menu ? 5.0f : 6.0f);
	float th = bound(12.0f, row_h * 0.40f, 26.0f);
	int r, c, len, i;
	float key_w, ky, kh, ax, aw;
	const char *row;
	char chbuf[2];
	qbool split = touch_kb_split.integer != 0 && w > h;
	float half_w, left_x, right_x, side_w;

	half_w = (w - gap * 3.0f) * 0.5f;
	left_x = x + gap;
	right_x = x + w * 0.5f + gap * 0.5f;
	side_w = half_w;

	for (r = 0; r < 4; r++)
	{
		row = TouchUI_LayerRows(touchui_layer, r);
		len = (int)strlen(row);
		ky = y + (float)r * row_h + gap;
		kh = row_h - gap * 2.0f;
		if (split && len > 2)
		{
			int mid = (len + 1) / 2;
			int side, start, count;
			float sx, sw;
			for (side = 0; side < 2; side++)
			{
				start = side ? mid : 0;
				count = side ? (len - mid) : mid;
				if (count <= 0)
					continue;
				sx = side ? right_x : left_x;
				sw = side_w;
				key_w = (sw - gap * (float)(count + 1)) / (float)count;
				TouchUI_WarnMinkey(key_w, kh);
				for (c = 0; c < count; c++)
				{
					chbuf[0] = row[start + c];
					chbuf[1] = 0;
					TouchUI_PushItem(out, &n, maxout,
						sx + gap + (float)c * (key_w + gap), ky, key_w, kh,
						chbuf, th, TOUCHUI_STYLE_GLASS, TOUCHUI_ACT_CHAR, (unsigned char)chbuf[0]);
				}
			}
		}
		else
		{
			key_w = (w - gap * (float)(len + 1)) / (float)len;
			TouchUI_WarnMinkey(key_w, kh);
			for (c = 0; c < len; c++)
			{
				chbuf[0] = row[c];
				chbuf[1] = 0;
				TouchUI_PushItem(out, &n, maxout,
					x + gap + (float)c * (key_w + gap), ky, key_w, kh,
					chbuf, th, TOUCHUI_STYLE_GLASS, TOUCHUI_ACT_CHAR, (unsigned char)chbuf[0]);
			}
		}
	}

	/* Menu text field: one row under the letters. A tap in the field
	 * places the cursor, so no arrows; DONE puts the keys away. */
	if (touchui_layout_menu)
	{
		struct { const char *lab; touchui_action_t act; int arg; float weight; touchui_style_t st; } acts[] = {
			{ "?123", TOUCHUI_ACT_LAYER_SYM, 0, 1.2f, TOUCHUI_STYLE_ACCENT },
			{ "SHIFT", TOUCHUI_ACT_LAYER_SHIFT, 0, 1.3f, TOUCHUI_STYLE_GLASS },
			{ "SPACE", TOUCHUI_ACT_CHAR, ' ', 3.6f, TOUCHUI_STYLE_GLASS },
			{ "BKSP", TOUCHUI_ACT_BKSP_HOLD, K_BACKSPACE, 1.4f, TOUCHUI_STYLE_GLASS },
			{ "DONE", TOUCHUI_ACT_DONE, 0, 1.3f, TOUCHUI_STYLE_DIM },
			{ "ENTER", TOUCHUI_ACT_KEY, K_ENTER, 1.5f, TOUCHUI_STYLE_ACCENT },
		};
		int nacts = (int)(sizeof(acts) / sizeof(acts[0]));
		float total = 0;
		if (touchui_layer == TOUCHUI_LAYER_SYM)
		{
			acts[0].lab = "ABC";
			acts[0].act = TOUCHUI_ACT_LAYER_ABC;
		}
		if (touchui_layer == TOUCHUI_LAYER_SHIFT)
		{
			acts[1].lab = "SHIFT*";
			acts[1].st = TOUCHUI_STYLE_ACCENT;
		}
		ky = y + 4.0f * row_h + gap;
		kh = row_h - gap * 2.0f;
		for (i = 0; i < nacts; i++)
			total += acts[i].weight;
		ax = x + gap;
		for (i = 0; i < nacts; i++)
		{
			aw = (w - gap * (float)(nacts + 1)) * (acts[i].weight / total);
			TouchUI_PushItem(out, &n, maxout, ax, ky, aw, kh,
				acts[i].lab, th * 0.9f, acts[i].st, acts[i].act, acts[i].arg);
			ax += aw + gap;
		}
		return n;
	}

	/* Modifier + action row */
	ky = y + 4.0f * row_h + gap;
	kh = row_h - gap * 2.0f;
	{
		struct { const char *lab; touchui_action_t act; int arg; touchui_style_t st; } mods[4];
		mods[0].lab = (touchui_layer == TOUCHUI_LAYER_SYM) ? "ABC" : "?123";
		mods[0].act = (touchui_layer == TOUCHUI_LAYER_SYM) ? TOUCHUI_ACT_LAYER_ABC : TOUCHUI_ACT_LAYER_SYM;
		mods[0].arg = 0;
		mods[0].st = TOUCHUI_STYLE_ACCENT;
		mods[1].lab = (touchui_layer == TOUCHUI_LAYER_SHIFT) ? "SHIFT*" : "SHIFT";
		mods[1].act = TOUCHUI_ACT_LAYER_SHIFT;
		mods[1].arg = 0;
		mods[1].st = (touchui_layer == TOUCHUI_LAYER_SHIFT) ? TOUCHUI_STYLE_ACCENT : TOUCHUI_STYLE_GLASS;
		mods[2].lab = "TAB";
		mods[2].act = TOUCHUI_ACT_KEY;
		mods[2].arg = K_TAB;
		mods[2].st = TOUCHUI_STYLE_GLASS;
		mods[3].lab = "BKSP";
		mods[3].act = TOUCHUI_ACT_BKSP_HOLD;
		mods[3].arg = K_BACKSPACE;
		mods[3].st = TOUCHUI_STYLE_GLASS;
		aw = (w - gap * 5.0f) / 4.0f;
		for (i = 0; i < 4; i++)
		{
			ax = x + gap + (float)i * (aw + gap);
			TouchUI_PushItem(out, &n, maxout, ax, ky, aw, kh,
				mods[i].lab, th, mods[i].st, mods[i].act, mods[i].arg);
		}
	}

	/* Bottom action row: hist, arrows, space, enter, scroll */
	ky = y + 5.0f * row_h + gap;
	kh = row_h - gap * 2.0f;
	{
		struct { const char *lab; touchui_action_t act; int arg; float weight; touchui_style_t st; } acts[] = {
			{ "HIST", TOUCHUI_ACT_KEY, K_UPARROW, 1.0f, TOUCHUI_STYLE_DIM },
			{ "NEXT", TOUCHUI_ACT_KEY, K_DOWNARROW, 1.0f, TOUCHUI_STYLE_DIM },
			{ "<", TOUCHUI_ACT_KEY, K_LEFTARROW, 0.8f, TOUCHUI_STYLE_DIM },
			{ ">", TOUCHUI_ACT_KEY, K_RIGHTARROW, 0.8f, TOUCHUI_STYLE_DIM },
			/* A character: K_SPACE carries none, and the console types only text. */
			{ "SPACE", TOUCHUI_ACT_CHAR, ' ', 2.4f, TOUCHUI_STYLE_GLASS },
			{ "PGUP", TOUCHUI_ACT_KEY, K_PGUP, 1.0f, TOUCHUI_STYLE_DIM },
			{ "PGDN", TOUCHUI_ACT_KEY, K_PGDN, 1.0f, TOUCHUI_STYLE_DIM },
			{ "ENTER", TOUCHUI_ACT_KEY, K_ENTER, 1.4f, TOUCHUI_STYLE_ACCENT },
		};
		int nacts = (int)(sizeof(acts) / sizeof(acts[0]));
		float total = 0;
		for (i = 0; i < nacts; i++)
			total += acts[i].weight;
		ax = x + gap;
		for (i = 0; i < nacts; i++)
		{
			aw = (w - gap * (float)(nacts + 1)) * (acts[i].weight / total);
			TouchUI_PushItem(out, &n, maxout, ax, ky, aw, kh,
				acts[i].lab, th * 0.9f, acts[i].st, acts[i].act, acts[i].arg);
			ax += aw + gap;
		}
	}
	return n;
}

/* A palette-file entry named like a spectator action on screen (the stock file
 * has "join" and "spectate") would show the same word twice, and "spectate"
 * would even mean two different things. The spectator action wins while shown. */
static qbool TouchUI_PaletteShadowed(int idx)
{
	int k;
	for (k = 0; k < touchui_spec_shown_count; k++)
		if (!strcasecmp(touchui_palette[idx].label, touchui_spec_entries[touchui_spec_shown[k]].label))
			return true;
	return false;
}

static int TouchUI_LayoutPalette(float x, float y, float w, float h,
	touchui_item_t *out, int maxout, int n)
{
	float gap = bound(2.0f, touch_kb_gap.value * w, 16.0f);
	int cols = (w > h) ? 4 : 3;
	int rows, i, col, row, slot, total;
	float cw, rh, th;

	TouchUI_ReloadPalette();
	TouchUI_UpdateSpectatorEntries();
	total = touchui_spec_shown_count;
	for (i = 0; i < touchui_palette_count; i++)
		if (!TouchUI_PaletteShadowed(i))
			total++;
	if (total <= 0)
		return n;
	rows = (total + cols - 1) / cols;
	if (rows < 1)
		rows = 1;
	cw = (w - gap * (float)(cols + 1)) / (float)cols;
	rh = (h - gap * (float)(rows + 1)) / (float)rows;
	th = bound(10.0f, rh * 0.35f, 22.0f);
	/* Spectator actions first, in accent, then the palette file. */
	for (slot = 0; slot < touchui_spec_shown_count; slot++)
	{
		col = slot % cols;
		row = slot / cols;
		TouchUI_PushItem(out, &n, maxout,
			x + gap + (float)col * (cw + gap),
			y + gap + (float)row * (rh + gap),
			cw, rh,
			touchui_spec_entries[touchui_spec_shown[slot]].label, th,
			TOUCHUI_STYLE_ACCENT, TOUCHUI_ACT_SPECTATE, slot);
	}
	for (i = 0; i < touchui_palette_count; i++)
	{
		if (TouchUI_PaletteShadowed(i))
			continue;
		col = slot % cols;
		row = slot / cols;
		TouchUI_PushItem(out, &n, maxout,
			x + gap + (float)col * (cw + gap),
			y + gap + (float)row * (rh + gap),
			cw, rh,
			touchui_palette[i].label, th,
			TOUCHUI_STYLE_GLASS, TOUCHUI_ACT_PALETTE, i);
		slot++;
	}
	return n;
}

static int TouchUI_LayoutHeader(float x, float y, float w, float h,
	touchui_sheet_t kind, touchui_item_t *out, int maxout, int n)
{
	float gap = bound(2.0f, touch_kb_gap.value * w, 12.0f);
	float th = bound(12.0f, h * 0.42f, 24.0f);
	float bw, bx;
	qbool keys = TouchUI_KeysShown(kind);
	qbool toggle = kind != TOUCHUI_SHEET_PREVIEW;
	int slots = ((kind == TOUCHUI_SHEET_CHAT) ? 2 : (keys ? 5 : 4)) + (toggle ? 1 : 0);
	const char *close_lab = (kind == TOUCHUI_SHEET_CHAT) ? "CLOSE CHAT" : "CLOSE";

	bw = (w - gap * (float)(slots + 1)) / (float)slots;
	bx = x + gap;
	TouchUI_PushItem(out, &n, maxout, bx, y + gap, bw, h - gap * 2.0f,
		close_lab, th, TOUCHUI_STYLE_DANGER, TOUCHUI_ACT_CLOSE, 0);
	bx += bw + gap;
	if (kind != TOUCHUI_SHEET_CHAT)
	{
		if (keys)
		{
			TouchUI_PushItem(out, &n, maxout, bx, y + gap, bw, h - gap * 2.0f,
				"KEYS", th, touchui_tab == TOUCHUI_TAB_KEYS ? TOUCHUI_STYLE_ACCENT : TOUCHUI_STYLE_GLASS,
				TOUCHUI_ACT_TAB_KEYS, 0);
			bx += bw + gap;
		}
		TouchUI_PushItem(out, &n, maxout, bx, y + gap, bw, h - gap * 2.0f,
			"COMMANDS", th, (touchui_tab == TOUCHUI_TAB_COMMANDS || !keys) ? TOUCHUI_STYLE_ACCENT : TOUCHUI_STYLE_GLASS,
			TOUCHUI_ACT_TAB_COMMANDS, 0);
		bx += bw + gap;
		TouchUI_PushItem(out, &n, maxout, bx, y + gap, bw, h - gap * 2.0f,
			"PGUP", th, TOUCHUI_STYLE_DIM, TOUCHUI_ACT_KEY, K_PGUP);
		bx += bw + gap;
		TouchUI_PushItem(out, &n, maxout, bx, y + gap, bw, h - gap * 2.0f,
			"PGDN", th, TOUCHUI_STYLE_DIM, TOUCHUI_ACT_KEY, K_PGDN);
		bx += bw + gap;
	}
	else
	{
		TouchUI_PushItem(out, &n, maxout, bx, y + gap, bw, h - gap * 2.0f,
			"SAY", th, TOUCHUI_STYLE_ACCENT, TOUCHUI_ACT_KEY, K_ENTER);
		bx += bw + gap;
	}
	if (toggle)
		TouchUI_PushItem(out, &n, maxout, bx, y + gap, bw, h - gap * 2.0f,
			keys ? "HIDE KEYS" : "KEYBOARD", th, TOUCHUI_STYLE_GLASS, TOUCHUI_ACT_KB_TOGGLE, 0);
	return n;
}

static int TouchUI_LayoutSampleLog(float x, float y, float w, float h,
	touchui_item_t *out, int maxout, int n)
{
	/* Stand-in scrollback so the preview body matches the real console sheet. */
	float line = bound(10.0f, h / 5.0f, 16.0f);
	char buf[TOUCHUI_LABEL_LEN];
	TouchUI_PushItem(out, &n, maxout, x + 4, y + 2, w - 8, line,
		"] status", line * 0.9f, TOUCHUI_STYLE_TEXT, TOUCHUI_ACT_NONE, 0);
	TouchUI_PushItem(out, &n, maxout, x + 4, y + 2 + line, w - 8, line,
		"host: listen server", line * 0.9f, TOUCHUI_STYLE_TEXT, TOUCHUI_ACT_NONE, 0);
	dpsnprintf(buf, sizeof(buf), "] kb_height %.2f", touch_kb_height.value);
	TouchUI_PushItem(out, &n, maxout, x + 4, y + 2 + line * 2.0f, w - 8, line,
		buf, line * 0.9f, TOUCHUI_STYLE_TEXT, TOUCHUI_ACT_NONE, 0);
	TouchUI_PushItem(out, &n, maxout, x + 4, y + 2 + line * 3.0f, w - 8, line,
		"] ", line * 0.9f, TOUCHUI_STYLE_TEXT, TOUCHUI_ACT_NONE, 0);
	(void)h;
	return n;
}

/* Draws in the console and chat fonts as itself, not as a missing-glyph box. */
static qbool TouchUI_GlyphDraws(Uchar ch)
{
	if (!utf8_enable.integer)
		return false;
	if (!FONT_CONSOLE->ft2 || !Font_HasGlyph(FONT_CONSOLE->ft2, ch))
		return false;
	if (!FONT_CHAT->ft2 || !Font_HasGlyph(FONT_CHAT->ft2, ch))
		return false;
	return true;
}

/* The key's character, then its variants the fonts can draw. 0 if none. */
static int TouchUI_Variants(int base, Uchar *out, int maxout)
{
	int i, n = 0;
	const char *p;
	Uchar c;

	for (i = 0; i < TOUCHUI_VARIANT_COUNT; i++)
	{
		if (touchui_variant_table[i].base != base)
			continue;
		out[n++] = (Uchar)base;
		for (p = touchui_variant_table[i].variants; *p && n < maxout; )
		{
			c = u8_getchar_utf8_enabled(p, &p);
			if (!c)
				break;
			if (TouchUI_GlyphDraws(c))
				out[n++] = c;
		}
		break;
	}
	return n > 1 ? n : 0;
}

static void TouchUI_OpenPopup(int finger)
{
	touchui_popup_t *pp = &touchui_popup;
	float minx = touchui_sheet_x + 2.0f, maxx = touchui_sheet_x + touchui_sheet_w - 2.0f;
	float kx = touchui_press[finger].x, kw = touchui_press[finger].w;
	float start;
	int i, n = TouchUI_Variants(touchui_press[finger].ch, pp->ch, TOUCHUI_POPUP_MAX);

	if (!n)
		return;
	pp->count = n;
	/* Taller than the key, as on a phone: the row is read under a thumb. */
	pp->chh = max(touchui_press[finger].h * 1.6f, 32.0f);
	pp->cw = max(kw, pp->chh * 0.9f);
	pp->key_y = touchui_press[finger].y;
	pp->key_h = touchui_press[finger].h;
	pp->y = max(touchui_sheet_y, pp->key_y - pp->chh - pp->key_h * 0.15f);
	/* The key's own character sits over the key, the others run right of
	 * it, or left of it near the right edge. */
	start = kx + (kw - pp->cw) * 0.5f;
	if (start + (float)n * pp->cw <= maxx)
		for (i = 0; i < n; i++)
			pp->cx[i] = start + (float)i * pp->cw;
	else if (start + pp->cw - (float)n * pp->cw >= minx)
		for (i = 0; i < n; i++)
			pp->cx[i] = start - (float)i * pp->cw;
	else
	{
		start = max(minx, maxx - (float)n * pp->cw);
		for (i = 0; i < n; i++)
			pp->cx[i] = start + (float)i * pp->cw;
	}
	pp->sel = 0;
	pp->finger = finger;
	pp->open = true;
}

static void TouchUI_PopupSpan(float *lo, float *hi)
{
	int i;
	*lo = touchui_popup.cx[0];
	*hi = touchui_popup.cx[0];
	for (i = 1; i < touchui_popup.count; i++)
	{
		*lo = min(*lo, touchui_popup.cx[i]);
		*hi = max(*hi, touchui_popup.cx[i]);
	}
	*hi += touchui_popup.cw;
}

static qbool TouchUI_InPopup(float fx, float fy)
{
	float lo, hi;
	TouchUI_PopupSpan(&lo, &hi);
	return fx >= lo && fx < hi && fy >= touchui_popup.y && fy < touchui_popup.y + touchui_popup.chh;
}

/* Sliding picks the nearest cell, from the row down to the held key; well
 * away from that band nothing is picked. */
static void TouchUI_PopupSlide(float fx, float fy)
{
	touchui_popup_t *pp = &touchui_popup;
	float lo, hi, d, best = 1e9f;
	int i;

	TouchUI_PopupSpan(&lo, &hi);
	if (fy < pp->y - pp->chh * 0.75f || fy > pp->key_y + pp->key_h * 1.5f
		|| fx < lo - pp->cw * 0.5f || fx >= hi + pp->cw * 0.5f)
	{
		pp->sel = -1;
		return;
	}
	for (i = 0; i < pp->count; i++)
	{
		d = fabs(fx - (pp->cx[i] + pp->cw * 0.5f));
		if (d < best)
		{
			best = d;
			pp->sel = i;
		}
	}
}

static int TouchUI_LayoutPopup(touchui_item_t *out, int maxout, int n)
{
	touchui_popup_t *pp = &touchui_popup;
	char lab[8];
	int i, len;

	if (!pp->open)
		return n;
	for (i = 0; i < pp->count; i++)
	{
		len = u8_fromchar(pp->ch[i], lab, sizeof(lab) - 1);
		lab[len > 0 ? len : 0] = 0;
		TouchUI_PushItem(out, &n, maxout, pp->cx[i], pp->y, pp->cw, pp->chh,
			lab, bound(14.0f, pp->chh * 0.6f, 36.0f),
			i == pp->sel ? TOUCHUI_STYLE_POPUP_SEL : TOUCHUI_STYLE_POPUP, TOUCHUI_ACT_POPUP, i);
	}
	return n;
}

/*
 * Keyboard and mouse (touch controls off): the classic console, with a small
 * KEYBOARD toggle in the corner that brings up the same keys, closed until
 * the player opens them.
 */
static int TouchUI_LayoutMouseSheet(touchui_sheet_t kind, float x, float y, float w, float h,
	touchui_item_t *out, int maxout)
{
	int n = 0;
	qbool keys = TouchUI_KeysShown(kind);
	float bh = bound(28.0f, h * 0.06f, 44.0f);
	float bw = bh * 4.0f;
	float kb_h = TouchUI_KeysHeight(w, h, 0, bound(0.28f, touch_kb_height.value, 0.70f));

	float by;

	if (keys)
		n = TouchUI_LayoutKeyboard(x, y + h - kb_h, w, kb_h, out, maxout, n);
	if (kind == TOUCHUI_SHEET_CHAT)
		touchui_chat_keys_top = keys ? y + h - kb_h : -1;
	if (kind == TOUCHUI_SHEET_CONSOLE && keys)
	{
		touchui_console_keys_top = y + h - kb_h;
		touchui_console_keys_time = host.realtime;
	}
	/* Hung under the right end of the console, or of the chat line at the top,
	 * and never over the keys. */
	by = y + 8.0f;
	if (kind == TOUCHUI_SHEET_CONSOLE)
		by = y + (float)min(scr_con_current, (unsigned int)h) + 6.0f;
	if (keys)
		by = min(by, y + h - kb_h - bh - 6.0f);
	by = bound(y, by, y + h - bh);
	TouchUI_PushItem(out, &n, maxout, x + w - bw - 8.0f, by, bw, bh,
		keys ? "HIDE KEYS" : "KEYBOARD", bh * 0.42f, TOUCHUI_STYLE_GLASS, TOUCHUI_ACT_KB_TOGGLE, 0);
	return TouchUI_LayoutPopup(out, maxout, n);
}

/* The menu's keys along the bottom: five rows of finger-sized keys, about
 * 42% of the screen, never over 55%. */
static int TouchUI_LayoutMenuKeys(float x, float y, float w, float h,
	touchui_item_t *out, int maxout)
{
	float dp = TouchUI_ConPerDp();
	float gap = TouchUI_KeyGap(w);
	float px2con = vid.mode.height > 0 ? vid_conheight.value / (float)vid.mode.height : 1.0f;
	float key = max(TOUCHUI_MIN_KEY_DP * dp, touch_kb_minkey_px.value * px2con);
	float kb_h = min(max(5.0f * (key + gap * 2.0f), h * 0.42f), h * 0.55f);
	float top;
	int n;
	n = 0;
	/* An opaque plate first, under the keys: drawn after the menu, so the
	 * menu does not show between and through them. */
	TouchUI_PushItem(out, &n, maxout, x, y + h - kb_h, w, kb_h, " ", 1.0f, TOUCHUI_STYLE_PLATE, TOUCHUI_ACT_NONE, 0);
	touchui_layout_menu = true;
	n = TouchUI_LayoutKeyboard(x, y + h - kb_h, w, kb_h, out, maxout, n);
	touchui_layout_menu = false;
	touchui_menu_top = y + h - kb_h;
	top = vid_conheight.value > 0 ? touchui_menu_top / vid_conheight.value : 0;
	if (fabs(touch_menu_keys_top.value - top) > 0.0005f)
		Cvar_SetValueQuick(&touch_menu_keys_top, top);
	return TouchUI_LayoutPopup(out, maxout, n);
}

int TouchUI_LayoutSheet(touchui_sheet_t kind, float x, float y, float w, float h,
	touchui_item_t *out, int maxout)
{
	int n = 0;
	float hdr_h, kb_h, kb_frac, body_top, body_h, log_h;
	qbool keys = TouchUI_KeysShown(kind);

	if (!out || maxout <= 0 || w < 32.0f || h < 32.0f)
		return 0;
	touchui_sheet_x = x;
	touchui_sheet_y = y;
	touchui_sheet_w = w;
	touchui_sheet_h = h;
	touchui_processing_menu = kind == TOUCHUI_SHEET_MENU;
	if (kind == TOUCHUI_SHEET_MENU)
		return TouchUI_LayoutMenuKeys(x, y, w, h, out, maxout);
	if (!vid_touchscreen.integer && kind != TOUCHUI_SHEET_PREVIEW)
		return TouchUI_LayoutMouseSheet(kind, x, y, w, h, out, maxout);

	kb_frac = bound(0.28f, touch_kb_height.value, 0.70f);
	hdr_h = TouchUI_SheetHeaderHeight(h);
	kb_h = TouchUI_KeysHeight(w, h, hdr_h, kb_frac);
	if (kb_h < 80.0f)
		kb_h = min(h * 0.5f, 80.0f);
	/* Chat with a keyboard attached: just the quick phrases, at the bottom. */
	if (kind == TOUCHUI_SHEET_CHAT && !keys)
		kb_h = 0;
	body_top = y + hdr_h;
	body_h = h - hdr_h - kb_h;
	if (body_h < 0)
		body_h = 0;

	n = TouchUI_LayoutHeader(x, y, w, hdr_h, kind, out, maxout, n);

	if (kind == TOUCHUI_SHEET_PREVIEW && body_h > 8.0f)
	{
		log_h = body_h * 0.7f;
		n = TouchUI_LayoutSampleLog(x, body_top, w, log_h, out, maxout, n);
	}

	if (kind == TOUCHUI_SHEET_CONSOLE)
	{
		touchui_console_keys_top = y + h - kb_h;
		touchui_console_keys_time = host.realtime;
	}
	if (kind == TOUCHUI_SHEET_CONSOLE || kind == TOUCHUI_SHEET_PREVIEW)
	{
		if (touchui_tab == TOUCHUI_TAB_COMMANDS || !keys)
			n = TouchUI_LayoutPalette(x, y + h - kb_h, w, kb_h, out, maxout, n);
		else
			n = TouchUI_LayoutKeyboard(x, y + h - kb_h, w, kb_h, out, maxout, n);
	}
	else /* chat */
	{
		touchui_chat_keys_top = keys ? y + h - kb_h : -1;
		/* Quick phrases strip above keyboard */
		if (body_h > 24.0f)
		{
			float strip_h = min(body_h, bound(28.0f, h * 0.10f, 48.0f));
			int i, cols = min(4, touchui_palette_count);
			float gap = bound(2.0f, touch_kb_gap.value * w, 12.0f);
			float cw, th;
			TouchUI_ReloadPalette();
			cols = min(4, touchui_palette_count);
			if (cols > 0)
			{
				cw = (w - gap * (float)(cols + 1)) / (float)cols;
				th = bound(10.0f, strip_h * 0.4f, 18.0f);
				for (i = 0; i < cols; i++)
				{
					/* Prefer say phrases near the end of the builtin list */
					int idx = touchui_palette_count - cols + i;
					if (idx < 0)
						idx = i;
					TouchUI_PushItem(out, &n, maxout,
						x + gap + (float)i * (cw + gap),
						y + h - kb_h - strip_h + gap * 0.5f,
						cw, strip_h - gap,
						touchui_palette[idx].label, th,
						TOUCHUI_STYLE_ACCENT, TOUCHUI_ACT_PALETTE, idx);
				}
				if (keys)
					touchui_chat_keys_top = y + h - kb_h - strip_h;
			}
		}
		if (keys)
			n = TouchUI_LayoutKeyboard(x, y + h - kb_h, w, kb_h, out, maxout, n);
	}
	return TouchUI_LayoutPopup(out, maxout, n);
}

static void TouchUI_EmitKey(int key)
{
	Key_Event(key, 0, true);
	Key_Event(key, 0, false);
}

static void TouchUI_EmitText(Uchar ch)
{
	Key_Event(K_TEXT, ch, true);
	Key_Event(K_TEXT, ch, false);
}

/* Released: the picked character replaces the one typed on the press. */
static void TouchUI_ClosePopup(void)
{
	touchui_popup_t *pp = &touchui_popup;
	pp->open = false;
	if (pp->sel == 0)
		return;
	Key_Event(K_BACKSPACE, 0, true);
	Key_Event(K_BACKSPACE, 0, false);
	if (pp->sel > 0 && pp->sel < pp->count)
		TouchUI_EmitText(pp->ch[pp->sel]);
}

static void TouchUI_EmitChar(int ch)
{
	Key_Event(K_TEXT, (unsigned int)(unsigned char)ch, true);
	Key_Event(K_TEXT, (unsigned int)(unsigned char)ch, false);
	/* One-shot shift: return to abc after a shifted character */
	if (touchui_layer == TOUCHUI_LAYER_SHIFT)
		touchui_layer = TOUCHUI_LAYER_ABC;
}

static void TouchUI_FireAction(const touchui_item_t *it, double realtime)
{
	char vabuf[256];
	switch (it->action)
	{
	case TOUCHUI_ACT_CHAR:
		TouchUI_EmitChar(it->action_arg);
		break;
	case TOUCHUI_ACT_KEY:
		TouchUI_EmitKey(it->action_arg);
		break;
	case TOUCHUI_ACT_BKSP_HOLD:
		TouchUI_EmitKey(K_BACKSPACE);
		touchui_bksp_held = true;
		touchui_bksp_next = realtime + 0.40;
		break;
	case TOUCHUI_ACT_CLOSE:
		TouchUI_EmitKey(K_ESCAPE);
		TouchUI_ResetInputState();
		break;
	case TOUCHUI_ACT_TAB_KEYS:
		touchui_tab = TOUCHUI_TAB_KEYS;
		break;
	case TOUCHUI_ACT_DONE:
		touchui_menu_dismissed = true;
		touchui_popup.open = false;
		break;
	case TOUCHUI_ACT_KB_TOGGLE:
		TouchUI_ToggleKeys();
		if (TouchUI_KeysShown(TOUCHUI_SHEET_CONSOLE))
			touchui_tab = TOUCHUI_TAB_KEYS;
		break;
	case TOUCHUI_ACT_TAB_COMMANDS:
		touchui_tab = TOUCHUI_TAB_COMMANDS;
		TouchUI_ReloadPalette();
		break;
	case TOUCHUI_ACT_LAYER_SHIFT:
		if (touchui_layer == TOUCHUI_LAYER_SHIFT)
			touchui_layer = TOUCHUI_LAYER_ABC;
		else
			touchui_layer = TOUCHUI_LAYER_SHIFT;
		break;
	case TOUCHUI_ACT_LAYER_SYM:
		touchui_layer = TOUCHUI_LAYER_SYM;
		break;
	case TOUCHUI_ACT_LAYER_ABC:
		touchui_layer = TOUCHUI_LAYER_ABC;
		break;
	case TOUCHUI_ACT_PALETTE:
		if (it->action_arg >= 0 && it->action_arg < touchui_palette_count)
		{
			dpsnprintf(vabuf, sizeof(vabuf), "%s\n", touchui_palette[it->action_arg].command);
			Cbuf_AddText(cmd_local, vabuf);
		}
		break;
	case TOUCHUI_ACT_SPECTATE:
		if (it->action_arg >= 0 && it->action_arg < touchui_spec_shown_count)
		{
			dpsnprintf(vabuf, sizeof(vabuf), "%s\n", touchui_spec_entries[touchui_spec_shown[it->action_arg]].command);
			Cbuf_AddText(cmd_local, vabuf);
			/* The sheet covers the view these actions change; close it so the
			 * player sees the next player, the camera or the join. */
			TouchUI_EmitKey(K_ESCAPE);
			TouchUI_ResetInputState();
		}
		break;
	default:
		break;
	}
}

qbool TouchUI_ProcessFinger(int finger, float fx, float fy, qbool finger_down,
	const touchui_item_t *items, int nitems, double realtime)
{
	int k, hit = -1;
	const touchui_item_t *it;
	unsigned int bit;

	if (finger < 0 || finger >= TOUCHUI_MAX_FINGERS)
		return false;
	bit = 1u << finger;

	/* The menu's keys take only fingers that land on them: one that pressed
	 * the menu and slides down over them types nothing. */
	if (touchui_processing_menu)
	{
		if (!finger_down)
			touchui_menu_seen &= ~bit, touchui_menu_foreign &= ~bit;
		else
		{
			if (!(touchui_menu_seen & bit))
			{
				touchui_menu_seen |= bit;
				if (fy < touchui_menu_top)
					touchui_menu_foreign |= bit;
			}
			if (touchui_menu_foreign & bit)
				return false;
		}
	}

	if (!finger_down)
	{
		if (touchui_popup.open && touchui_popup.finger == finger)
			TouchUI_ClosePopup();
		touchui_press[finger].armed = false;
		touchui_typed_mask &= ~bit;
		if (finger == touchui_bksp_finger)
		{
			touchui_bksp_held = false;
			touchui_bksp_finger = -1;
		}
		return false;
	}

	if (touchui_popup.open)
	{
		if (touchui_popup.finger == finger)
		{
			TouchUI_PopupSlide(fx, fy);
			return true;
		}
		/* The row covers keys; another finger on it presses none of them. */
		if (TouchUI_InPopup(fx, fy))
			return true;
	}

	/* Held on the character it typed: open its variants. A tap typed on the
	 * press already, so the timer never delays one. */
	if ((touchui_typed_mask & bit) && touchui_press[finger].armed)
	{
		float hold = touch_kb_hold_time.value;
		if (hold > 0 && realtime - touchui_press[finger].time >= hold)
		{
			touchui_press[finger].armed = false;
			if (!touchui_popup.open
				&& fx >= touchui_press[finger].x && fx < touchui_press[finger].x + touchui_press[finger].w
				&& fy >= touchui_press[finger].y && fy < touchui_press[finger].y + touchui_press[finger].h)
				TouchUI_OpenPopup(finger);
			if (touchui_popup.open)
				return true;
		}
	}

	for (k = 0; k < nitems; k++)
	{
		it = &items[k];
		if (it->action == TOUCHUI_ACT_NONE || it->action == TOUCHUI_ACT_POPUP)
			continue;
		if (it->w <= 0 || it->h <= 0)
			continue;
		if (fx >= it->x && fy >= it->y && fx < it->x + it->w && fy < it->y + it->h)
		{
			hit = k;
			break;
		}
	}

	/* Hold-repeat backspace */
	if (touchui_bksp_held && finger == touchui_bksp_finger && hit >= 0 && items[hit].action == TOUCHUI_ACT_BKSP_HOLD)
	{
		if (realtime >= touchui_bksp_next)
		{
			TouchUI_EmitKey(K_BACKSPACE);
			touchui_bksp_next = realtime + 0.07;
		}
		return true;
	}

	if (hit < 0)
		return false;

	if ((touchui_typed_mask & bit) != 0)
		return true;

	touchui_typed_mask |= bit;
	it = &items[hit];
	touchui_press[finger].armed = it->action == TOUCHUI_ACT_CHAR;
	touchui_press[finger].time = realtime;
	touchui_press[finger].ch = it->action_arg;
	touchui_press[finger].x = it->x;
	touchui_press[finger].y = it->y;
	touchui_press[finger].w = it->w;
	touchui_press[finger].h = it->h;
	if (it->action == TOUCHUI_ACT_BKSP_HOLD)
		touchui_bksp_finger = finger;
	TouchUI_FireAction(it, realtime);
	return true;
}

void TouchUI_DrawGlassItem(const touchui_item_t *it, float pressed, float opacity)
{
	float r, g, b, a, th;
	float tw;
	if (!it || it->w < 1.0f || it->h < 1.0f)
		return;

	a = opacity * (it->style == TOUCHUI_STYLE_TEXT ? 0.0f : 1.0f);
	r = TOUCHUI_GLASS_R * 0.35f;
	g = TOUCHUI_GLASS_G * 0.40f;
	b = TOUCHUI_GLASS_B * 0.50f;

	if (it->style == TOUCHUI_STYLE_ACCENT)
	{
		r = TOUCHUI_ACCENT_R * 0.45f;
		g = TOUCHUI_ACCENT_G * 0.55f;
		b = TOUCHUI_ACCENT_B * 0.70f;
	}
	else if (it->style == TOUCHUI_STYLE_DANGER)
	{
		r = TOUCHUI_DANGER_R * 0.55f;
		g = TOUCHUI_DANGER_G * 0.25f;
		b = TOUCHUI_DANGER_B * 0.20f;
	}
	else if (it->style == TOUCHUI_STYLE_DIM)
	{
		r = 0.12f;
		g = 0.16f;
		b = 0.22f;
	}

	if (it->style == TOUCHUI_STYLE_PLATE)
	{
		/* Behind the menu's keys: the menu under them does not show through. */
		DrawQ_Fill(it->x, it->y, it->w, it->h, 0.02f, 0.04f, 0.08f, 1.0f, 0);
		DrawQ_Fill(it->x, it->y, it->w, 1, TOUCHUI_GLASS_R, TOUCHUI_GLASS_G, TOUCHUI_GLASS_B, 0.30f, 0);
		return;
	}
	if (it->style == TOUCHUI_STYLE_POPUP || it->style == TOUCHUI_STYLE_POPUP_SEL)
	{
		/* Opaque, so the keys it covers do not show through. */
		qbool sel = it->style == TOUCHUI_STYLE_POPUP_SEL;
		DrawQ_Fill(it->x, it->y, it->w, it->h, 0.05f, 0.07f, 0.10f, 1.0f, 0);
		if (sel)
			DrawQ_Fill(it->x + 2, it->y + 2, it->w - 4, it->h - 4,
				TOUCHUI_ACCENT_R * 0.55f, TOUCHUI_ACCENT_G * 0.65f, TOUCHUI_ACCENT_B * 0.85f, 1.0f, 0);
		else
			DrawQ_Fill(it->x + 1, it->y + 1, it->w - 2, it->h - 2, 0.16f, 0.20f, 0.27f, 1.0f, 0);
	}
	else if (it->style != TOUCHUI_STYLE_TEXT)
	{
		DrawQ_Fill(it->x, it->y, it->w, it->h,
			r + 0.10f * pressed, g + 0.12f * pressed, b + 0.14f * pressed,
			0.55f * a + 0.30f * pressed * a, 0);
		/* Rim */
		DrawQ_Fill(it->x + 1, it->y + 1, it->w - 2, 1,
			TOUCHUI_GLASS_R, TOUCHUI_GLASS_G, TOUCHUI_GLASS_B, 0.35f * a, 0);
		DrawQ_Fill(it->x + 2, it->y + 2, it->w - 4, it->h - 4,
			r * 0.7f, g * 0.75f, b * 0.85f, 0.40f * a + 0.20f * pressed * a, 0);
	}

	if (it->label[0])
	{
		th = it->textheight > 0 ? it->textheight : bound(10.0f, it->h * 0.4f, 22.0f);
		tw = DrawQ_TextWidth(it->label, 0, th, th, false, FONT_CHAT);
		DrawQ_String(it->x + (it->w - tw) * 0.5f, it->y + (it->h - th) * 0.5f,
			it->label, 0, th, th, 1, 1, 1, 1, 0, NULL, false, FONT_CHAT);
	}
}
