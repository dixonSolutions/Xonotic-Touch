#include "touch_console.qh"
#include "touch_layout.qh"
#include "touch_glass.qh"
#include "touch_init.qh"
#include "touch_config.qh"
#include "touch_defs.qh"

int touch_con_state;
vector touch_con_down_pos;
vector touch_con_grab_offset;
float touch_con_down_time;
vector touch_con_preview_center;
bool touch_con_preview_live;
float touch_con_flash_until;
bool touch_con_flash_danger;
float touch_autosave_at;

void Touch_Console_Reset(void)
{
	touch_con_state = TOUCH_CON_IDLE;
	touch_con_preview_live = false;
}

void Touch_Console_InitDefaults(void)
{
	if (cvar("touch_con_x") <= 0.001)
		cvar_set("touch_con_x", "0.5");
	if (cvar("touch_con_y") <= 0.001)
		cvar_set("touch_con_y", "0.045");
	Touch_Console_Reset();
	touch_autosave_at = 0;
}

float Touch_Console_X(void)
{
	float x = autocvar_touch_con_x;
	if (x <= 0.001)
		x = 0.5;
	return x;
}

float Touch_Console_Y(void)
{
	float y = autocvar_touch_con_y;
	if (y <= 0.001)
		y = 0.045;
	return y;
}

bool Touch_Console_Dragging(void)
{
	return touch_con_state == TOUCH_CON_DRAGGING;
}

bool Touch_Console_Arming(void)
{
	return touch_con_state == TOUCH_CON_ARMING;
}

bool Touch_Console_Busy(void)
{
	return touch_con_state == TOUCH_CON_ARMING || touch_con_state == TOUCH_CON_DRAGGING;
}

vector Touch_Console_CenterPx(void)
{
	if (touch_con_preview_live)
		return touch_con_preview_center;
	return Touch_WidgetCenter(Touch_Console_X(), Touch_Console_Y(), autocvar_touch_con_size);
}

vector Touch_Console_Half(void)
{
	return Touch_WidgetHalfExtents(autocvar_touch_con_size, autocvar_touch_con_aspect);
}

bool Touch_Console_Hit(vector p)
{
	if (!autocvar_touch_con_visible)
		return false;
	return Touch_HitRect(p, Touch_Console_X(), Touch_Console_Y(), autocvar_touch_con_size, autocvar_touch_con_aspect, 1.20);
}

vector Touch_Console_ClampCenter(vector c)
{
	vector ui = Touch_UISize();
	vector half = Touch_Console_Half();
	float m = autocvar_touch_con_margin_px;
	c.x = bound(half.x + m, c.x, ui.x - half.x - m);
	c.y = bound(half.y + m, c.y, ui.y - half.y - m);
	c.z = 0;
	return c;
}

float Touch_Console_Quantize(float n, float snap)
{
	if (snap <= 0)
		return n;
	return floor(n / snap + 0.5) * snap;
}

int Touch_Console_OnDown(vector p)
{
	if (!Touch_Console_Hit(p))
		return TOUCH_ROLE_NONE;
	touch_con_state = TOUCH_CON_ARMING;
	touch_con_down_pos = p;
	touch_con_down_time = time;
	touch_con_grab_offset = Touch_Console_CenterPx() - p;
	touch_con_preview_live = false;
	return TOUCH_ROLE_CON;
}

void Touch_Console_OnMove(vector p)
{
	if (touch_con_state == TOUCH_CON_ARMING)
	{
		vector d = p - touch_con_down_pos;
		d.z = 0;
		if (vlen(d) > autocvar_touch_con_tap_slop_px && (time - touch_con_down_time) * 1000 < autocvar_touch_con_hold_ms)
		{
			touch_con_state = TOUCH_CON_IDLE; // CANCEL → ignore (look swipe)
			return;
		}
		if ((time - touch_con_down_time) * 1000 >= autocvar_touch_con_hold_ms && !autocvar_touch_con_lock)
		{
			touch_con_state = TOUCH_CON_DRAGGING;
			touch_con_preview_live = true;
			touch_con_preview_center = Touch_Console_ClampCenter(p + touch_con_grab_offset);
		}
	}
	else if (touch_con_state == TOUCH_CON_DRAGGING)
	{
		touch_con_preview_center = Touch_Console_ClampCenter(p + touch_con_grab_offset);
		touch_con_preview_live = true;
	}
}

void Touch_Console_PersistSoon(void)
{
	touch_autosave_at = time + 1.0;
}

void Touch_Console_OnUp(vector p)
{
	if (touch_con_state == TOUCH_CON_ARMING)
	{
		vector d = p - touch_con_down_pos;
		d.z = 0;
		if ((time - touch_con_down_time) * 1000 < autocvar_touch_con_hold_ms && vlen(d) <= autocvar_touch_con_tap_slop_px)
			localcmd("toggleconsole\n");
		touch_con_state = TOUCH_CON_IDLE;
		return;
	}
	if (touch_con_state == TOUCH_CON_DRAGGING)
	{
		vector ui = Touch_UISize();
		vector c = Touch_Console_ClampCenter(touch_con_preview_center);
		float nx = Touch_Console_Quantize(c.x / ui.x, autocvar_touch_con_snap);
		float ny = Touch_Console_Quantize(c.y / ui.y, autocvar_touch_con_snap);
		if (fabs(nx - 0.5) < 0.02)
			nx = 0.5;
		cvar_set("touch_con_x", ftos(nx));
		cvar_set("touch_con_y", ftos(ny));
		touch_con_flash_until = time + 0.25;
		touch_con_flash_danger = false;
		Touch_Console_PersistSoon();
	}
	touch_con_state = TOUCH_CON_IDLE;
	touch_con_preview_live = false;
}

void Touch_Console_PromoteHold(void)
{
	// Stationary hold→drag. Only call while a live CON finger is still reported (B4).
	if (touch_con_state != TOUCH_CON_ARMING || autocvar_touch_con_lock)
		return;
	if ((time - touch_con_down_time) * 1000 < autocvar_touch_con_hold_ms)
		return;
	touch_con_state = TOUCH_CON_DRAGGING;
	touch_con_preview_live = true;
	touch_con_preview_center = Touch_Console_ClampCenter(touch_con_down_pos + touch_con_grab_offset);
}

void Touch_Console_Frame(void)
{
	if (touch_autosave_at > 0 && time >= touch_autosave_at)
	{
		touch_autosave_at = 0;
		Touch_SaveLayoutFile(autocvar_touch_layout_file);
	}
}

void Touch_Console_Draw(float alpha)
{
	if (!autocvar_touch_con_visible)
		return;
	vector c = Touch_Console_CenterPx();
	vector half = Touch_Console_Half();
	bool pressed = (touch_con_state == TOUCH_CON_ARMING || touch_con_state == TOUCH_CON_DRAGGING);
	vector tint = TOUCH_GLASS_TINT;
	if (touch_con_state == TOUCH_CON_ARMING)
		tint = TOUCH_GLASS_ACCENT;
	if (time < touch_con_flash_until)
	{
		if (touch_con_flash_danger)
			tint = TOUCH_GLASS_DANGER;
		else
			tint = TOUCH_GLASS_ACCENT;
	}
	// Wide top pill — readable “Console” control, not a faint CON chip.
	Touch_Glass_DrawBar(c, half.x * 2, half.y * 2, tint, max(0.75, alpha), pressed);
	float fs = bound(12, half.y * 0.85, 18);
	string label = "CONSOLE";
	float tw = strlen(label) * fs * 0.55;
	drawstring_builtin(c - eX * (tw * 0.5) - eY * (fs * 0.35), label, '1 1 0' * fs, TOUCH_GLASS_HIGHLIGHT, 1, 0);
}
