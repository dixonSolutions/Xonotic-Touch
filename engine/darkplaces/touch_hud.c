/*
Xonotic Touch: engine-side copy of the Touch HUD (see touch_hud.h).

Everything here mirrors client/touch_*.qc in xonotic-data.pk3dir:
  touch_layout.qc   -> TouchHUD_Center / Radius / Half / zones
  touch_shape.qc    -> the pic-based disc / ring / shadow / capsule primitives
  touch_widget.qc   -> Button / Capsule / ChromePill (shadow, surface, rim, ink)
  touch_draw.qc     -> which controls draw, and the move stick
  touch_input.qc    -> sticky finger roles, stick thresholds, hop latch
  touch_look.qc     -> drag look with the one-euro filter
The cvar names and defaults are the CSQC's own, so a profile or a saved layout
applies to both.
*/

#include "quakedef.h"
#include "cl_screen.h"
#include "csprogs.h"
#include "touch_hud.h"

extern cvar_t vid_touchscreen;
extern cvar_t vid_conwidth;
extern cvar_t vid_conheight;
extern cvar_t cl_forwardspeed;
extern cvar_t cl_sidespeed;
extern qbool cl_touch_csqc_active;

// vid_sdl.c's finger table: [i][0] = finger id + 1 (0 when up), [i][1..2] =
// position normalised to the window. The last slot is the mouse-as-finger
// used for desktop testing.
#define TOUCHHUD_ENGINE_FINGERS 11
extern float multitouch[TOUCHHUD_ENGINE_FINGERS][3];

// ---------------------------------------------------------------------------
// Theme (touch_theme.qh)
// ---------------------------------------------------------------------------
static const float C_SURFACE[3]      = {0.07f, 0.08f, 0.10f};
static const float C_RIM[3]          = {1, 1, 1};
static const float C_INK[3]          = {1, 1, 1};
static const float C_SHADOW[3]       = {0, 0, 0};
static const float C_ACCENT[3]       = {0.208f, 0.518f, 0.894f};
static const float C_ACCENT_LIGHT[3] = {0.506f, 0.816f, 1.0f};
static const float C_DANGER[3]       = {0.902f, 0.176f, 0.259f};
static const float C_SUCCESS[3]      = {0.227f, 0.580f, 0.290f};
static const float C_WARNING[3]      = {0.784f, 0.533f, 0.000f};

#define A_SURFACE        0.34f
#define A_SURFACE_ACTIVE 0.58f
#define A_SURFACE_LATCH  0.46f
#define A_RIM            0.62f
#define A_RIM_ACTIVE     0.95f
#define A_CONTOUR        0.50f
#define A_SHADOW         0.36f
#define A_INK            0.95f
#define A_GHOST          0.16f
#define A_GLOW           0.30f
#define A_CHROME_REST    0.55f
#define A_CHROME_FILL    0.88f
#define W_RING_THIN      0.035f
#define W_CONTOUR_INSET  0.030f
#define UI_REF_SHORT_AXIS 540.0f
#define ELEVATION_Y      0.045f
#define FONT_MIN         9.0f
#define FONT_MAX         20.0f
#define FONT_FIT_H       0.75f
#define FONT_FIT_W       0.70f
#define FONT_ADVANCE     0.55f
#define FONT_READOUT     24.0f
#define LOOK_DEG_PER_PX  0.18f
#define LOOK_REF_WIDTH   960.0f

// ---------------------------------------------------------------------------
// Cvars: the CSQC registers these (touch_init.qc); when only a server's CSQC
// has run this session they may not exist, so every read carries the default.
// ---------------------------------------------------------------------------
static float cv(const char *name, float def)
{
	cvar_t *var = Cvar_FindVar(&cvars_all, name, ~0);
	if (!var)
		return def;
	return var->value;
}

// ---------------------------------------------------------------------------
// Layout (touch_layout.qc)
// ---------------------------------------------------------------------------
static float ui_w(void) { return max(1.0f, vid_conwidth.value); }
static float ui_h(void) { return max(1.0f, vid_conheight.value); }
static float screen_min(void) { return min(ui_w(), ui_h()); }
static float widget_scale(void) { return max(0.01f, cv("touch_scale", 1.0f)); }
static float ui_scale(void) { return screen_min() / UI_REF_SHORT_AXIS; }
static float mirror_x(float nx) { return cv("touch_handedness", 0) ? 1.0f - nx : nx; }
static void widget_center(float nx, float ny, float *cx, float *cy)
{
	*cx = mirror_x(nx) * ui_w();
	*cy = ny * ui_h();
}
static float widget_radius(float nsize) { return screen_min() * nsize * 0.5f * widget_scale(); }
static void widget_half(float nsize, float aspect, float *hx, float *hy)
{
	float short_axis = screen_min() * nsize * widget_scale();
	float long_axis = short_axis * max(1.0f, aspect);
	*hx = long_axis * 0.5f;
	*hy = short_axis * 0.5f;
}
static qbool hit_circle(float px, float py, float nx, float ny, float nsize, float grab)
{
	float cx, cy, dx, dy, r;
	widget_center(nx, ny, &cx, &cy);
	r = widget_radius(nsize) * max(1.0f, grab);
	dx = px - cx; dy = py - cy;
	return dx * dx + dy * dy <= r * r;
}
static qbool hit_rect(float px, float py, float nx, float ny, float nsize, float aspect, float grab)
{
	float cx, cy, hx, hy;
	widget_center(nx, ny, &cx, &cy);
	widget_half(nsize, aspect, &hx, &hy);
	hx *= max(1.0f, grab); hy *= max(1.0f, grab);
	return px >= cx - hx && px <= cx + hx && py >= cy - hy && py <= cy + hy
		&& px >= 0 && py >= 0 && px <= ui_w() && py <= ui_h();
}
static qbool in_edge_deadzone(float px, float py)
{
	float dz = cv("touch_edge_deadzone_px", 10);
	return px < dz || py < dz || px > ui_w() - dz || py > ui_h() - dz;
}
static qbool in_move_zone(float px, float py)
{
	float w = cv("touch_move_zone_w", 0.32f);
	float left = 0, right;
	if (w <= 0)
		w = cv("touch_look_zone_left", 0.32f);
	right = w * ui_w();
	if (cv("touch_handedness", 0))
	{
		left = (1.0f - w) * ui_w();
		right = ui_w();
	}
	return px >= left && px <= right && py > ui_h() * 0.42f;
}
static qbool in_look_zone(float px, float py)
{
	float left = cv("touch_look_zone_left", 0.32f);
	float right = cv("touch_look_zone_right", 1.0f);
	if (cv("touch_handedness", 0))
	{
		float t = 1.0f - right;
		right = 1.0f - left;
		left = t;
	}
	return px >= left * ui_w() && px <= right * ui_w();
}

// ---------------------------------------------------------------------------
// Shapes (touch_shape.qc): everything is a tinted pic
// ---------------------------------------------------------------------------
static cachepic_t *pic_disc, *pic_disc_soft, *pic_ring_thin, *pic_shadow;
static qbool pics_ready, pics_probed;

static void shapes_probe(void)
{
	if (pics_probed)
		return;
	pics_probed = true;
	pic_disc = Draw_CachePic_Flags("gfx/touch/disc", CACHEPICFLAG_FAILONMISSING);
	pic_disc_soft = Draw_CachePic_Flags("gfx/touch/disc_soft", CACHEPICFLAG_FAILONMISSING);
	pic_ring_thin = Draw_CachePic_Flags("gfx/touch/ring_thin", CACHEPICFLAG_FAILONMISSING);
	pic_shadow = Draw_CachePic_Flags("gfx/touch/shadow", CACHEPICFLAG_FAILONMISSING);
	pics_ready = Draw_IsPicLoaded(pic_disc) && Draw_IsPicLoaded(pic_ring_thin)
		&& Draw_IsPicLoaded(pic_shadow) && Draw_IsPicLoaded(pic_disc_soft);
	if (!pics_ready)
		Con_Printf(CON_WARN "TouchHUD: gfx/touch shape masks missing, drawing flat\n");
}

static void shape_centered(cachepic_t *pic, float cx, float cy, float hx, float hy, const float *rgb, float alpha)
{
	if (hx < 0.5f || hy < 0.5f || alpha <= 0)
		return;
	if (Draw_IsPicLoaded(pic))
		DrawQ_Pic(cx - hx, cy - hy, pic, hx * 2, hy * 2, rgb[0], rgb[1], rgb[2], alpha, 0);
	else
		DrawQ_Fill(cx - hx, cy - hy, hx * 2, hy * 2, rgb[0], rgb[1], rgb[2], alpha, 0);
}
static void shape_disc(float cx, float cy, float r, const float *rgb, float a) { shape_centered(pic_disc, cx, cy, r, r, rgb, a); }
static void shape_glow(float cx, float cy, float r, const float *rgb, float a) { shape_centered(pic_disc_soft, cx, cy, r, r, rgb, a); }
static void shape_shadow(float cx, float cy, float r, float a) { shape_centered(pic_shadow, cx, cy, r, r, C_SHADOW, a); }
static void shape_ring(float cx, float cy, float r, const float *rgb, float a) { shape_centered(pic_ring_thin, cx, cy, r, r, rgb, a); }

// Left half / right half of a pic, for capsule caps (drawsubpic in QC).
static void shape_half_pic(cachepic_t *pic, float x, float y, float w, float h, float s0, float s1, const float *rgb, float a)
{
	if (w < 0.5f || h < 0.5f || a <= 0)
		return;
	if (!Draw_IsPicLoaded(pic))
	{
		DrawQ_Fill(x, y, w, h, rgb[0], rgb[1], rgb[2], a, 0);
		return;
	}
	DrawQ_SuperPic(x, y, pic, w, h,
		s0, 0, rgb[0], rgb[1], rgb[2], a,
		s1, 0, rgb[0], rgb[1], rgb[2], a,
		s0, 1, rgb[0], rgb[1], rgb[2], a,
		s1, 1, rgb[0], rgb[1], rgb[2], a, 0);
}

static void shape_capsule_from(cachepic_t *pic, float cx, float cy, float hx, float hy, const float *rgb, float a, qbool hollow, float stroke)
{
	float cap = hy, midw, t;
	if (hx < 0.5f || hy < 0.5f || a <= 0)
		return;
	if (hx <= cap)
	{
		shape_centered(pic, cx, cy, hy, hy, rgb, a);
		return;
	}
	shape_half_pic(pic, cx - hx, cy - cap, cap, cap * 2, 0.0f, 0.5f, rgb, a);
	shape_half_pic(pic, cx + hx - cap, cy - cap, cap, cap * 2, 0.5f, 1.0f, rgb, a);
	midw = (hx - cap) * 2;
	if (hollow)
	{
		t = max(1.0f, stroke * cap);
		DrawQ_Fill(cx - hx + cap, cy - cap, midw, t, rgb[0], rgb[1], rgb[2], a, 0);
		DrawQ_Fill(cx - hx + cap, cy + cap - t, midw, t, rgb[0], rgb[1], rgb[2], a, 0);
	}
	else
		DrawQ_Fill(cx - hx + cap, cy - cap, midw, cap * 2, rgb[0], rgb[1], rgb[2], a, 0);
}
static void shape_capsule(float cx, float cy, float hx, float hy, const float *rgb, float a) { shape_capsule_from(pic_disc, cx, cy, hx, hy, rgb, a, false, 0); }
static void shape_capsule_ring(float cx, float cy, float hx, float hy, const float *rgb, float a) { shape_capsule_from(pic_ring_thin, cx, cy, hx, hy, rgb, a, true, W_RING_THIN); }
static void shape_capsule_shadow(float cx, float cy, float hx, float hy, float a) { shape_capsule_from(pic_shadow, cx, cy, hx, hy, C_SHADOW, a, false, 0); }

static float label_width(float fs, const char *label)
{
	float w = DrawQ_TextWidth(label, 0, fs, fs, false, FONT_CHAT);
	if (w > 0)
		return w;
	return strlen(label) * fs * FONT_ADVANCE;
}
static void shape_label(float cx, float cy, float fs, const char *label, const float *rgb, float a)
{
	float w;
	if (!label || !label[0] || a <= 0 || fs < 1)
		return;
	w = label_width(fs, label);
	DrawQ_String(cx - w * 0.5f, cy - fs * 0.5f, label, 0, fs, fs, rgb[0], rgb[1], rgb[2], a, 0, NULL, false, FONT_CHAT);
}
static void shape_label_right(float rx, float cy, float fs, const char *label, const float *rgb, float a)
{
	float w;
	if (!label || !label[0] || a <= 0 || fs < 1)
		return;
	w = label_width(fs, label);
	DrawQ_String(rx - w, cy - fs * 0.5f, label, 0, fs, fs, rgb[0], rgb[1], rgb[2], a, 0, NULL, false, FONT_CHAT);
}

// ---------------------------------------------------------------------------
// Widgets (touch_widget.qc)
// ---------------------------------------------------------------------------
static float label_size(float hx, float hy, const char *label)
{
	float by_height = hy * FONT_FIT_H;
	float unit = max(0.01f, label_width(1.0f, label));
	float by_width = (hx * 2 * FONT_FIT_W) / unit;
	return bound(FONT_MIN, min(by_height, by_width), FONT_MAX);
}
static float elevation(float r) { return max(1.0f, r * ELEVATION_Y); }

static void widget_rim(float cx, float cy, float r, const float *rgb, float a)
{
	shape_ring(cx, cy, r, C_SHADOW, a * A_CONTOUR);
	shape_ring(cx, cy, r * (1 - W_CONTOUR_INSET), rgb, a);
}
static void widget_capsule_rim(float cx, float cy, float hx, float hy, const float *rgb, float a)
{
	shape_capsule_ring(cx, cy, hx, hy, C_SHADOW, a * A_CONTOUR);
	shape_capsule_ring(cx, cy, hx * (1 - W_CONTOUR_INSET), hy * (1 - W_CONTOUR_INSET), rgb, a);
}

static void widget_button(float cx, float cy, float r, const char *label, qbool pressed, float alpha, const float *accent)
{
	const float *surface = pressed ? accent : C_SURFACE;
	const float *rim = pressed ? accent : C_RIM;
	float a_surface = pressed ? A_SURFACE_ACTIVE : A_SURFACE;
	float a_rim = pressed ? A_RIM_ACTIVE : A_RIM;
	float fs;
	if (r < 2 || alpha <= 0)
		return;
	fs = label_size(r, r, label);
	shape_shadow(cx, cy + elevation(r), r * 1.12f, alpha * A_SHADOW);
	shape_disc(cx, cy, r, surface, alpha * a_surface);
	if (pressed)
		shape_glow(cx, cy, r * 1.35f, accent, alpha * A_GLOW);
	widget_rim(cx, cy, r, rim, alpha * a_rim);
	shape_label(cx, cy, fs, label, C_INK, alpha * A_INK);
}

static void widget_capsule(float cx, float cy, float hx, float hy, const char *label, qbool pressed, qbool latched, float alpha, const float *accent)
{
	qbool tinted = pressed || latched;
	const float *surface = tinted ? accent : C_SURFACE;
	const float *rim = tinted ? accent : C_RIM;
	float a_surface = A_SURFACE;
	float a_rim = tinted ? A_RIM_ACTIVE : A_RIM;
	float fs;
	if (hx < 2 || hy < 2 || alpha <= 0)
		return;
	if (pressed)
		a_surface = A_SURFACE_ACTIVE;
	else if (latched)
		a_surface = A_SURFACE_LATCH;
	fs = label_size(hx, hy, label);
	shape_capsule_shadow(cx, cy + elevation(hy), hx + hy * 0.10f, hy + hy * 0.10f, alpha * A_SHADOW);
	shape_capsule(cx, cy, hx, hy, surface, alpha * a_surface);
	if (pressed)
		shape_capsule(cx, cy, hx + hy * 0.18f, hy + hy * 0.18f, accent, alpha * A_GLOW);
	widget_capsule_rim(cx, cy, hx, hy, rim, alpha * a_rim);
	shape_label(cx, cy, fs, label, C_INK, alpha * A_INK);
	if (latched)
	{
		float pip_x = cx + (hx - hy);
		float pip_r = max(2.0f, hy * 0.26f);
		shape_disc(pip_x, cy, pip_r, C_INK, alpha * A_INK);
		shape_ring(pip_x, cy, pip_r * 1.5f, C_INK, alpha * A_RIM);
	}
}

static void widget_chrome_pill(float cx, float cy, float hx, float hy, const char *label, qbool pressed, float alpha, const float *accent)
{
	float a = pressed ? max(0.85f, alpha) : max(A_CHROME_REST, alpha * 0.8f);
	const float *surface = pressed ? accent : C_SURFACE;
	const float *rim = pressed ? accent : C_RIM;
	float fs;
	if (hx < 2 || hy < 2)
		return;
	fs = min(label_size(hx, hy, label), label_size(hx, hy, "CONSOLE"));
	shape_capsule(cx, cy, hx, hy, surface, a * A_CHROME_FILL);
	widget_capsule_rim(cx, cy, hx, hy, rim, a * A_RIM);
	shape_label(cx, cy, fs, label, C_INK, a * A_INK);
}

// ---------------------------------------------------------------------------
// Input state (touch_input.qc, touch_look.qc)
// ---------------------------------------------------------------------------
enum
{
	ROLE_NONE = -1, ROLE_IGNORED = -2,
	ROLE_MOVE = 0, ROLE_LOOK, ROLE_FIRE,
	ROLE_JUMP = 100, ROLE_CROUCH, ROLE_WEAPON, ROLE_CON = 106, ROLE_PAUSE, ROLE_CHAT = 109, ROLE_SCORES
};

#define MAX_FINGERS 4
typedef struct finger_s
{
	qbool used;
	int src;            // multitouch id (nonzero)
	int role;
	float down_x, down_y, last_x, last_y;
	double down_time;
	double lost_time;   // 0 while reported
	qbool first_frame;
	qbool fired;        // role-specific one-shot (pill tap, weapon swipe)
} finger_t;

static finger_t fingers[MAX_FINGERS];
static float move_off_x, move_off_y;
static qbool key_forward, key_back, key_left, key_right, key_attack, key_jump, key_crouch, key_scores;
static qbool hop_latched, hop_cancelling;
static double hop_press_time;
static double tapfire_until;
static qbool was_active;

// look
static float look_vel_x, look_vel_y, look_prev_raw_x, look_prev_raw_y, look_dvel_x, look_dvel_y;
static qbool look_active;
static double look_prev_time;

static void set_key(const char *name, qbool *state, qbool down)
{
	char buf[64];
	if (*state == down)
		return;
	*state = down;
	dpsnprintf(buf, sizeof(buf), "%s%s\n", down ? "+" : "-", name);
	Cbuf_AddText(cmd_local, buf);
}

void TouchHUD_ReleaseAll(void)
{
	int i;
	set_key("forward", &key_forward, false);
	set_key("back", &key_back, false);
	set_key("moveleft", &key_left, false);
	set_key("moveright", &key_right, false);
	set_key("attack", &key_attack, false);
	set_key("jump", &key_jump, false);
	set_key("crouch", &key_crouch, false);
	set_key("showscores", &key_scores, false);
	hop_latched = false;
	hop_cancelling = false;
	tapfire_until = 0;
	move_off_x = move_off_y = 0;
	look_active = false;
	look_vel_x = look_vel_y = 0;
	for (i = 0; i < MAX_FINGERS; i++)
		fingers[i].used = false;
}

qbool TouchHUD_Active(void)
{
	if (!vid_touchscreen.integer)
		return false;
	if (!CLVM_prog->loaded || cl_touch_csqc_active)
		return false;
	if (cls.state != ca_connected || cls.signon != SIGNONS)
		return false;
	if (key_dest != key_game || (key_consoleactive & KEY_CONSOLEACTIVE_USER))
		return false;
	return true;
}

static qbool role_owned(int role)
{
	int i;
	for (i = 0; i < MAX_FINGERS; i++)
		if (fingers[i].used && fingers[i].role == role)
			return true;
	return false;
}

static qbool weapon_button_shown(void)
{
	// The stock strip cannot be tapped without the port's CSQC, so the WEP
	// glass button stands in for it here whatever its cvar says.
	return true;
}

static int assign_role(float px, float py)
{
	if (cv("touch_con_visible", 1) && hit_rect(px, py, cv("touch_con_x", 0.090f), cv("touch_con_y", 0.948f), cv("touch_con_size", 0.060f), cv("touch_con_aspect", 2.9f), 1.20f))
		return ROLE_CON;
	if (cv("touch_pause_visible", 1) && hit_rect(px, py, cv("touch_pause_x", 0.680f), cv("touch_pause_y", 0.088f), cv("touch_pause_size", 0.060f), cv("touch_pause_aspect", 2.9f), 1.20f))
		return ROLE_PAUSE;
	if (cv("touch_chat_visible", 1) && hit_rect(px, py, cv("touch_chat_x", 0.862f), cv("touch_chat_y", 0.088f), cv("touch_chat_size", 0.060f), cv("touch_chat_aspect", 2.9f), 1.20f))
		return ROLE_CHAT;
	if (cv("touch_scores_visible", 1) && hit_rect(px, py, cv("touch_scores_x", 0.272f), cv("touch_scores_y", 0.948f), cv("touch_scores_size", 0.060f), cv("touch_scores_aspect", 2.9f), 1.20f))
		return ROLE_SCORES;
	if (in_edge_deadzone(px, py))
		return ROLE_IGNORED;
	if (cv("touch_fire_visible", 1) && hit_circle(px, py, cv("touch_fire_x", 0.855f), cv("touch_fire_y", 0.680f), cv("touch_fire_size", 0.140f), 1.15f))
		return ROLE_FIRE;
	if (cv("touch_jump_visible", 1) && hit_rect(px, py, cv("touch_jump_x", 0.855f), cv("touch_jump_y", 0.885f), cv("touch_jump_size", 0.072f), cv("touch_jump_aspect", 2.6f), 1.10f))
		return ROLE_JUMP;
	if (cv("touch_crouch_visible", 1) && hit_circle(px, py, cv("touch_crouch_x", 0.700f), cv("touch_crouch_y", 0.885f), cv("touch_crouch_size", 0.066f), 1.10f))
		return ROLE_CROUCH;
	if (weapon_button_shown() && hit_circle(px, py, cv("touch_weapon_x", 0.540f), cv("touch_weapon_y", 0.860f), cv("touch_weapon_size", 0.100f), 1.10f))
		return ROLE_WEAPON;
	if (in_move_zone(px, py) || (cv("touch_move_visible", 1) && hit_circle(px, py, cv("touch_move_x", 0.170f), cv("touch_move_y", 0.680f), cv("touch_move_size", 0.229f), 1.0f)))
	{
		if (role_owned(ROLE_MOVE))
			return ROLE_IGNORED;
		return ROLE_MOVE;
	}
	if (in_look_zone(px, py))
	{
		if (role_owned(ROLE_LOOK))
			return ROLE_IGNORED;
		return ROLE_LOOK;
	}
	if (role_owned(ROLE_LOOK))
		return ROLE_IGNORED;
	return ROLE_LOOK;
}

static float look_alpha(float cutoff, float dt)
{
	float tau = 1.0f / (2.0f * (float)M_PI * max(cutoff, 0.0001f));
	return 1.0f / (1.0f + tau / dt);
}

static void look_apply(float raw_x, float raw_y, float dt)
{
	float vrx, vry, a, cutoff, dvx, dvy, da, k, yaw, pitch, cap;
	dt = bound(1.0f / 240.0f, dt, 0.1f);
	if (sqrt(raw_x * raw_x + raw_y * raw_y) < cv("touch_look_deadzone_px", 4) * 0.25f)
		return;
	vrx = raw_x / dt;
	vry = raw_y / dt;
	if (cv("touch_look_filter", 1) == 0)
	{
		look_vel_x = vrx;
		look_vel_y = vry;
	}
	else if (cv("touch_look_filter", 1) == 2)
	{
		float tau = 0, s = cv("touch_look_smoothing", 1);
		if (s == 1) tau = 0.035f; else if (s >= 2) tau = 0.070f;
		a = 1.0f;
		if (tau > 0)
			a = 1.0f - expf(-dt / tau);
		look_vel_x += (vrx - look_vel_x) * a;
		look_vel_y += (vry - look_vel_y) * a;
	}
	else
	{
		// one-euro filter (touch_look.qc)
		dvx = (vrx - look_prev_raw_x) / dt;
		dvy = (vry - look_prev_raw_y) / dt;
		da = look_alpha(1.0f, dt);
		look_dvel_x += (dvx - look_dvel_x) * da;
		look_dvel_y += (dvy - look_dvel_y) * da;
		look_prev_raw_x = vrx;
		look_prev_raw_y = vry;
		cutoff = max(cv("touch_look_fcmin", 1.5f), 0.01f)
			+ max(cv("touch_look_beta", 0.03f), 0.0f) * sqrt(look_dvel_x * look_dvel_x + look_dvel_y * look_dvel_y);
		a = look_alpha(cutoff, dt);
		look_vel_x += (vrx - look_vel_x) * a;
		look_vel_y += (vry - look_vel_y) * a;
	}
	k = LOOK_DEG_PER_PX * cv("touch_sens_base", 2.8f) * (LOOK_REF_WIDTH / max((float)vid.mode.width, 1.0f));
	yaw = look_vel_x * dt * k;
	pitch = look_vel_y * dt * k * cv("touch_sens_y_mult", 0.85f);
	if (cv("touch_invert_y", 0))
		pitch = -pitch;
	cap = cv("touch_look_max_deg_per_s", 900) * dt;
	yaw = bound(-cap, yaw, cap);
	pitch = bound(-cap, pitch, cap);
	cl.viewangles[YAW] -= yaw;
	cl.viewangles[PITCH] += pitch;
	cl.viewangles[PITCH] = bound(-89, cl.viewangles[PITCH], 89);
	cl.viewangles[ROLL] = 0;
}

static void look_begin(void)
{
	look_active = true;
	look_vel_x = look_vel_y = 0;
	look_prev_raw_x = look_prev_raw_y = 0;
	look_dvel_x = look_dvel_y = 0;
	look_prev_time = host.realtime;
}

static void weapon_gesture(finger_t *f, qbool up)
{
	// touch_weapon_mode 2: tap = next weapon, swipe up/down = prev/next.
	float dy = f->last_y - f->down_y;
	float dx = f->last_x - f->down_x;
	float swipe = max(20.0f, widget_radius(cv("touch_weapon_size", 0.100f)) * 0.6f);
	if (f->fired)
		return;
	if (fabs(dy) > swipe && fabs(dy) > fabs(dx))
	{
		Cbuf_AddText(cmd_local, dy < 0 ? "weapprev\n" : "weapnext\n");
		f->fired = true;
		return;
	}
	if (up && host.realtime - f->down_time < 0.35 && fabs(dx) < swipe && fabs(dy) < swipe)
	{
		Cbuf_AddText(cmd_local, "weapnext\n");
		f->fired = true;
	}
}

static void pill_release(finger_t *f)
{
	// Chrome pills act on release, like the CSQC's tap-with-slop rule.
	float slop = 10 + cv("touch_pause_slop_px", 10);
	float dx = f->last_x - f->down_x, dy = f->last_y - f->down_y;
	if (fabs(dx) > slop || fabs(dy) > slop)
		return;
	switch (f->role)
	{
	case ROLE_CON:
		Cbuf_AddText(cmd_local, "toggleconsole\n");
		break;
	case ROLE_CHAT:
		Cbuf_AddText(cmd_local, "messagemode\n");
		break;
	case ROLE_PAUSE:
		TouchHUD_ReleaseAll();
		if (Cvar_VariableValue(&cvars_all, "menu_gamemenu", 0) && !cls.demoplayback
			&& Cvar_VariableValue(&cvars_all, "_menu_gamemenu_dialog_available", 0))
			Cbuf_AddText(cmd_local, "\nmenu_showgamemenudialog\n");
		else
			Cbuf_AddText(cmd_local, "\ntogglemenu 1\n");
		break;
	default:
		break;
	}
}

void TouchHUD_Frame(void)
{
	int i, j;
	double now = host.realtime;
	float dt;
	float grace = cv("touch_finger_grace_ms", 120) * 0.001f;
	qbool fire_count = false, jump_down = false, crouch_down = false, scores_down = false, move_active = false, look_seen = false;
	float look_dx = 0, look_dy = 0;
	qbool look_first = false;

	if (!TouchHUD_Active())
	{
		if (was_active)
			TouchHUD_ReleaseAll();
		was_active = false;
		return;
	}
	was_active = true;
	shapes_probe();

	dt = bound(1.0f / 240.0f, (float)(now - look_prev_time), 0.1f);

	// 1. match live contacts to the finger table by id; claim new ones
	for (i = 0; i < TOUCHHUD_ENGINE_FINGERS; i++)
	{
		int id = (int)multitouch[i][0];
		float px, py;
		finger_t *f = NULL;
		if (!id)
			continue;
		px = multitouch[i][1] * ui_w();
		py = multitouch[i][2] * ui_h();
		for (j = 0; j < MAX_FINGERS; j++)
			if (fingers[j].used && fingers[j].src == id)
				f = &fingers[j];
		if (!f)
		{
			for (j = 0; j < MAX_FINGERS; j++)
				if (!fingers[j].used)
				{
					f = &fingers[j];
					break;
				}
			if (!f)
				continue;
			memset(f, 0, sizeof(*f));
			f->used = true;
			f->src = id;
			f->down_x = f->last_x = px;
			f->down_y = f->last_y = py;
			f->down_time = now;
			f->first_frame = true;
			f->role = assign_role(px, py);
			if (f->role == ROLE_LOOK)
				look_begin();
			if (f->role == ROLE_JUMP)
				hop_press_time = now;
		}
		else
		{
			if (f->role == ROLE_LOOK && !f->first_frame)
			{
				look_dx += px - f->last_x;
				look_dy += py - f->last_y;
			}
			f->last_x = px;
			f->last_y = py;
			f->first_frame = false;
		}
		f->lost_time = 0;
	}

	// 2. contacts that vanished: keep them for the grace period, then release
	for (j = 0; j < MAX_FINGERS; j++)
	{
		finger_t *f = &fingers[j];
		qbool seen = false;
		if (!f->used)
			continue;
		for (i = 0; i < TOUCHHUD_ENGINE_FINGERS; i++)
			if ((int)multitouch[i][0] == f->src)
				seen = true;
		if (seen)
			continue;
		if (!f->lost_time)
			f->lost_time = now;
		if (now - f->lost_time < grace)
			continue;
		// released
		if (f->role == ROLE_CON || f->role == ROLE_CHAT || f->role == ROLE_PAUSE)
			pill_release(f);
		else if (f->role == ROLE_WEAPON)
			weapon_gesture(f, true);
		else if (f->role == ROLE_LOOK)
		{
			// tap in the look zone fires once (touch_look_tap_fire)
			float dx = f->last_x - f->down_x, dy = f->last_y - f->down_y;
			if (cv("touch_look_tap_fire", 1) && now - f->down_time < cv("touch_look_tap_ms", 200) * 0.001
				&& fabs(dx) < 12 && fabs(dy) < 12)
				tapfire_until = now + 0.08;
			look_active = false;
		}
		f->used = false;
	}

	// 3. roles -> outputs
	move_off_x = move_off_y = 0;
	for (j = 0; j < MAX_FINGERS; j++)
	{
		finger_t *f = &fingers[j];
		if (!f->used)
			continue;
		switch (f->role)
		{
		case ROLE_MOVE:
		{
			float cx, cy, r, ox, oy, mag;
			widget_center(cv("touch_move_x", 0.170f), cv("touch_move_y", 0.680f), &cx, &cy);
			r = max(1.0f, widget_radius(cv("touch_move_size", 0.229f)) * max(0.1f, cv("touch_stick_range", 1.0f)));
			ox = f->last_x - cx;
			oy = f->last_y - cy;
			mag = sqrt(ox * ox + oy * oy);
			if (mag > r)
			{
				ox = ox / mag * r;
				oy = oy / mag * r;
			}
			move_off_x = ox / r;
			move_off_y = oy / r;
			move_active = true;
			break;
		}
		case ROLE_LOOK:
			look_seen = true;
			if (f->first_frame)
				look_first = true;
			break;
		case ROLE_FIRE:
			fire_count = true;
			break;
		case ROLE_JUMP:
			jump_down = true;
			break;
		case ROLE_CROUCH:
			crouch_down = true;
			break;
		case ROLE_SCORES:
			scores_down = true;
			break;
		case ROLE_WEAPON:
			weapon_gesture(f, false);
			break;
		default:
			break;
		}
	}

	// movement keys from the stick (Touch_ApplyMove thresholds)
	if (move_active && sqrt(move_off_x * move_off_x + move_off_y * move_off_y) > cv("touch_stick_deadzone", 0.18f))
	{
		set_key("forward", &key_forward, move_off_y < -0.15f);
		set_key("back", &key_back, move_off_y > 0.15f);
		set_key("moveleft", &key_left, move_off_x < -0.15f);
		set_key("moveright", &key_right, move_off_x > 0.15f);
	}
	else
	{
		set_key("forward", &key_forward, false);
		set_key("back", &key_back, false);
		set_key("moveleft", &key_left, false);
		set_key("moveright", &key_right, false);
	}

	// look
	if (look_seen && look_active && !look_first)
		look_apply(look_dx, look_dy, dt);
	look_prev_time = now;

	// fire: held button, or a look-zone tap
	set_key("attack", &key_attack, fire_count || now < tapfire_until);

	// hop: hold past touch_hop_latch_ms latches bunny-hop; a tap while latched clears it
	if (cv("touch_hop_mode", 1) == 1)
	{
		static qbool prev_jump_down;
		if (jump_down && !prev_jump_down)
		{
			if (hop_latched)
			{
				hop_latched = false;
				hop_cancelling = true;
			}
		}
		if (!jump_down)
			hop_cancelling = false;
		if (jump_down && !hop_cancelling && !hop_latched && now - hop_press_time >= cv("touch_hop_latch_ms", 350) * 0.001)
			hop_latched = true;
		set_key("jump", &key_jump, hop_latched || (jump_down && !hop_cancelling));
		prev_jump_down = jump_down;
	}
	else
		set_key("jump", &key_jump, jump_down);

	set_key("crouch", &key_crouch, crouch_down);
	set_key("showscores", &key_scores, scores_down);
}

// ---------------------------------------------------------------------------
// Vitals readout (touch_hud.qc, condensed: no skin icons, same rows)
// ---------------------------------------------------------------------------
static void num_color(int value, int maxvalue, float *rgb)
{
	float frac = maxvalue > 0 ? (float)value / (float)maxvalue : 0;
	if (value <= 0 || frac <= 0.25f)
		VectorCopy(C_DANGER, rgb);
	else if (frac <= 0.5f)
		VectorCopy(C_WARNING, rgb);
	else if (frac < 1.0f)
		VectorCopy(C_INK, rgb);
	else
		VectorCopy(C_SUCCESS, rgb);
}

static void draw_bar(float lx, float cy, float w, float h, float frac, const float *fill, float a)
{
	float hx = w * 0.5f, hy = h * 0.5f, fw;
	frac = bound(0, frac, 1);
	shape_capsule(lx + hx, cy, hx, hy, C_SHADOW, a * A_SURFACE);
	shape_capsule_ring(lx + hx, cy, hx, hy, C_RIM, a * A_GHOST);
	fw = w * frac;
	if (fw > h)
		shape_capsule(lx + fw * 0.5f, cy, fw * 0.5f, hy, fill, a * 0.92f);
	else if (frac > 0.001f)
		shape_disc(lx + hy, cy, hy, fill, a * 0.92f);
}

static void draw_stat_row(float ox, float oy, float hud_u, const char *tag, int value, int maxvalue, float a)
{
	float icon = 22 * hud_u, num_slot = 54 * hud_u, bar_w = 122 * hud_u, bar_h = 9 * hud_u, row_h = 30 * hud_u;
	float mid_y = oy + row_h * 0.5f;
	float rgb[3];
	char num[16];
	num_color(value, maxvalue, rgb);
	shape_label(ox + icon * 0.5f, mid_y, 12 * hud_u, tag, rgb, a * A_INK);
	dpsnprintf(num, sizeof(num), "%d", value);
	shape_label_right(ox + icon + 6 * hud_u + num_slot, mid_y, FONT_READOUT * hud_u, num, rgb, a * A_INK);
	draw_bar(ox + icon + 6 * hud_u + num_slot + 12 * hud_u, mid_y, bar_w, bar_h, maxvalue > 0 ? (float)value / maxvalue : 0, rgb, a);
}

static void draw_vitals(float a)
{
	float hud_u = ui_scale() * max(0.4f, cv("touch_hud_scale", 0.85f));
	float w = (22 + 6 + 54 + 12 + 122) * hud_u;
	float h = (30 * 3 + 6 * 2) * hud_u;
	float cx, cy, ox, oy, pitch = (30 + 6) * hud_u;
	int health = max(0, cl.stats[STAT_HEALTH]);
	int armor = max(0, cl.stats[STAT_ARMOR]);
	int ammo = max(0, cl.stats[STAT_AMMO]);
	if (!cv("touch_mobile_hud", 1))
		return;
	widget_center(cv("touch_hud_x", 0.150f), cv("touch_hud_y", 0.115f), &cx, &cy);
	ox = cx - w * 0.5f;
	oy = cy - h * 0.5f;
	draw_stat_row(ox, oy, hud_u, "HP", health, max(100, (int)cv("touch_hud_maxhealth", 100)), a);
	draw_stat_row(ox, oy + pitch, hud_u, "AR", armor, max(100, (int)cv("touch_hud_maxarmor", 100)), armor > 0 ? a : a * 0.7f);
	draw_stat_row(ox, oy + pitch * 2, hud_u, "AM", ammo, 100, a);
}

// ---------------------------------------------------------------------------
// Draw (touch_draw.qc)
// ---------------------------------------------------------------------------
static void draw_move_stick(float a)
{
	float cx, cy, r, knob_r, knob_x, knob_y, mag;
	const float *knob_rgb = C_RIM;
	float knob_a = A_SURFACE_ACTIVE;
	qbool active;
	widget_center(cv("touch_move_x", 0.170f), cv("touch_move_y", 0.680f), &cx, &cy);
	r = widget_radius(cv("touch_move_size", 0.229f));
	if (r < 4)
		return;
	mag = sqrt(move_off_x * move_off_x + move_off_y * move_off_y);
	active = mag > cv("touch_stick_deadzone", 0.18f);
	knob_r = r * 0.34f;
	knob_x = cx + move_off_x * (r - knob_r);
	knob_y = cy + move_off_y * (r - knob_r);
	shape_shadow(cx, cy + elevation(r), r * 1.06f, a * A_SHADOW * 0.6f);
	shape_disc(cx, cy, r, C_SURFACE, a * A_SURFACE * 0.75f);
	widget_rim(cx, cy, r, C_RIM, a * A_RIM * 0.85f);
	if (active)
	{
		knob_rgb = C_ACCENT_LIGHT;
		knob_a = A_RIM_ACTIVE;
		shape_glow(knob_x, knob_y, knob_r * 1.6f, C_ACCENT, a * A_GLOW);
	}
	shape_shadow(knob_x, knob_y + elevation(knob_r), knob_r * 1.20f, a * A_SHADOW);
	shape_disc(knob_x, knob_y, knob_r, knob_rgb, a * knob_a);
	widget_rim(knob_x, knob_y, knob_r, C_RIM, a * A_RIM_ACTIVE);
}

static void draw_circle_widget(const char *xk, float dx, const char *yk, float dy, const char *sk, float ds, const char *label, qbool pressed, float a, const float *accent)
{
	float cx, cy;
	widget_center(cv(xk, dx), cv(yk, dy), &cx, &cy);
	widget_button(cx, cy, widget_radius(cv(sk, ds)), label, pressed, a, accent);
}

static void draw_pill(const char *xk, float dx, const char *yk, float dy, const char *sk, float ds, const char *ak, float da, const char *label, qbool pressed, float a)
{
	float cx, cy, hx, hy;
	widget_center(cv(xk, dx), cv(yk, dy), &cx, &cy);
	widget_half(cv(sk, ds), cv(ak, da), &hx, &hy);
	widget_chrome_pill(cx, cy, hx, hy, label, pressed, a, C_ACCENT);
}

void TouchHUD_Draw(void)
{
	float a;
	float cx, cy, hx, hy;
	if (!TouchHUD_Active())
		return;
	shapes_probe();
	a = bound(0.1f, cv("touch_opacity", 0.90f), 1.0f);

	if (cv("touch_pause_visible", 1))
		draw_pill("touch_pause_x", 0.680f, "touch_pause_y", 0.088f, "touch_pause_size", 0.060f, "touch_pause_aspect", 2.9f, "MENU", role_owned(ROLE_PAUSE), a);
	if (cv("touch_chat_visible", 1))
		draw_pill("touch_chat_x", 0.862f, "touch_chat_y", 0.088f, "touch_chat_size", 0.060f, "touch_chat_aspect", 2.9f, "CHAT", role_owned(ROLE_CHAT), a);
	if (cv("touch_con_visible", 1))
		draw_pill("touch_con_x", 0.090f, "touch_con_y", 0.948f, "touch_con_size", 0.060f, "touch_con_aspect", 2.9f, "CONSOLE", role_owned(ROLE_CON), a);
	if (cv("touch_scores_visible", 1))
		draw_pill("touch_scores_x", 0.272f, "touch_scores_y", 0.948f, "touch_scores_size", 0.060f, "touch_scores_aspect", 2.9f, "SCORE", key_scores, a);

	if (cv("touch_move_visible", 1))
		draw_move_stick(a);
	if (cv("touch_fire_visible", 1))
		draw_circle_widget("touch_fire_x", 0.855f, "touch_fire_y", 0.680f, "touch_fire_size", 0.140f, "FIRE", key_attack, a, C_DANGER);
	if (cv("touch_jump_visible", 1))
	{
		widget_center(cv("touch_jump_x", 0.855f), cv("touch_jump_y", 0.885f), &cx, &cy);
		widget_half(cv("touch_jump_size", 0.072f), cv("touch_jump_aspect", 2.6f), &hx, &hy);
		widget_capsule(cx, cy, hx, hy, "HOP", key_jump && !hop_latched, hop_latched, a, C_ACCENT);
	}
	if (cv("touch_crouch_visible", 1))
		draw_circle_widget("touch_crouch_x", 0.700f, "touch_crouch_y", 0.885f, "touch_crouch_size", 0.066f, "DUCK", key_crouch, a, C_ACCENT);
	if (weapon_button_shown())
		draw_circle_widget("touch_weapon_x", 0.540f, "touch_weapon_y", 0.860f, "touch_weapon_size", 0.100f, "WEP", role_owned(ROLE_WEAPON), a, C_ACCENT);

	draw_vitals(a);
}
