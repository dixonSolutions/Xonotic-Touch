#include "touch_pause.qh"
#include "touch_layout.qh"
#include "touch_glass.qh"
#include "touch_init.qh"
#include "touch_defs.qh"
#include "touch_input.qh"

bool touch_pause_sheet_open;
bool touch_pause_engine_paused;
int touch_pause_pill_state;
vector touch_pause_down_pos;
float touch_pause_down_realtime;
int touch_pause_pressed_btn;
float touch_pause_quit_arm_until;
bool touch_pause_quit_armed;

float Touch_Pause_Realtime(void)
{
	return gettime(GETTIME_REALTIME);
}

void Touch_Pause_Reset(void)
{
	touch_pause_pill_state = TOUCH_PAUSE_IDLE;
	touch_pause_pressed_btn = TOUCH_PAUSE_BTN_NONE;
}

void Touch_Pause_InitDefaults(void)
{
	if (cvar("touch_pause_x") <= 0.001)
		cvar_set("touch_pause_x", "0.300");
	if (cvar("touch_pause_y") <= 0.001)
		cvar_set("touch_pause_y", "0.050");
	// Ensure the pill cannot be left invisible by a stale layout file.
	if (cvar("touch_pause_visible") <= 0)
		cvar_set("touch_pause_visible", "1");
	Touch_Pause_Reset();
	touch_pause_sheet_open = false;
	touch_pause_engine_paused = false;
	touch_pause_quit_armed = false;
	touch_pause_quit_arm_until = 0;
}

bool Touch_Pause_SheetOpen(void)
{
	return touch_pause_sheet_open;
}

int Touch_Pause_SheetOpenInt(void)
{
	if (touch_pause_sheet_open)
		return 1;
	return 0;
}

bool Touch_Pause_Busy(void)
{
	return touch_pause_pill_state == TOUCH_PAUSE_ARMING || touch_pause_sheet_open;
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
	if (touch_pause_sheet_open)
		return false;
	return Touch_HitRect(p, autocvar_touch_pause_x, autocvar_touch_pause_y, autocvar_touch_pause_size, autocvar_touch_pause_aspect, 1.20);
}

// Fixed sheet geometry in normalized console space (independent of touch_scale).
vector Touch_Pause_BtnCenter(int btn)
{
	vector ui = Touch_UISize();
	float cy = 0.313;
	if (btn == TOUCH_PAUSE_BTN_MENU)
		cy = 0.505;
	else if (btn == TOUCH_PAUSE_BTN_LEAVE)
		cy = 0.688;
	else if (btn == TOUCH_PAUSE_BTN_QUIT)
		cy = 0.861;
	return 0.5 * ui.x * eX + cy * ui.y * eY;
}

vector Touch_Pause_BtnHalf(int btn)
{
	vector ui = Touch_UISize();
	float short_n = 0.115;
	float aspect = 4.4;
	if (btn == TOUCH_PAUSE_BTN_MENU || btn == TOUCH_PAUSE_BTN_LEAVE)
	{
		short_n = 0.095;
		aspect = 4.0;
	}
	else if (btn == TOUCH_PAUSE_BTN_QUIT)
	{
		short_n = 0.075;
		aspect = 3.0;
	}
	float short_axis = min(ui.x, ui.y) * short_n;
	float long_axis = short_axis * aspect;
	return (long_axis * 0.5) * eX + (short_axis * 0.5) * eY;
}

int Touch_Pause_HitButton(vector p)
{
	int b;
	for (b = TOUCH_PAUSE_BTN_RESUME; b <= TOUCH_PAUSE_BTN_QUIT; ++b)
		if (Touch_HitRectPx(p, Touch_Pause_BtnCenter(b), Touch_Pause_BtnHalf(b), 1.05))
			return b;
	return TOUCH_PAUSE_BTN_NONE;
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

void Touch_Pause_Enter(void)
{
	if (touch_pause_sheet_open)
		return;
	Touch_ReleaseAll();
	touch_pause_sheet_open = true;
	touch_pause_pill_state = TOUCH_PAUSE_IDLE;
	touch_pause_pressed_btn = TOUCH_PAUSE_BTN_NONE;
	touch_pause_quit_armed = false;
	touch_pause_quit_arm_until = 0;
	touch_pause_engine_paused = false;
	Touch_Pause_ClearGameplayKeys();
	// Optional engine freeze AFTER sheet is armed. Prefer touch_pause_freeze 0 so
	// the CSQC sheet stays the visible escape UI (engine "PAUSE" can obscure it).
	if (autocvar_touch_pause_freeze)
	{
		localcmd("pause\n");
		touch_pause_engine_paused = true;
	}
	if (autocvar_touch_debug)
	{
		localcmd("echo touch_pause_sheet_open\n");
		localcmd("screenshot\n");
	}
}

void Touch_Pause_Exit(void)
{
	if (!touch_pause_sheet_open)
		return;
	touch_pause_sheet_open = false;
	touch_pause_pill_state = TOUCH_PAUSE_IDLE;
	touch_pause_pressed_btn = TOUCH_PAUSE_BTN_NONE;
	touch_pause_quit_armed = false;
	touch_pause_quit_arm_until = 0;
	if (touch_pause_engine_paused)
	{
		localcmd("pause\n");
		touch_pause_engine_paused = false;
	}
	Touch_Pause_ClearGameplayKeys();
}

void Touch_Pause_RunAction(int btn)
{
	if (btn == TOUCH_PAUSE_BTN_RESUME)
	{
		Touch_Pause_Exit();
		return;
	}
	if (btn == TOUCH_PAUSE_BTN_MENU)
	{
		Touch_Pause_Exit();
		if (cvar("menu_gamemenu") && !isdemo() && cvar("_menu_gamemenu_dialog_available"))
			localcmd("\nmenu_showgamemenudialog\n");
		else
			localcmd("\ntogglemenu 1\n");
		return;
	}
	if (btn == TOUCH_PAUSE_BTN_LEAVE)
	{
		Touch_Pause_Exit();
		localcmd("disconnect\n");
		return;
	}
	if (btn == TOUCH_PAUSE_BTN_QUIT)
	{
		float now = Touch_Pause_Realtime();
		if (touch_pause_quit_armed && now <= touch_pause_quit_arm_until)
		{
			touch_pause_quit_armed = false;
			localcmd("quit\n");
			return;
		}
		touch_pause_quit_armed = true;
		touch_pause_quit_arm_until = now + (autocvar_touch_pause_quit_confirm_ms / 1000.0);
	}
}

int Touch_Pause_OnDown(vector p)
{
	if (touch_pause_sheet_open)
	{
		touch_pause_pressed_btn = Touch_Pause_HitButton(p);
		touch_pause_down_pos = p;
		touch_pause_down_realtime = Touch_Pause_Realtime();
		return TOUCH_ROLE_SHEET;
	}
	if (!Touch_Pause_PillHit(p))
		return TOUCH_ROLE_NONE;
	touch_pause_pill_state = TOUCH_PAUSE_ARMING;
	touch_pause_down_pos = p;
	touch_pause_down_realtime = Touch_Pause_Realtime();
	return TOUCH_ROLE_PAUSE;
}

void Touch_Pause_OnMove(vector p)
{
	if (touch_pause_sheet_open)
	{
		if (touch_pause_pressed_btn != TOUCH_PAUSE_BTN_NONE)
		{
			if (Touch_Pause_HitButton(p) != touch_pause_pressed_btn)
				touch_pause_pressed_btn = TOUCH_PAUSE_BTN_NONE;
		}
		return;
	}
	if (touch_pause_pill_state != TOUCH_PAUSE_ARMING)
		return;
	vector d = p - touch_pause_down_pos;
	d.z = 0;
	if (vlen(d) > autocvar_touch_pause_slop_px)
		touch_pause_pill_state = TOUCH_PAUSE_IDLE; // CANCEL — look swipe
}

void Touch_Pause_OnUp(vector p)
{
	if (touch_pause_sheet_open)
	{
		int btn = touch_pause_pressed_btn;
		touch_pause_pressed_btn = TOUCH_PAUSE_BTN_NONE;
		if (btn != TOUCH_PAUSE_BTN_NONE && Touch_Pause_HitButton(p) == btn)
			Touch_Pause_RunAction(btn);
		return;
	}
	if (touch_pause_pill_state == TOUCH_PAUSE_ARMING)
	{
		vector d = p - touch_pause_down_pos;
		d.z = 0;
		if (vlen(d) <= autocvar_touch_pause_slop_px)
			Touch_Pause_Enter();
	}
	touch_pause_pill_state = TOUCH_PAUSE_IDLE;
}

void Touch_Pause_Frame(void)
{
	if (!touch_pause_sheet_open)
		return;
	Touch_Pause_ClearGameplayKeys();
	if (touch_pause_quit_armed && Touch_Pause_Realtime() > touch_pause_quit_arm_until)
		touch_pause_quit_armed = false;
}

void Touch_Pause_DrawLabel(vector c, string label, float fs, vector rgb, float alpha)
{
	float tw = strlen(label) * fs * 0.55;
	drawstring_builtin(c - eX * (tw * 0.5) - eY * (fs * 0.35), label, '1 1 0' * fs, rgb, alpha, 0);
}

void Touch_Pause_DrawPill(float alpha)
{
	if (!autocvar_touch_pause_visible || touch_pause_sheet_open)
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

void Touch_Pause_DrawSheetButton(int btn, float alpha)
{
	vector c = Touch_Pause_BtnCenter(btn);
	vector half = Touch_Pause_BtnHalf(btn);
	bool pressed = (touch_pause_pressed_btn == btn);
	vector tint = TOUCH_GLASS_TINT;
	string label = "RESUME";
	if (btn == TOUCH_PAUSE_BTN_MENU)
		label = "MENU";
	else if (btn == TOUCH_PAUSE_BTN_LEAVE)
	{
		if (isdemo())
			label = "STOP DEMO";
		else
			label = "LEAVE MATCH";
	}
	else if (btn == TOUCH_PAUSE_BTN_QUIT)
	{
		tint = TOUCH_GLASS_DANGER;
		if (touch_pause_quit_armed)
			label = "TAP AGAIN TO QUIT";
		else
			label = "QUIT";
	}
	if (btn == TOUCH_PAUSE_BTN_RESUME)
		tint = TOUCH_GLASS_ACCENT;
	if (pressed)
		tint = tint * 0.85 + TOUCH_GLASS_HIGHLIGHT * 0.15;
	Touch_Glass_DrawBar(c, half.x * 2, half.y * 2, tint, max(0.80, alpha), pressed);
	float fs = bound(14, half.y * 0.55, 22);
	if (btn == TOUCH_PAUSE_BTN_QUIT)
		fs = bound(11, half.y * 0.45, 16);
	Touch_Pause_DrawLabel(c, label, fs, TOUCH_GLASS_HIGHLIGHT, 1);
}

void Touch_Pause_DrawSheet(float alpha)
{
	if (!touch_pause_sheet_open)
		return;
	vector ui = Touch_UISize();
	Touch_Glass_Fill('0 0 0', ui, TOUCH_GLASS_SHADOW, 0.55 * alpha);

	vector title_c = 0.5 * ui.x * eX + 0.203 * ui.y * eY;
	string title = "GAME MENU";
	if (touch_pause_engine_paused)
		title = "PAUSED";
	Touch_Pause_DrawLabel(title_c, title, 22, TOUCH_GLASS_HIGHLIGHT, 1);

	Touch_Pause_DrawSheetButton(TOUCH_PAUSE_BTN_RESUME, alpha);
	Touch_Pause_DrawSheetButton(TOUCH_PAUSE_BTN_MENU, alpha);
	Touch_Pause_DrawSheetButton(TOUCH_PAUSE_BTN_LEAVE, alpha);
	Touch_Pause_DrawSheetButton(TOUCH_PAUSE_BTN_QUIT, alpha);

	vector hint_c = 0.5 * ui.x * eX + 0.938 * ui.y * eY;
	Touch_Pause_DrawLabel(hint_c, "TAP RESUME TO CONTINUE", 11, TOUCH_GLASS_TINT, 0.85);
}
