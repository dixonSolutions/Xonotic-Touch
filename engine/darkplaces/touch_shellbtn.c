/*
Godnotic: the console button (see touch_shellbtn.h).
*/

#include "quakedef.h"
#include "touch_shellbtn.h"

extern cvar_t vid_conwidth;
extern cvar_t vid_conheight;

static cvar_t touch_shell_button = {CF_CLIENT | CF_ARCHIVE, "touch_shell_button", "2",
	"console button in the top right corner: 0 never, 1 menus and console only, 2 also in a match"};
static cvar_t touch_shell_button_size = {CF_CLIENT | CF_ARCHIVE, "touch_shell_button_size", "48",
	"console button size in density-independent pixels (dp)"};
static cvar_t touch_shell_chat_sheet = {CF_CLIENT, "_touch_chat_sheet", "0",
	"set by the CSQC while its chat sheet is open (touch_chat.qc): the button would sit on the sheet's TEAM button"};
static cvar_t touch_shell_safe_insets = {CF_CLIENT, "_touch_safe_insets", "",
	"set by the platform: \"left top right bottom density\" in window pixels, the area cutouts and rounded corners keep clear"};

static qbool shell_cvars_ready;
static int held_id;          // finger on the button, finger table id; 0 none
static qbool mouse_held;     // a real mouse pressed on it
static qbool synth_held;     // SDL's made-up click for a finger on it
static qbool drawn;
static float bx, by, bs;     // last drawn rect, virtual (con) units

static void ensure_cvars(void)
{
	if (shell_cvars_ready)
		return;
	shell_cvars_ready = true;
	Cvar_RegisterVariable(&touch_shell_button);
	Cvar_RegisterVariable(&touch_shell_button_size);
	Cvar_RegisterVariable(&touch_shell_safe_insets);
	Cvar_RegisterVariable(&touch_shell_chat_sheet);
}

static float cv(const char *name, float def)
{
	cvar_t *var = Cvar_FindVar(&cvars_all, name, ~0);
	return var ? var->value : def;
}

static qbool console_open(void)
{
	return (key_consoleactive & KEY_CONSOLEACTIVE_USER) != 0;
}

static qbool in_match(void)
{
	return cls.state == ca_connected && cls.signon == SIGNONS && key_dest == key_game;
}

static qbool visible(void)
{
	int mode;
	ensure_cvars();
	mode = touch_shell_button.integer;
	if (mode <= 0 || scr_loading)
		return false;
	// Joining: the loading screen shows until the world is in.
	if (cls.state == ca_connected && cls.signon < SIGNONS && !console_open())
		return false;
	if (key_dest == key_menu_grabbed)
		return false; // binding a key takes every press as it is
	if (console_open())
		return true;
	if (cls.state != ca_connected && touch_shell_chat_sheet.integer)
		Cvar_SetValueQuick(&touch_shell_chat_sheet, 0); // a sheet left open by a CSQC that went away
	if (key_dest == key_game)
	{
		if (cls.state != ca_connected)
			return false; // nothing on screen yet (startup, between maps)
		if (mode < 2)
			return false;
		// The CSQC chat sheet has its own CLOSE; the button covered TEAM.
		if (touch_shell_chat_sheet.integer)
			return false;
		// A keyboard-and-mouse player has the mouse grabbed and the ` key.
		return vid_touchscreen.integer || VID_SDL_HasTouchDevices() || vid_touchscreen_detected.integer;
	}
	return true; // menus, chat
}

typedef struct { float x, y, w, h; } srect_t;

static qbool overlap(const srect_t *a, const srect_t *b)
{
	return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

/* The top bar of the touch HUD (touch_hud.c / the CSQC): CHAT and, while a
 * vote is open, the vote chip beside it (touch_vote.c chip_place). */
static int topbar_rects(srect_t *out)
{
	float W = vid_conwidth.value, H = vid_conheight.value, s = min(W, H);
	float scale = max(0.01f, cv("touch_scale", 1.0f));
	qbool flip = cv("touch_handedness", 0) != 0;
	float mx = cv("touch_pause_x", 0.680f), chx_n = cv("touch_chat_x", 0.862f);
	float hh = s * cv("touch_chat_size", 0.060f) * scale * 0.5f;
	float hw = hh * max(1.0f, cv("touch_chat_aspect", 2.9f));
	float cy = cv("touch_chat_y", 0.088f) * H, cx, r, margin, vx;
	int n = 0;
	if (flip)
	{
		mx = 1 - mx;
		chx_n = 1 - chx_n;
	}
	cx = chx_n * W;
	if (cv("touch_chat_visible", 1))
	{
		out[n].x = cx - hw; out[n].y = cy - hh; out[n].w = hw * 2; out[n].h = hh * 2;
		n++;
	}
	r = max(0.024f * s, hh);
	margin = r * 0.6f;
	vx = cx + (chx_n >= mx ? 1 : -1) * (hw + margin + r);
	vx = bound(r + margin * 0.5f, vx, W - r - margin * 0.5f);
	out[n].x = vx - r; out[n].y = cy - r; out[n].w = r * 2; out[n].h = r * 2;
	n++;
	return n;
}

static void place(void)
{
	float W = vid_conwidth.value, H = vid_conheight.value;
	float px2con = vid.mode.width > 0 ? W / (float)vid.mode.width : 1.0f;
	float in_l = 0, in_t = 0, in_r = 0, in_b = 0, density = 0, margin;
	srect_t me;

	if (touch_shell_safe_insets.string[0])
		sscanf(touch_shell_safe_insets.string, "%f %f %f %f %f", &in_l, &in_t, &in_r, &in_b, &density);
	if (density <= 0)
	{
		// No platform figure: a phone-sized guess from the shorter side
		// (a 1080 px tall phone is about 2.6x), never below 1 on a desktop.
		float shortside = (float)min(vid.mode.width, vid.mode.height);
		density = shortside >= 900 ? shortside / 420.0f : 1.0f;
		density = bound(1.0f, density, 4.0f);
	}
	bs = bound(32.0f, touch_shell_button_size.value, 96.0f) * density * px2con;
	margin = 8.0f * density * px2con;
	bx = W - in_r * px2con - margin - bs;
	by = in_t * px2con + margin;
	(void)in_b;

	if (console_open() && vid_touchscreen.integer)
	{
		// Under the touch console sheet's header (TouchUI_LayoutSheet).
		float hdr = bound(36.0f, H * 0.10f, 64.0f);
		by = max(by, hdr + margin);
	}
	else if (in_match() && vid_touchscreen.integer && cv("touch_pause_visible", 1))
	{
		// In a match the corner belongs to the score list and the info
		// messages, and the top bar has MENU, CHAT and the vote chip: the
		// button joins the bar on MENU's inner side (away from CHAT), the
		// one place nothing else ever takes.
		float s = min(W, H), scale = max(0.01f, cv("touch_scale", 1.0f));
		float mx = cv("touch_pause_x", 0.680f), chx_n = cv("touch_chat_x", 0.862f);
		float hh = s * cv("touch_pause_size", 0.060f) * scale * 0.5f;
		float hw = hh * max(1.0f, cv("touch_pause_aspect", 2.9f));
		float my = cv("touch_pause_y", 0.088f) * H;
		if (cv("touch_handedness", 0) != 0)
		{
			mx = 1 - mx;
			chx_n = 1 - chx_n;
		}
		mx *= W;
		bx = chx_n * W >= mx ? mx - hw - margin * 2 - bs : mx + hw + margin * 2;
		by = my - bs * 0.5f;
		bx = bound(in_l * px2con + margin, bx, W - in_r * px2con - margin - bs);
		by = max(by, in_t * px2con + margin * 0.5f);
	}
	else if (in_match())
	{
		srect_t bar[2];
		int i, n = topbar_rects(bar);
		me.x = bx; me.y = by; me.w = bs; me.h = bs;
		for (i = 0; i < n; i++)
			if (overlap(&me, &bar[i]))
			{
				by = bar[i].y + bar[i].h + margin;
				me.y = by;
			}
	}
}

void TouchShell_Draw(void)
{
	float cx, cy, r, a, ink, stroke, pressed;
	qbool open;
	drawn = false;
	if (!visible())
	{
		held_id = 0;
		mouse_held = false;
		return;
	}
	place();
	drawn = true;
	open = console_open();
	pressed = (held_id || mouse_held) ? 1.0f : 0.0f;
	a = bound(0.3f, cv("touch_opacity", 0.90f), 1.0f);
	r = bs * 0.5f;
	cx = bx + r;
	cy = by + r;
	stroke = max(1.0f, bs / 24.0f);

	// Plate: translucent dark glass, accent while the console is open.
	DrawQ_RoundedBox(cx, cy + r * 0.05f, r, r, r * 0.30f, 0, 0, 0, 0, 0.30f * a, 0);
	if (open || pressed > 0)
		DrawQ_RoundedBox(cx, cy, r * 0.94f, r * 0.94f, r * 0.28f, 0, 0.208f, 0.518f, 0.894f, (open ? 0.80f : 0.65f) * a, 0);
	else
		DrawQ_RoundedBox(cx, cy, r * 0.94f, r * 0.94f, r * 0.28f, 0, 0.07f, 0.08f, 0.10f, 0.58f * a, 0);
	DrawQ_RoundedBox(cx, cy, r * 0.94f, r * 0.94f, r * 0.28f, stroke, 1, 1, 1, 0.35f * a, 0);

	// The terminal glyph: a prompt chevron and a cursor bar.
	ink = 0.95f * a;
	{
		float gx = cx - r * 0.42f, gy = cy - r * 0.30f, gw = r * 0.34f, gh = r * 0.30f;
		float lw = max(2.0f, bs / 11.0f);
		// Both strokes left to right: DrawQ_Line's quad winds by direction,
		// and the UI mesh culls the other winding.
		DrawQ_Line(lw, gx, gy, gx + gw, gy + gh, 1, 1, 1, ink, 0);
		DrawQ_Line(lw, gx, gy + gh * 2, gx + gw, gy + gh, 1, 1, 1, ink, 0);
		DrawQ_Fill(cx + r * 0.02f, cy + r * 0.24f, r * 0.40f, lw, 1, 1, 1, ink, 0);
	}
}

static qbool hit(float vx, float vy)
{
	float grow = bs * 0.20f;
	// In the menu nothing grows above the button: full-height dialogs
	// (Multiplayer, Credits) have their close X just over it, in the corner.
	float top = (key_dest == key_menu && !console_open()) ? by : by - grow;
	return drawn && vx >= bx - grow && vx <= bx + bs + grow && vy >= top && vy <= by + bs + grow;
}

static void toggle(void)
{
	static double last;
	// One press can arrive both as a finger and as a mouse click (SDL makes
	// each from the other on some platforms): one toggle.
	if (host.realtime - last < 0.3)
		return;
	last = host.realtime;
	Con_DPrintf("Console button: toggleconsole\n");
	Cbuf_AddText(cmd_local, "toggleconsole\n");
}

qbool TouchShell_Finger(int type, int id, float nx, float ny)
{
	float vx = nx * vid_conwidth.value, vy = ny * vid_conheight.value;
	if (type == 0)
	{
		if (held_id || !hit(vx, vy))
			return false;
		held_id = id;
		return true;
	}
	if (id != held_id)
		return false;
	if (type == 2)
	{
		held_id = 0;
		if (hit(vx, vy))
			toggle();
	}
	return true;
}

qbool TouchShell_Mouse(qbool down, int button, qbool synthetic, float wx, float wy)
{
	float vx, vy;
	if (button != 1 || vid.mode.width <= 0 || vid.mode.height <= 0)
		return false;
	vx = wx / (float)vid.mode.width * vid_conwidth.value;
	vy = wy / (float)vid.mode.height * vid_conheight.value;
	if (synthetic)
	{
		if (down && (held_id || hit(vx, vy)))
		{
			synth_held = true;
			return true;
		}
		if (!down && synth_held)
		{
			synth_held = false;
			return true;
		}
		return false;
	}
	if (down)
	{
		if (!hit(vx, vy))
			return false;
		mouse_held = true;
		return true;
	}
	if (!mouse_held)
		return false;
	mouse_held = false;
	if (hit(vx, vy))
		toggle();
	return true;
}
