#include "touch_draw.qh"
#include "touch_customize.qh"
#include "touch_hud.qh"

void Touch_DrawLabel(vector center, float size, string label, float theAlpha)
{
	vector sz = '1 1 0' * size * 0.35;
	drawstring_builtin(center - eX * (strlen(label) * size * 0.08) - eY * (size * 0.12), label, sz, TOUCH_GLASS_HIGHLIGHT, theAlpha, 0);
}

void Touch_DrawActionButton(float nx, float ny, float nsize, string label, bool pressed, float theAlpha)
{
	if (!Touch_WidgetVisible(nx, ny))
		return;
	vector c = Touch_WidgetCenter(nx, ny, nsize);
	float r = Touch_WidgetRadius(nsize);
	Touch_Glass_DrawRoundedButton(c, r * 2, TOUCH_GLASS_TINT, theAlpha, pressed);
	Touch_DrawLabel(c, r * 2, label, theAlpha * 0.9);
}

void Touch_DrawWeaponWheel(float theAlpha)
{
	if (!Touch_WeaponWheelOpen())
		return;
	vector vf = getpropertyvec(VF_SIZE);
	vector center = vf.x * 0.5 * eX + vf.y * 0.45 * eY;
	float radius = min(vf.x, vf.y) * 0.22;
	Touch_Glass_DrawCircle(center, radius, TOUCH_GLASS_TINT, theAlpha * 0.55);
	Touch_Glass_DrawCircleOutline(center, radius, TOUCH_GLASS_HIGHLIGHT, theAlpha * 0.4, 2);
	// eight weapon segments (labels simplified)
	int i;
	for (i = 0; i < 8; ++i)
	{
		float a = (i / 8.0) * 360 - 90;
		vector dir = '0 0 0';
		string label = itos(i + 1);
		dir.x = cos(a * DEG2RAD);
		dir.y = sin(a * DEG2RAD);
		vector pos = center + dir * (radius * 0.65);
		Touch_Glass_DrawRoundedButton(pos, radius * 0.22, TOUCH_GLASS_ACCENT, theAlpha, false);
		Touch_DrawLabel(pos, radius * 0.22, label, theAlpha);
	}
}

void Touch_DrawOverlay(float overlayAlpha)
{
	// Move stick — OpenGL glass joystick
	vector move_c = Touch_WidgetCenter(autocvar_touch_move_x, autocvar_touch_move_y, autocvar_touch_move_size);
	float move_r = Touch_WidgetRadius(autocvar_touch_move_size);
	Touch_Glass_DrawJoystickBase(move_c, move_r, overlayAlpha);
	vector knob_off = Touch_MoveStickOffset() * move_r * 0.55;
	bool move_active = vlen(Touch_MoveStickOffset()) > autocvar_touch_stick_deadzone;
	Touch_Glass_DrawJoystickKnob(move_c, move_r, knob_off, overlayAlpha, move_active);

	// Action buttons
	Touch_DrawActionButton(autocvar_touch_fire_x, autocvar_touch_fire_y, autocvar_touch_fire_size, "FIRE", Touch_FireHeld(), overlayAlpha);
	Touch_DrawActionButton(autocvar_touch_jump_x, autocvar_touch_jump_y, autocvar_touch_jump_size, "JUMP", false, overlayAlpha);
	Touch_DrawActionButton(autocvar_touch_crouch_x, autocvar_touch_crouch_y, autocvar_touch_crouch_size, "CR", false, overlayAlpha);
	Touch_DrawActionButton(autocvar_touch_weapon_x, autocvar_touch_weapon_y, autocvar_touch_weapon_size, "WEP", Touch_WeaponWheelOpen(), overlayAlpha);
	Touch_DrawActionButton(autocvar_touch_zoom_x, autocvar_touch_zoom_y, autocvar_touch_zoom_size, "ZOOM", false, overlayAlpha);
	Touch_DrawActionButton(autocvar_touch_reload_x, autocvar_touch_reload_y, autocvar_touch_reload_size, "R", false, overlayAlpha);
	if (autocvar_touch_dodge_visible)
		Touch_DrawActionButton(autocvar_touch_dodge_x, autocvar_touch_dodge_y, autocvar_touch_dodge_size, "DODGE", false, overlayAlpha);

	Touch_DrawWeaponWheel(overlayAlpha);
}

void Touch_Draw(void)
{
	if (!Touch_Active())
		return;

	float overlayAlpha = bound(0.3, autocvar_touch_opacity, 1.0);

	if (Touch_CustomizeActive())
	{
		Touch_DrawOverlay(overlayAlpha * 0.45);
		Touch_Customize_Draw();
		return;
	}

	Touch_Hud_Draw(overlayAlpha);
	Touch_DrawOverlay(overlayAlpha);
}
