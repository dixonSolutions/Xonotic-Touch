// SPDX-License-Identifier: GPL-3.0-or-later
#include "touch_uib_touch.qh"

// Godnotic UI bridge: Settings > Controls > Touch controls. How the in-match
// touch controls feel: the look (swipe) sensitivity and its acceleration, the
// move stick, the buttons, the layout. Every row is one cvar the two HUD
// copies read (docs/CONTROLS.md "Touch controls settings"); the stock-menu
// fallback with the same rows is dialog_touch_controls.qc.
//
// Hooks in touch_uib_settings.qc: UIB_SecTouchControls() as the first sub-tab
// of UIB_SecTouch, UIB_TouchControlsAction() in UIB_SettingsAction.

void UIB_SecTouchControls()
{
	if (UIB_SubTab("feel", _("Touch controls")))
	{
		UIB_Section("look", _("Look"), false);
			UIB_Slider("sens", _("Swipe sensitivity"), "cvar:touch_sens_base", 1, 8, 0.1, "x",
				_("How far the view turns for a swipe. The same swipe turns the same angle on every screen size"));
			UIB_Slider("sensy", _("Up and down, compared with sideways"), "cvar:touch_sens_y_mult", 0.4, 1.4, 0.05, "x", "");
			UIB_Choice("accel", _("Faster for quick swipes"), "cvar:touch_look_accel", "segmented",
				UIB_Opts3("0", _("Off"), "0.8", _("On"), "1.4", _("Strong")),
				_("A slow drag aims exactly; a quick swipe turns further, enough to turn around in one"));
			UIB_Switch("invert", _("Invert up and down"), "cvar:touch_invert_y", "");
		UIB_Close();
		UIB_Section("stick", _("Move stick"), false);
			UIB_Slider("sticksize", _("Size"), "cvar:touch_move_size", 0.2, 0.45, 0.01, "x", "");
			UIB_Switch("float", _("Appears under your thumb"), "cvar:touch_stick_float",
				_("The stick starts wherever your left thumb lands and follows it; off, it stays where it is drawn"));
			UIB_Slider("deadzone", _("Dead zone"), "cvar:touch_stick_deadzone", 0, 0.35, 0.01, "percent",
				_("How far the thumb moves before you do"));
			UIB_Switch("analog", _("Move at any angle"), "cvar:touch_stick_analog",
				_("Off: eight directions, like keys"));
		UIB_Close();
		UIB_Section("buttons", _("Buttons"), false);
			UIB_Slider("size", _("Size"), "cvar:touch_scale", 0.7, 1.4, 0.05, "x", "");
			UIB_Slider("opacity", _("Opacity"), "cvar:touch_opacity", 0.3, 1, 0.05, "percent", "");
			UIB_Switch("firedrag", _("Aim while firing"), "cvar:touch_fire_drag_look",
				_("Drag your thumb off FIRE to turn while it keeps firing"));
			UIB_Switch("buttondrag", _("Aim from HOP, DUCK and ALT too"), "cvar:touch_button_drag_look", "");
			UIB_Choice("hop", _("HOP"), "cvar:touch_hop_mode", "segmented",
				UIB_Opts3("3", _("Hold, and a KEEP switch"), "0", _("Hold"), "2", _("Jumps while you move")),
				_("Holding HOP keeps jumping, which is how you gain speed"));
			UIB_Switch("assist", _("Aim assist"), "cvar:cl_touch_aimassist",
				_("In matches on this device, and on servers that allow it"));
			if (cvar("_touch_haptics_available"))
				UIB_Switch("haptics", _("Vibrate on button presses"), "cvar:touch_haptics", "");
		UIB_Close();
		UIB_Section("layout", _("Layout"), false);
			UIB_Choice("preset", _("Preset"), "cvar:touch_preset", "segmented", strcat("[", UIB_Opt("standard", _("Standard")), ",", UIB_Opt("casual", _("Casual")), ",",
				strcat(UIB_Opt("competitive", _("Competitive")), ",", UIB_Opt("minimal", _("Minimal")), "]")), "");
			UIB_Switch("hand", _("Left-handed"), "cvar:touch_handedness", _("Mirrors the layout: move on the right, aim on the left"));
			UIB_Button("edit", _("Edit layout"), UIB_A("touch.edit", _("Edit layout"), "edit"), "secondary");
			UIB_Open("reset", "button");
				uib_s("label", _("Reset to defaults"));
				uib_s("style", "destructive");
				uib_j("action", UIB_AC("touch.reset", _("Reset to defaults"), "refresh", "destructive", _("Put every touch control setting back to its default?"), _("Reset"), true));
			UIB_Close();
		UIB_Close();
	}
	UIB_Close();
}

bool UIB_TouchControlsAction(string a)
{
	switch (a)
	{
		case "touch.edit":
			// the editor is in the match's HUD
			if (!(gamestatus & (GAME_CONNECTED | GAME_ISSERVER)))
			{
				uib_toast(_("Start a match first: the layout is edited on the real controls"), "info", "info", 4);
				return true;
			}
			localcmd("\ntouch_customize 1\n");
			m_hide();
			return true;
		case "touch.reset":
			// every default (touch_defaults.cfg is generated from the settings table), then the standard preset
			localcmd("\nexec touch_defaults.cfg\nexec touch/profiles/standard.cfg\nexec touch/profiles/helpers.cfg\ntouch_topbar_auto 1\nmenu_cmd sync\n");
			uib_toast(_("Touch controls reset"), "success", "check", 2);
			UIB_RebuildAll();
			return true;
	}
	return false;
}
