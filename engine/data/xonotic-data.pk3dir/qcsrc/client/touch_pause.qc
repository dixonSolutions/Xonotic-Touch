#include "touch_pause.qh"
#include "touch_layout.qh"
#include "touch_glass.qh"
#include "touch_init.qh"
#include "touch_defs.qh"
#include "touch_input.qh"

int touch_pause_pill_state;
vector touch_pause_down_pos;

void Touch_Pause_Reset(void)
{
	touch_pause_pill_state = TOUCH_PAUSE_IDLE;
}

void Touch_Pause_InitDefaults(void)
{
	if (cvar("touch_pause_x") <= 0.001)
		cvar_set("touch_pause_x", "0.300");
	if (cvar("touch_pause_y") <= 0.001)
		cvar_set("touch_pause_y", "0.050");
	if (cvar("touch_pause_visible") <= 0)
		cvar_set("touch_pause_visible", "1");
	Touch_Pause_Reset();
}

// Legacy API — custom pause sheet removed; Xonotic GameMenu handles pause.
bool Touch_Pause_SheetOpen(void)
{
	return false;
}

int Touch_Pause_SheetOpenInt(void)
{
	return 0;
}

bool Touch_Pause_Busy(void)
{
	return touch_pause_pill_state == TOUCH_PAUSE_ARMING;
}

vector Touch_Pause_PillCenter(void)
{
	return Touch_WidgetCenter(autocvar_touch_pause_x, autocvar_touch_pause_y, autocvar_touch_pause_size);
}

vector Touch_Pause_PillHalf(void)
{
	return Touch_WidgetHalfExtents(autocvar_touch_pause_size, autocvar_touch_pause_aspect);
}

bool Touch_Pause_PillHit(vector p)
{
	if (!autocvar_touch_pause_visible)
		return false;
	return Touch_HitRect(p, autocvar_touch_pause_x, autocvar_touch_pause_y, autocvar_touch_pause_size, autocvar_touch_pause_aspect, 1.20);
}

void Touch_Pause_ClearGameplayKeys(void)
{
	Touch_SetKey("attack", false);
	Touch_SetKey("jump", false);
	Touch_SetKey("crouch", false);
	Touch_SetKey("zoom", false);
	Touch_SetKey("forward", false);
	Touch_SetKey("back", false);
	Touch_SetKey("moveleft", false);
	Touch_SetKey("moveright", false);
}

// Same path as Escape in CSQC_InputEvent (client/main.qc): open GameMenu / menu.
void Touch_Pause_TriggerEscape(void)
{
	Touch_ReleaseAll();
	Touch_Pause_ClearGameplayKeys();
	// Mirror Escape handling in CSQC_InputEvent (client/main.qc).
	if (cvar("menu_gamemenu") && !isdemo() && cvar("_menu_gamemenu_dialog_available"))
		localcmd("\nmenu_showgamemenudialog\n");
	else
		localcmd("\ntogglemenu 1\n");
	if (autocvar_touch_debug)
		localcmd("echo touch_pause_escape\n");
}

void Touch_Pause_Enter(void)
{
	Touch_Pause_TriggerEscape();
}

void Touch_Pause_Exit(void)
{
	// Close native menu (GameMenu Resume / Escape equivalent).
	localcmd("\ntogglemenu 0\n");
}

int Touch_Pause_OnDown(vector p)
{
	if (!Touch_Pause_PillHit(p))
		return TOUCH_ROLE_NONE;
	touch_pause_pill_state = TOUCH_PAUSE_ARMING;
	touch_pause_down_pos = p;
	return TOUCH_ROLE_PAUSE;
}

void Touch_Pause_OnMove(vector p)
{
	if (touch_pause_pill_state != TOUCH_PAUSE_ARMING)
		return;
	vector d = p - touch_pause_down_pos;
	d.z = 0;
	if (vlen(d) > autocvar_touch_pause_slop_px)
		touch_pause_pill_state = TOUCH_PAUSE_IDLE; // CANCEL — look swipe
}

void Touch_Pause_OnUp(vector p)
{
	if (touch_pause_pill_state == TOUCH_PAUSE_ARMING)
	{
		vector d = p - touch_pause_down_pos;
		d.z = 0;
		if (vlen(d) <= autocvar_touch_pause_slop_px)
			Touch_Pause_TriggerEscape();
	}
	touch_pause_pill_state = TOUCH_PAUSE_IDLE;
}

void Touch_Pause_Frame(void)
{
}

void Touch_Pause_DrawLabel(vector c, string label, float fs, vector rgb, float alpha)
{
	float tw = strlen(label) * fs * 0.55;
	drawstring_builtin(c - eX * (tw * 0.5) - eY * (fs * 0.35), label, '1 1 0' * fs, rgb, alpha, 0);
}

void Touch_Pause_DrawPill(float alpha)
{
	if (!autocvar_touch_pause_visible)
		return;
	vector c = Touch_Pause_PillCenter();
	vector half = Touch_Pause_PillHalf();
	bool pressed = (touch_pause_pill_state == TOUCH_PAUSE_ARMING);
	vector tint = TOUCH_GLASS_TINT;
	if (pressed)
		tint = TOUCH_GLASS_ACCENT;
	Touch_Glass_DrawBar(c, half.x * 2, half.y * 2, tint, max(0.75, alpha), pressed);
	float fs = bound(12, half.y * 0.85, 18);
	Touch_Pause_DrawLabel(c, "PAUSE", fs, TOUCH_GLASS_HIGHLIGHT, 1);
}

void Touch_Pause_DrawSheet(float alpha)
{
	// Custom sheet removed — Xonotic menu owns pause UI.
}
