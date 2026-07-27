/*
 * Touch text-sheet: keyboard layers, command palette, console/chat/preview layout.
 */
#include "quakedef.h"
#include "touch_ui.h"
#include "draw.h"

cvar_t touch_kb_height = {CF_CLIENT | CF_ARCHIVE, "touch_kb_height", "0.46", "fraction of sheet height used by the touch keyboard"};
cvar_t touch_kb_gap = {CF_CLIENT | CF_ARCHIVE, "touch_kb_gap", "0.008", "gap between keys as a fraction of sheet width"};
cvar_t touch_kb_opacity = {CF_CLIENT | CF_ARCHIVE, "touch_kb_opacity", "0.92", "opacity of touch keyboard glass plates"};
cvar_t touch_kb_layout = {CF_CLIENT | CF_ARCHIVE, "touch_kb_layout", "0", "0 = qwerty, 1 = compact"};
cvar_t touch_kb_split = {CF_CLIENT | CF_ARCHIVE, "touch_kb_split", "0", "1 = split keyboard under both thumbs in landscape"};
cvar_t touch_kb_minkey_px = {CF_CLIENT | CF_ARCHIVE, "touch_kb_minkey_px", "36", "warn when a key's smaller dimension is below this (device pixels)"};
cvar_t touch_conui_shade = {CF_CLIENT | CF_ARCHIVE, "touch_conui_shade", "0.62", "console sheet background shade alpha"};
cvar_t touch_conui_palette_file = {CF_CLIENT | CF_ARCHIVE, "touch_conui_palette_file", "touch/console_palette.txt", "file of preset console commands for the COMMANDS tab"};
cvar_t _touch_kb_preview = {CF_CLIENT, "_touch_kb_preview", "0", "internal: menu preview active this frame"};
cvar_t _touch_kb_preview_x = {CF_CLIENT, "_touch_kb_preview_x", "0", "internal: preview cell x"};
cvar_t _touch_kb_preview_y = {CF_CLIENT, "_touch_kb_preview_y", "0", "internal: preview cell y"};
cvar_t _touch_kb_preview_w = {CF_CLIENT, "_touch_kb_preview_w", "0", "internal: preview cell w"};
cvar_t _touch_kb_preview_h = {CF_CLIENT, "_touch_kb_preview_h", "0", "internal: preview cell h"};

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
	Cvar_RegisterVariable(&touch_conui_shade);
	Cvar_RegisterVariable(&touch_conui_palette_file);
	Cvar_RegisterVariable(&_touch_kb_preview);
	Cvar_RegisterVariable(&_touch_kb_preview_x);
	Cvar_RegisterVariable(&_touch_kb_preview_y);
	Cvar_RegisterVariable(&_touch_kb_preview_w);
	Cvar_RegisterVariable(&_touch_kb_preview_h);
}

void TouchUI_Init(void)
{
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
	float gap = bound(2.0f, touch_kb_gap.value * w, 16.0f);
	float row_h = h / 6.0f;
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
			{ "SPACE", TOUCHUI_ACT_KEY, K_SPACE, 2.4f, TOUCHUI_STYLE_GLASS },
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

static int TouchUI_LayoutPalette(float x, float y, float w, float h,
	touchui_item_t *out, int maxout, int n)
{
	float gap = bound(2.0f, touch_kb_gap.value * w, 16.0f);
	int cols = (w > h) ? 4 : 3;
	int rows, i, col, row;
	float cw, rh, th;

	TouchUI_ReloadPalette();
	if (touchui_palette_count <= 0)
		return n;
	rows = (touchui_palette_count + cols - 1) / cols;
	if (rows < 1)
		rows = 1;
	cw = (w - gap * (float)(cols + 1)) / (float)cols;
	rh = (h - gap * (float)(rows + 1)) / (float)rows;
	th = bound(10.0f, rh * 0.35f, 22.0f);
	for (i = 0; i < touchui_palette_count; i++)
	{
		col = i % cols;
		row = i / cols;
		TouchUI_PushItem(out, &n, maxout,
			x + gap + (float)col * (cw + gap),
			y + gap + (float)row * (rh + gap),
			cw, rh,
			touchui_palette[i].label, th,
			TOUCHUI_STYLE_GLASS, TOUCHUI_ACT_PALETTE, i);
	}
	return n;
}

static int TouchUI_LayoutHeader(float x, float y, float w, float h,
	touchui_sheet_t kind, touchui_item_t *out, int maxout, int n)
{
	float gap = bound(2.0f, touch_kb_gap.value * w, 12.0f);
	float th = bound(12.0f, h * 0.42f, 24.0f);
	float bw, bx;
	int slots = (kind == TOUCHUI_SHEET_CHAT) ? 2 : 5;
	const char *close_lab = (kind == TOUCHUI_SHEET_CHAT) ? "CLOSE CHAT" : "CLOSE";

	bw = (w - gap * (float)(slots + 1)) / (float)slots;
	bx = x + gap;
	TouchUI_PushItem(out, &n, maxout, bx, y + gap, bw, h - gap * 2.0f,
		close_lab, th, TOUCHUI_STYLE_DANGER, TOUCHUI_ACT_CLOSE, 0);
	bx += bw + gap;
	if (kind != TOUCHUI_SHEET_CHAT)
	{
		TouchUI_PushItem(out, &n, maxout, bx, y + gap, bw, h - gap * 2.0f,
			"KEYS", th, touchui_tab == TOUCHUI_TAB_KEYS ? TOUCHUI_STYLE_ACCENT : TOUCHUI_STYLE_GLASS,
			TOUCHUI_ACT_TAB_KEYS, 0);
		bx += bw + gap;
		TouchUI_PushItem(out, &n, maxout, bx, y + gap, bw, h - gap * 2.0f,
			"COMMANDS", th, touchui_tab == TOUCHUI_TAB_COMMANDS ? TOUCHUI_STYLE_ACCENT : TOUCHUI_STYLE_GLASS,
			TOUCHUI_ACT_TAB_COMMANDS, 0);
		bx += bw + gap;
		TouchUI_PushItem(out, &n, maxout, bx, y + gap, bw, h - gap * 2.0f,
			"PGUP", th, TOUCHUI_STYLE_DIM, TOUCHUI_ACT_KEY, K_PGUP);
		bx += bw + gap;
		TouchUI_PushItem(out, &n, maxout, bx, y + gap, bw, h - gap * 2.0f,
			"PGDN", th, TOUCHUI_STYLE_DIM, TOUCHUI_ACT_KEY, K_PGDN);
	}
	else
	{
		TouchUI_PushItem(out, &n, maxout, bx, y + gap, bw, h - gap * 2.0f,
			"SAY", th, TOUCHUI_STYLE_ACCENT, TOUCHUI_ACT_KEY, K_ENTER);
	}
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

int TouchUI_LayoutSheet(touchui_sheet_t kind, float x, float y, float w, float h,
	touchui_item_t *out, int maxout)
{
	int n = 0;
	float hdr_h, kb_h, kb_frac, body_top, body_h, log_h;

	if (!out || maxout <= 0 || w < 32.0f || h < 32.0f)
		return 0;

	kb_frac = bound(0.28f, touch_kb_height.value, 0.70f);
	hdr_h = bound(36.0f, h * 0.10f, 64.0f);
	kb_h = h * kb_frac;
	if (kb_h < 80.0f)
		kb_h = min(h * 0.5f, 80.0f);
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

	if (kind == TOUCHUI_SHEET_CONSOLE || kind == TOUCHUI_SHEET_PREVIEW)
	{
		if (touchui_tab == TOUCHUI_TAB_COMMANDS)
			n = TouchUI_LayoutPalette(x, y + h - kb_h, w, kb_h, out, maxout, n);
		else
			n = TouchUI_LayoutKeyboard(x, y + h - kb_h, w, kb_h, out, maxout, n);
	}
	else /* chat */
	{
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
			}
		}
		n = TouchUI_LayoutKeyboard(x, y + h - kb_h, w, kb_h, out, maxout, n);
	}
	return n;
}

static void TouchUI_EmitKey(int key)
{
	Key_Event(key, 0, true);
	Key_Event(key, 0, false);
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
	default:
		break;
	}
}

qbool TouchUI_ProcessFinger(int finger, float fx, float fy, qbool finger_down,
	const touchui_item_t *items, int nitems, double realtime)
{
	int k, hit = -1;
	const touchui_item_t *it;

	if (finger < 0 || finger >= TOUCHUI_MAX_FINGERS)
		return false;

	if (!finger_down)
	{
		touchui_typed_mask &= ~(1u << finger);
		if (finger == 0)
			touchui_bksp_held = false;
		return false;
	}

	for (k = 0; k < nitems; k++)
	{
		it = &items[k];
		if (it->action == TOUCHUI_ACT_NONE)
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
	if (touchui_bksp_held && hit >= 0 && items[hit].action == TOUCHUI_ACT_BKSP_HOLD)
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

	if ((touchui_typed_mask & (1u << finger)) != 0)
		return true;

	touchui_typed_mask |= (1u << finger);
	TouchUI_FireAction(&items[hit], realtime);
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

	if (it->style != TOUCHUI_STYLE_TEXT)
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

void TouchUI_DrawPreview(float cell_x, float cell_y, float cell_w, float cell_h)
{
	float screen_w = vid_conwidth.value;
	float screen_h = vid_conheight.value;
	float aspect, fit_w, fit_h, ox, oy;
	float shade;
	touchui_item_t items[TOUCHUI_MAX_ITEMS];
	int n, i;
	float opacity;

	if (cell_w < 8.0f || cell_h < 8.0f || screen_w < 1.0f || screen_h < 1.0f)
		return;

	aspect = screen_w / screen_h;
	if (cell_w / cell_h > aspect)
	{
		fit_h = cell_h;
		fit_w = fit_h * aspect;
	}
	else
	{
		fit_w = cell_w;
		fit_h = fit_w / aspect;
	}
	ox = cell_x + (cell_w - fit_w) * 0.5f;
	oy = cell_y + (cell_h - fit_h) * 0.5f;

	shade = TouchUI_GetShadeAlpha();
	DrawQ_Fill(ox, oy, fit_w, fit_h, 0.02f, 0.04f, 0.08f, shade, 0);

	n = TouchUI_LayoutSheet(TOUCHUI_SHEET_PREVIEW, ox, oy, fit_w, fit_h, items, TOUCHUI_MAX_ITEMS);
	opacity = TouchUI_GetKeyOpacity();
	for (i = 0; i < n; i++)
		TouchUI_DrawGlassItem(&items[i], 0.0f, opacity);
}
