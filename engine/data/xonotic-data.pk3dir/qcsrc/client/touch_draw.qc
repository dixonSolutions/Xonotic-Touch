#include "touch_draw.qh"
#include "touch_customize.qh"
#include "touch_hud.qh"
#include "touch_glass.qh"
#include "touch_console.qh"
#include "touch_pause.qh"
#include "touch_init.qh"
#include "touch_input.qh"
#include "touch_defs.qh"

void Touch_DrawLabel(vector center, float size, string label, float theAlpha)
{
	// Cap font: size used to be widget diameter → huge overlapping text at the bezel.
	float fs = bound(11, size * 0.14, 18);
	vector sz = '1 1 0' * fs;
	float tw = strlen(label) * fs * 0.55;
	vector pos = center - eX * (tw * 0.5) - eY * (fs * 0.35);
	pos.z = 0;
	drawstring_builtin(pos, label, sz, TOUCH_GLASS_HIGHLIGHT, theAlpha, 0);
}

void Touch_DrawGlassCircle(float nx, float ny, float nsize, string label, bool pressed, float theAlpha, vector tint)
{
	vector c = Touch_WidgetCenter(nx, ny, nsize);
	float r = Touch_WidgetRadius(nsize);
	int q = autocvar_touch_glass_quality;
	if (q <= 0)
	{
		vector pos = c - eX * r - eY * r;
		Touch_Glass_Fill(pos + '1 2 0', eX * (r * 2) + eY * (r * 2), TOUCH_GLASS_SHADOW, theAlpha * 0.45);
		Touch_Glass_Fill(pos, eX * (r * 2) + eY * (r * 2), tint, theAlpha * 0.70);
		Touch_Glass_Frame(pos, eX * (r * 2) + eY * (r * 2), TOUCH_GLASS_HIGHLIGHT, theAlpha * 0.65, 2);
		Touch_DebugAddFills(3);
	}
	else
	{
		Touch_Glass_DrawRoundedButton(c, r * 2, tint, theAlpha, pressed);
		Touch_DebugAddFills(pressed ? 6 : 5);
	}
	Touch_DrawLabel(c, r * 2, label, theAlpha * 0.9);
}

void Touch_DrawSpaceBar(float theAlpha)
{
	vector c = Touch_WidgetCenter(autocvar_touch_jump_x, autocvar_touch_jump_y, autocvar_touch_jump_size);
	vector half = Touch_WidgetHalfExtents(autocvar_touch_jump_size, autocvar_touch_jump_aspect);
	bool pressed = Touch_JumpHeld();
	vector tint = TOUCH_GLASS_TINT;
	if (pressed)
		tint = TOUCH_GLASS_ACCENT;
	int q = autocvar_touch_glass_quality;
	if (q <= 0)
	{
		vector pos = c - eX * half.x - eY * half.y;
		Touch_Glass_Fill(pos + '1 2 0', eX * (half.x * 2) + eY * (half.y * 2), TOUCH_GLASS_SHADOW, theAlpha * 0.45);
		Touch_Glass_Fill(pos, eX * (half.x * 2) + eY * (half.y * 2), tint, theAlpha * 0.75);
		Touch_Glass_Frame(pos, eX * (half.x * 2) + eY * (half.y * 2), TOUCH_GLASS_HIGHLIGHT, theAlpha * 0.70, 2);
		Touch_DebugAddFills(3);
	}
	else
	{
		Touch_Glass_DrawBar(c, half.x * 2, half.y * 2, tint, theAlpha, pressed);
		Touch_DebugAddFills(pressed ? 4 : 3);
	}
	Touch_DrawLabel(c, half.y * 2, "JUMP", theAlpha * 0.95);
}

void Touch_DrawMoveStick(float theAlpha)
{
	vector move_c = Touch_WidgetCenter(autocvar_touch_move_x, autocvar_touch_move_y, autocvar_touch_move_size);
	float move_r = Touch_WidgetRadius(autocvar_touch_move_size);
	vector knob_off = Touch_MoveStickOffset() * move_r * 0.55;
	bool active = vlen(Touch_MoveStickOffset()) > autocvar_touch_stick_deadzone;
	int q = autocvar_touch_glass_quality;

	if (q <= 0)
	{
		vector pos = move_c - eX * move_r - eY * move_r;
		Touch_Glass_Fill(pos, eX * (move_r * 2) + eY * (move_r * 2), TOUCH_GLASS_SHADOW, theAlpha * 0.50);
		Touch_Glass_Fill(pos + '3 3 0', eX * (move_r * 2 - 6) + eY * (move_r * 2 - 6), TOUCH_GLASS_TINT, theAlpha * 0.70);
		Touch_Glass_Frame(pos, eX * (move_r * 2) + eY * (move_r * 2), TOUCH_GLASS_HIGHLIGHT, theAlpha * 0.65, 2);
		float kr = move_r * 0.38;
		vector kc = move_c + knob_off;
		vector kpos = kc - eX * kr - eY * kr;
		vector ktint = TOUCH_GLASS_HIGHLIGHT;
		if (active)
			ktint = TOUCH_GLASS_ACCENT;
		Touch_Glass_Fill(kpos, eX * (kr * 2) + eY * (kr * 2), ktint, theAlpha * 0.85);
		Touch_DebugAddFills(4);
	}
	else
	{
		Touch_Glass_DrawJoystickBase(move_c, move_r, theAlpha);
		Touch_Glass_DrawJoystickKnob(move_c, move_r, knob_off, theAlpha, active);
		Touch_DebugAddFills(10);
	}
	Touch_DrawLabel(move_c - eY * (move_r * 0.92), move_r, "MOVE", theAlpha * 0.95);
}

void Touch_DrawWeaponWheel(float theAlpha)
{
	if (!Touch_WeaponWheelOpen())
		return;
	if (autocvar_touch_glass_quality < 2)
		return;
	vector ui = Touch_UISize();
	vector center = ui.x * 0.5 * eX + ui.y * 0.45 * eY;
	float radius = min(ui.x, ui.y) * 0.22;
	Touch_Glass_DrawCircle(center, radius, TOUCH_GLASS_TINT, theAlpha * 0.55);
	Touch_Glass_DrawCircleOutline(center, radius, TOUCH_GLASS_HIGHLIGHT, theAlpha * 0.4, 2);
	Touch_DebugAddFills(9);
}

void Touch_DrawDebugProbes(void)
{
	// High-contrast markers at live widget centers — proves CSQC 2D lands in the TGA.
	if (!autocvar_touch_debug)
		return;
	vector c;
	float r;
	c = Touch_WidgetCenter(autocvar_touch_move_x, autocvar_touch_move_y, autocvar_touch_move_size);
	r = max(8, Touch_WidgetRadius(autocvar_touch_move_size) * 0.35);
	drawfill_builtin(c - eX * r - eY * r, eX * (r * 2) + eY * (r * 2), '1 0 0', 0.85, DRAWFLAG_NORMAL);
	c = Touch_WidgetCenter(autocvar_touch_fire_x, autocvar_touch_fire_y, autocvar_touch_fire_size);
	r = max(8, Touch_WidgetRadius(autocvar_touch_fire_size) * 0.35);
	drawfill_builtin(c - eX * r - eY * r, eX * (r * 2) + eY * (r * 2), '0 1 0', 0.85, DRAWFLAG_NORMAL);
	c = Touch_WidgetCenter(autocvar_touch_jump_x, autocvar_touch_jump_y, autocvar_touch_jump_size);
	vector half = Touch_WidgetHalfExtents(autocvar_touch_jump_size, autocvar_touch_jump_aspect);
	drawfill_builtin(c - eX * half.x - eY * half.y, eX * (half.x * 2) + eY * (half.y * 2), '0 0.6 1', 0.85, DRAWFLAG_NORMAL);
	Touch_DebugAddFills(3);
}

void Touch_DrawDebugLine(void)
{
	if (!autocvar_touch_debug)
		return;
	string line = strcat("f0:", Touch_DebugRoleName(Touch_DebugFingerRole(0)));
	line = strcat(line, strcat(" f1:", Touch_DebugRoleName(Touch_DebugFingerRole(1))));
	line = strcat(line, strcat(" f2:", Touch_DebugRoleName(Touch_DebugFingerRole(2))));
	line = strcat(line, strcat(" f3:", Touch_DebugRoleName(Touch_DebugFingerRole(3))));
	line = strcat(line, strcat(" fills ", ftos(Touch_DebugFills())));
	line = strcat(line, strcat(" cmd/s ", ftos(Touch_DebugCmdsPerSec())));
	line = strcat(line, strcat(" con ", ftos(autocvar_touch_con_x)));
	line = strcat(line, strcat(",", ftos(autocvar_touch_con_y)));
	drawstring_builtin('8 8 0', line, '1 1 0' * 10, TOUCH_GLASS_HIGHLIGHT, 0.85, 0);
}

void Touch_DrawOverlay(float overlayAlpha)
{
	Touch_Pause_DrawPill(overlayAlpha);
	Touch_Console_Draw(overlayAlpha);
	Touch_DebugAddFills(Touch_Console_Dragging() ? 4 : 3);

	if (autocvar_touch_move_visible || autocvar_touch_debug)
		Touch_DrawMoveStick(overlayAlpha);

	if (autocvar_touch_fire_visible || autocvar_touch_debug)
	{
		vector fire_tint = TOUCH_GLASS_TINT;
		if (Touch_FireHeld())
			fire_tint = TOUCH_GLASS_DANGER;
		Touch_DrawGlassCircle(autocvar_touch_fire_x, autocvar_touch_fire_y, autocvar_touch_fire_size, "FIRE", Touch_FireHeld(), overlayAlpha, fire_tint);
	}

	if (autocvar_touch_jump_visible || autocvar_touch_debug)
		Touch_DrawSpaceBar(overlayAlpha);

	if (autocvar_touch_crouch_visible || autocvar_touch_debug)
		Touch_DrawGlassCircle(autocvar_touch_crouch_x, autocvar_touch_crouch_y, autocvar_touch_crouch_size, "CR", Touch_CrouchHeld(), overlayAlpha, TOUCH_GLASS_TINT);
	if (autocvar_touch_weapon_visible || autocvar_touch_debug)
		Touch_DrawGlassCircle(autocvar_touch_weapon_x, autocvar_touch_weapon_y, autocvar_touch_weapon_size, "WEP", Touch_WeaponWheelOpen(), overlayAlpha, TOUCH_GLASS_TINT);
	if (autocvar_touch_zoom_visible)
		Touch_DrawGlassCircle(autocvar_touch_zoom_x, autocvar_touch_zoom_y, autocvar_touch_zoom_size, "ZOOM", false, overlayAlpha, TOUCH_GLASS_TINT);
	if (autocvar_touch_reload_visible)
		Touch_DrawGlassCircle(autocvar_touch_reload_x, autocvar_touch_reload_y, autocvar_touch_reload_size, "R", false, overlayAlpha, TOUCH_GLASS_TINT);
	if (autocvar_touch_dodge_visible)
		Touch_DrawGlassCircle(autocvar_touch_dodge_x, autocvar_touch_dodge_y, autocvar_touch_dodge_size, "DODGE", false, overlayAlpha, TOUCH_GLASS_TINT);

	Touch_DrawWeaponWheel(overlayAlpha);
	// Probes only at touch_debug >= 2 (keep mid-game HUD clean).
	if (autocvar_touch_debug >= 2)
		Touch_DrawDebugProbes();
	Touch_DrawDebugLine();
}

void Touch_Draw(void)
{
	if (!Touch_Active())
		return;

	// HUD panels may leave a clip rect / dynamic scale armed — clear before glass.
	drawresetcliparea();
	HUD_Scale_Disable();

	float overlayAlpha = bound(0.3, autocvar_touch_opacity, 1.0);

	if (Touch_CustomizeActive())
	{
		Touch_DrawOverlay(overlayAlpha * 0.45);
		Touch_Customize_Draw();
		return;
	}

	if (autocvar_touch_mobile_hud && autocvar_touch_glass_quality >= 1)
		Touch_Hud_Draw(overlayAlpha);
	Touch_DrawOverlay(overlayAlpha);
}
