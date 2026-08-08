#include "touch_draw.qh"
#include "touch_customize.qh"
#include "touch_hud.qh"
#include "touch_glass.qh"
#include "touch_shape.qh"
#include "touch_theme.qh"
#include "touch_widget.qh"
#include "touch_console.qh"
#include "touch_pause.qh"
#include "touch_init.qh"
#include "touch_input.qh"
#include "touch_defs.qh"

// Pressed and latched visuals cannot be reached from a screenshot harness: a
// virtual uinput touchscreen is not routed to the game window, so the only way
// to review them is to ask for them. touch_debug 3 forces every control into its
// active state.
bool Touch_PreviewStates(void)
{
	return autocvar_touch_debug >= 3;
}

void Touch_DrawCircleWidget(float nx, float ny, float nsize, string label, bool pressed,
	float theAlpha, vector accent)
{
	Touch_Widget_Button(Touch_WidgetCenter(nx, ny, nsize), Touch_WidgetRadius(nsize),
		label, pressed || Touch_PreviewStates(), theAlpha, accent);
}

void Touch_DrawHopBar(float theAlpha)
{
	vector c = Touch_WidgetCenter(autocvar_touch_jump_x, autocvar_touch_jump_y, autocvar_touch_jump_size);
	vector half = Touch_WidgetHalfExtents(autocvar_touch_jump_size, autocvar_touch_jump_aspect);
	Touch_Widget_Capsule(c, half, "HOP", Touch_JumpHeld(),
		Touch_HopLatched() || Touch_PreviewStates(), theAlpha, TOUCH_C_ACCENT);
}

// Concentric rings are the universal joystick idiom (Jakob's law), so the stick
// carries no label — the shape already says what it is, and a word under the
// thumb would only be clutter.
void Touch_DrawMoveStick(float theAlpha)
{
	vector base = Touch_WidgetCenter(autocvar_touch_move_x, autocvar_touch_move_y, autocvar_touch_move_size);
	float r = Touch_WidgetRadius(autocvar_touch_move_size);
	if (r < 4)
		return;

	vector off = Touch_MoveStickOffset();
	if (Touch_PreviewStates())
		off = '-0.55 -0.62 0';
	bool active = vlen(off) > autocvar_touch_stick_deadzone;
	float knob_r = r * 0.34;
	vector knob_c = base + off * (r - knob_r);

	if (!Touch_Shape_Ready())
	{
		Touch_Glass_DrawJoystickBase(base, r, theAlpha);
		Touch_Glass_DrawJoystickKnob(base, r, off * (r * 0.55), theAlpha, active);
		return;
	}

	// The base stays quieter than the buttons: it is the largest object on
	// screen, and at button opacity it would dominate the view.
	Touch_Shape_Shadow(base + eY * Touch_Widget_Elevation(r), r * 1.06, theAlpha * TOUCH_A_SHADOW * 0.6);
	Touch_Shape_Disc(base, r, TOUCH_C_SURFACE, theAlpha * TOUCH_A_SURFACE * 0.75);
	Touch_Widget_Rim(base, r, TOUCH_C_RIM, theAlpha * TOUCH_A_RIM * 0.85);

	vector knob_rgb = TOUCH_C_RIM;
	float knob_a = TOUCH_A_SURFACE_ACTIVE;
	if (active)
	{
		knob_rgb = TOUCH_C_ACCENT_LIGHT;
		knob_a = TOUCH_A_RIM_ACTIVE;
		Touch_Shape_Glow(knob_c, knob_r * 1.6, TOUCH_C_ACCENT, theAlpha * TOUCH_A_GLOW);
	}
	Touch_Shape_Shadow(knob_c + eY * Touch_Widget_Elevation(knob_r), knob_r * 1.20, theAlpha * TOUCH_A_SHADOW);
	Touch_Shape_Disc(knob_c, knob_r, knob_rgb, theAlpha * knob_a);
	Touch_Widget_Rim(knob_c, knob_r, TOUCH_C_RIM, theAlpha * TOUCH_A_RIM_ACTIVE);
}

void Touch_DrawFireButton(float theAlpha)
{
	vector c = Touch_WidgetCenter(autocvar_touch_fire_x, autocvar_touch_fire_y, autocvar_touch_fire_size);
	float r = Touch_WidgetRadius(autocvar_touch_fire_size);
	bool held = Touch_FireHeld() || Touch_PreviewStates();
	Touch_Widget_Button(c, r, "FIRE", held, theAlpha, TOUCH_C_DANGER);

	// While a drag off FIRE is steering the view, echo the reticle idea on the
	// button so the merged gesture is discoverable instead of a hidden trick.
	if (held && Touch_Shape_Ready() && (Touch_Look_Escaped() || Touch_PreviewStates()))
		Touch_Shape_Ring(c, r * 0.55, TOUCH_W_RING_THIN, TOUCH_C_INK, theAlpha * TOUCH_A_GHOST * 2);
}

void Touch_DrawWeaponWheel(float theAlpha)
{
	if (!Touch_WeaponWheelOpen())
		return;
	vector ui = Touch_UISize();
	vector center = ui.x * 0.5 * eX + ui.y * 0.45 * eY;
	float radius = min(ui.x, ui.y) * 0.22;
	if (!Touch_Shape_Ready())
	{
		if (autocvar_touch_glass_quality < 2)
			return;
		Touch_Glass_DrawCircle(center, radius, TOUCH_C_SURFACE, theAlpha * 0.55);
		Touch_Glass_DrawCircleOutline(center, radius, TOUCH_C_RIM, theAlpha * 0.4, 2);
		return;
	}
	Touch_Shape_Shadow(center + eY * Touch_Widget_Elevation(radius), radius * 1.06, theAlpha * TOUCH_A_SHADOW);
	Touch_Shape_Disc(center, radius, TOUCH_C_SURFACE, theAlpha * TOUCH_A_SURFACE_ACTIVE);
	Touch_Widget_Rim(center, radius, TOUCH_C_RIM, theAlpha * TOUCH_A_RIM);
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
	line = strcat(line, strcat(" mm/r ", ftos(floor(Touch_UnitsToMM(Touch_WidgetRadius(autocvar_touch_fire_size)) * 10) / 10)));
	line = strcat(line, strcat(" shape ", Touch_Shape_Ready() ? "tga" : "flat"));
	drawstring_builtin('8 8 0', line, '1 1 0' * 10, TOUCH_C_INK, 0.85, 0);
}

void Touch_DrawOverlay(float overlayAlpha)
{
	// Edit mode owns the top bar for its SAVE/CANCEL/RESET row, and none of the
	// pills do anything while editing, so they would only collide with it.
	if (!Touch_CustomizeActive())
	{
		Touch_Pause_DrawPill(overlayAlpha);
		Touch_Chat_DrawPill(overlayAlpha);
		Touch_Console_Draw(overlayAlpha);
		Touch_Scores_DrawPill(overlayAlpha);
	}

	if (autocvar_touch_move_visible || autocvar_touch_debug)
		Touch_DrawMoveStick(overlayAlpha);

	if (autocvar_touch_fire_visible || autocvar_touch_debug)
		Touch_DrawFireButton(overlayAlpha);

	if (autocvar_touch_fire2_visible)
		Touch_DrawCircleWidget(autocvar_touch_fire2_x, autocvar_touch_fire2_y,
			autocvar_touch_fire2_size, "ALT", Touch_Fire2Held(), overlayAlpha, TOUCH_C_DANGER);

	if (autocvar_touch_jump_visible || autocvar_touch_debug)
		Touch_DrawHopBar(overlayAlpha);

	if (autocvar_touch_crouch_visible || autocvar_touch_debug)
		Touch_DrawCircleWidget(autocvar_touch_crouch_x, autocvar_touch_crouch_y,
			autocvar_touch_crouch_size, "DUCK", Touch_CrouchHeld(), overlayAlpha, TOUCH_C_ACCENT);
	if (autocvar_touch_weapon_visible || autocvar_touch_debug)
		Touch_DrawCircleWidget(autocvar_touch_weapon_x, autocvar_touch_weapon_y,
			autocvar_touch_weapon_size, "WEP", Touch_WeaponWheelOpen(), overlayAlpha, TOUCH_C_ACCENT);
	if (autocvar_touch_zoom_visible)
		Touch_DrawCircleWidget(autocvar_touch_zoom_x, autocvar_touch_zoom_y,
			autocvar_touch_zoom_size, "ZOOM", false, overlayAlpha, TOUCH_C_ACCENT);
	if (autocvar_touch_reload_visible)
		Touch_DrawCircleWidget(autocvar_touch_reload_x, autocvar_touch_reload_y,
			autocvar_touch_reload_size, "RLD", false, overlayAlpha, TOUCH_C_ACCENT);
	if (autocvar_touch_dodge_visible)
		Touch_DrawCircleWidget(autocvar_touch_dodge_x, autocvar_touch_dodge_y,
			autocvar_touch_dodge_size, "DASH", false, overlayAlpha, TOUCH_C_ACCENT);

	Touch_DrawWeaponWheel(overlayAlpha);
	// Probes are level 2 exactly: they are opaque blocks over the controls, so at
	// level 3 they would hide the very states that level is for.
	if (autocvar_touch_debug == 2)
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

	// The chat sheet is modal: drawing the controls under it would invite the
	// player to press something that is not listening.
	if (Touch_Chat_Active())
	{
		Touch_Chat_Draw();
		return;
	}

	if (autocvar_touch_mobile_hud && autocvar_touch_glass_quality >= 1)
		Touch_Hud_Draw(overlayAlpha);
	Touch_DrawOverlay(overlayAlpha);
}
