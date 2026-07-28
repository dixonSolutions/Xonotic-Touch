/*
 * Shared touch text-sheet layout for console and chat.
 * Layout takes a target rect so callers can place the sheet in any region.
 */
#ifndef TOUCH_UI_H
#define TOUCH_UI_H

#include "qtypes.h"

#define TOUCHUI_MAX_ITEMS       96
#define TOUCHUI_MAX_PALETTE     48
#define TOUCHUI_LABEL_LEN       24
#define TOUCHUI_CMD_LEN         128

typedef enum touchui_sheet_e
{
	TOUCHUI_SHEET_CONSOLE = 0,
	TOUCHUI_SHEET_CHAT,
	TOUCHUI_SHEET_PREVIEW
}
touchui_sheet_t;

typedef enum touchui_style_e
{
	TOUCHUI_STYLE_GLASS = 0,
	TOUCHUI_STYLE_ACCENT,
	TOUCHUI_STYLE_DANGER,
	TOUCHUI_STYLE_DIM,
	TOUCHUI_STYLE_TEXT
}
touchui_style_t;

typedef enum touchui_action_e
{
	TOUCHUI_ACT_NONE = 0,
	TOUCHUI_ACT_CHAR,          /* action_arg = unicode char */
	TOUCHUI_ACT_KEY,           /* action_arg = keynum */
	TOUCHUI_ACT_CLOSE,
	TOUCHUI_ACT_TAB_KEYS,
	TOUCHUI_ACT_TAB_COMMANDS,
	TOUCHUI_ACT_LAYER_SHIFT,
	TOUCHUI_ACT_LAYER_SYM,
	TOUCHUI_ACT_LAYER_ABC,
	TOUCHUI_ACT_PALETTE,       /* action_arg = palette index */
	TOUCHUI_ACT_BKSP_HOLD      /* backspace with hold-repeat */
}
touchui_action_t;

typedef enum touchui_tab_e
{
	TOUCHUI_TAB_KEYS = 0,
	TOUCHUI_TAB_COMMANDS
}
touchui_tab_t;

typedef enum touchui_layer_e
{
	TOUCHUI_LAYER_ABC = 0,
	TOUCHUI_LAYER_SHIFT,
	TOUCHUI_LAYER_SYM
}
touchui_layer_t;

typedef struct touchui_item_s
{
	float x, y, w, h;
	float textheight;
	touchui_style_t style;
	touchui_action_t action;
	int action_arg;
	char label[TOUCHUI_LABEL_LEN];
}
touchui_item_t;

typedef struct touchui_palette_entry_s
{
	char label[TOUCHUI_LABEL_LEN];
	char command[TOUCHUI_CMD_LEN];
}
touchui_palette_entry_t;

/* Cvars (archived) */
extern struct cvar_s touch_kb_height;
extern struct cvar_s touch_kb_gap;
extern struct cvar_s touch_kb_opacity;
extern struct cvar_s touch_kb_layout;
extern struct cvar_s touch_kb_split;
extern struct cvar_s touch_kb_minkey_px;
extern struct cvar_s touch_conui_shade;
extern struct cvar_s touch_conui_palette_file;

void TouchUI_Init(void);
void TouchUI_RegisterCvars(void);
void TouchUI_Shutdown(void);

/* Reload palette from touch_conui_palette_file (or built-in defaults). */
void TouchUI_ReloadPalette(void);

/*
 * Fill out[] with items for a sheet drawn inside (x,y,w,h).
 * Returns item count. Does not emit input or register touch areas.
 */
int TouchUI_LayoutSheet(touchui_sheet_t kind, float x, float y, float w, float h,
	touchui_item_t *out, int maxout);

/* Current interactive state */
touchui_tab_t TouchUI_GetTab(void);
touchui_layer_t TouchUI_GetLayer(void);
float TouchUI_GetShadeAlpha(void);
float TouchUI_GetKeyOpacity(void);

/*
 * Process one finger interaction against a laid-out sheet.
 * finger_down: true while finger is held.
 * Returns true if the finger hit an interactive item.
 * Emits Key_Event / Cbuf as appropriate. Handles backspace hold-repeat.
 */
qbool TouchUI_ProcessFinger(int finger, float fx, float fy, qbool finger_down,
	const touchui_item_t *items, int nitems, double realtime);

/* Reset per-finger emit masks (call when sheet closes). */
void TouchUI_ResetInputState(void);

/* Glass colour constants matching CSQC touch_defs.qh */
#define TOUCHUI_GLASS_R   0.78f
#define TOUCHUI_GLASS_G   0.86f
#define TOUCHUI_GLASS_B   0.96f
#define TOUCHUI_ACCENT_R  0.35f
#define TOUCHUI_ACCENT_G  0.78f
#define TOUCHUI_ACCENT_B  1.00f
#define TOUCHUI_DANGER_R  1.00f
#define TOUCHUI_DANGER_G  0.32f
#define TOUCHUI_DANGER_B  0.22f

void TouchUI_DrawGlassItem(const touchui_item_t *it, float pressed, float opacity);

#endif
