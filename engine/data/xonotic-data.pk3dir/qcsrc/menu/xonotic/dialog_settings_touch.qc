#include "dialog_settings_touch.qh"

#include "touch_menu_layout.qh"
#include "textlabel.qh"
#include "slider.qh"
#include "checkbox.qh"
#include "commandbutton.qh"
#include "button.qh"
#include "mixedslider.qh"
#include "inputbox.qh"
#include "dialog_settings_input.qh"
#include "tabcontroller.qh"

void TouchMenu_SaveLayout_Click(entity me, entity other)
{
	TouchMenu_WriteLayoutFile("touch.layout.cfg");
	localcmd("saveconfig\n");
}

void TouchMenu_Export_Click(entity me, entity other)
{
	TouchMenu_WriteLayoutFile("touch_export.cfg");
}

void TouchMenu_Import_Click(entity me, entity other)
{
	TouchMenu_WriteLayoutFile("touch.layout.bak");
	// fopen saved it one data/ deeper than exec looks; see GameData.java.
	localcmd("exec data/touch.layout.cfg\nmenu_cmd sync\n");
}

void TouchMenu_ResetPreset_Click(entity me, entity other)
{
	localcmd(strcat("exec touch/profiles/", cvar_string("touch_preset"), ".cfg\n"));
	// The engine shoot helpers are not in the CSQC table the preset resets to.
	if (cvar_string("touch_preset") != "competitive")
		localcmd("exec touch/profiles/helpers.cfg\n");
	localcmd("menu_cmd sync\n");
}

// Settings -> Mobile. Four sub-tabs; every control below is one cvar, and each
// cvar is on exactly one sub-tab.
//   Controls    input mode, layout, size and look of the controls, sticks and buttons
//   Aim         look speed, aim assist and its strength
//   Auto-shoot  auto-shoot, its timing and its conditions
//   Dynamics    performance profile, frame rate, dynamic resolution, hop
entity makeXonoticTouchSettingsTab()
{
	entity me = NEW(XonoticTouchSettingsTab);
	me.configureDialog(me);
	return me;
}

void XonoticTouchSettingsTab_fill(entity me)
{
	// One row of sub-tab buttons, then the sub-tab itself: 15.5 = 1.5 + 14.
	entity mc = makeXonoticTabController(me.rows - 1.5);
	me.TR(me);
		me.TD(me, 1, 1.5, mc.makeTabButton(mc, _("Controls"),   makeXonoticMobileControlsTab()));
		me.TD(me, 1, 1.5, mc.makeTabButton(mc, _("Aim"),        makeXonoticMobileAimTab()));
		me.TD(me, 1, 1.5, mc.makeTabButton(mc, _("Auto-shoot"), makeXonoticMobileAutoShootTab()));
		me.TD(me, 1, 1.5, mc.makeTabButton(mc, _("Dynamics"),   makeXonoticMobileDynamicsTab()));
	me.gotoRC(me, 1.5, 0);
		me.TD(me, me.rows - 1.5, me.columns, mc);
}

entity makeXonoticMobileControlsTab()
{
	entity me = NEW(XonoticMobileControlsTab);
	me.configureDialog(me);
	return me;
}

void XonoticMobileControlsTab_fill(entity me)
{
	entity e;

	// Dynamic shows the on-screen controls on a touchscreen with no physical
	// keyboard, from what the system lists (vid_touchdetect.c); input never
	// switches them. Touch and the mouse work in every mode -- the mode only
	// decides whether the controls are drawn. Ctrl+Alt+1 / Ctrl+Alt+2
	// override any of them until the game restarts.
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Input mode")));
	me.TR(me);
		me.TD(me, 1, 2, e = makeXonoticRadioButton_T(1, "vid_touchscreen_mode", "1", _("Dynamic"),
			_("Show the on-screen controls on a touchscreen, and hide them while a keyboard is attached. Touch and the mouse keep working either way")));
		me.TD(me, 1, 2, e = makeXonoticRadioButton_T(1, "vid_touchscreen_mode", "2", _("Touch"),
			_("Always show the on-screen touch controls, with no detection")));
		me.TD(me, 1, 2, e = makeXonoticRadioButton_T(1, "vid_touchscreen_mode", "0", _("Keyboard & mouse"),
			_("Never show the on-screen touch controls. Fingers still press menus and turn the view")));
	me.TR(me);
		me.TD(me, 1, 2, e = makeXonoticCheckBox_T(0, "vid_touchscreen_live", _("Switch mid-game"),
			_("Dynamic: show or hide the controls when a keyboard is plugged in or taken away. Off: decide once at launch and keep it until the game restarts")));
			setDependent(e, "vid_touchscreen_mode", 1, 1);
		me.TD(me, 1, 2, e = makeXonoticCheckBox_T(0, "vid_touchscreen_notify", _("Announce switches"),
			_("Show a message when the controls switch by themselves. Off: switch silently. Ctrl+Alt+1 and Ctrl+Alt+2 always say what they did")));
			setDependent(e, "vid_touchscreen_mode", 1, 1);
		me.TD(me, 1, 2, e = makeXonoticCheckBox_T(0, "vid_touchscreen_touchonly", _("Touch-only devices"),
			_("Dynamic: hide the controls while a physical keyboard is attached. Off: show them on any touchscreen, keyboard or not")));
			setDependent(e, "vid_touchscreen_mode", 1, 1);
	me.TR(me);
		me.TD(me, 1, 4, e = makeXonoticTextLabel(0, _("Ctrl+Alt+1: switch and lock    Ctrl+Alt+2: lock / unlock")));
		me.TD(me, 1, 2, e = makeXonoticCommandButton(_("Rescan hardware"), '0 0 0', "vid_touchscreen_rescan; menu_cmd sync", 0));

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Layout preset")));
	me.TR(me);
		me.TD(me, 1, 1.5, e = makeXonoticCommandButton(_("Standard"), '0 0 0', "exec touch/profiles/standard.cfg; exec touch/profiles/helpers.cfg; menu_cmd sync", 0));
		me.TD(me, 1, 1.5, e = makeXonoticCommandButton(_("Casual"), '0 0 0', "exec touch/profiles/casual.cfg; menu_cmd sync", 0));
		me.TD(me, 1, 1.5, e = makeXonoticCommandButton(_("Competitive"), '0 0 0', "exec touch/profiles/competitive.cfg; menu_cmd sync", 0));
		me.TD(me, 1, 1.5, e = makeXonoticCommandButton(_("Left hand"), '0 0 0', "exec touch/profiles/left.cfg; exec touch/profiles/helpers.cfg; menu_cmd sync", 0));

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Size and look")));
	// Three sliders to a row keep the tab short. Each slider's value is drawn
	// at its right end, so a gap after it keeps "0.90" off the next label.
	me.TR(me);
		me.TD(me, 1, 0.8, e = makeXonoticTextLabel(0, _("Opacity:")));
		me.TD(me, 1, 1.05, e = makeXonoticSlider_T(0.3, 1.0, 0.05, "touch_opacity", _("Control overlay opacity")));
		me.TDempty(me, 0.15);
		me.TD(me, 1, 0.8, e = makeXonoticTextLabel(0, _("Scale:")));
		me.TD(me, 1, 1.05, e = makeXonoticSlider_T(0.7, 1.4, 0.05, "touch_scale", _("Size of move/fire/jump widgets")));
		me.TDempty(me, 0.15);
		me.TD(me, 1, 0.8, e = makeXonoticTextLabel(0, _("Glass tint:")));
		me.TD(me, 1, 1.2, e = makeXonoticSlider_T(0, 1, 0.05, "touch_glass_hollow",
			_("How much of each control is glass. Left: tinted glass. All the way right: hollow, only the outline and label, nothing between you and the game")));
	me.TR(me);
		me.TD(me, 1, 0.9, e = makeXonoticTextLabel(0, _("HUD size:")));
		me.TD(me, 1, 2.1, e = makeXonoticSlider_T(0.6, 1.4, 0.05, "touch_hud_scale", _("Size of the health, ammo and score readouts")));
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "touch_handedness", _("Left-handed"),
			_("Mirror the layout: the move stick on the right, aim and buttons on the left")));

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Sticks and buttons")));
	me.TR(me);
		me.TD(me, 1, 1.2, e = makeXonoticTextLabel(0, _("Dead zone:")));
		me.TD(me, 1, 1.8, e = makeXonoticSlider_T(0, 0.5, 0.02, "touch_stick_deadzone", _("How far the move stick must be pushed before the character moves")));
		me.TD(me, 1, 1.2, e = makeXonoticTextLabel(0, _("Reach:")));
		me.TD(me, 1, 1.8, e = makeXonoticSlider_T(0.6, 1.5, 0.05, "touch_stick_range", _("How easily the move stick reaches full speed. Higher: full speed with a shorter push")));
	me.TR(me);
		me.TD(me, 1, 1.2, e = makeXonoticTextLabel(0, _("FIRE button:")));
		me.TD(me, 1, 1.2, e = makeXonoticRadioButton_T(2, "touch_fire_mode", "0", _("Hold"),
			_("Fire while the FIRE button is held down")));
		me.TD(me, 1, 1.2, e = makeXonoticRadioButton_T(2, "touch_fire_mode", "1", _("Toggle"),
			_("Tap FIRE to start firing and tap again to stop")));
		me.TD(me, 1, 2.4, e = makeXonoticCheckBox_T(0, "touch_dodge_visible", _("Dodge button"),
			_("Show a dodge button next to the move stick")));

	me.TR(me);
		me.TD(me, 1, 2, e = makeXonoticCommandButton(_("Customize layout"), '0 0 0', "touch_customize 1; togglemenu 0", COMMANDBUTTON_CLOSE));
		me.TD(me, 1, 2, e = makeXonoticButton(_("Save layout"), '0 0 0'));
			e.onClick = TouchMenu_SaveLayout_Click;
		me.TD(me, 1, 2, e = makeXonoticButton(_("Reset preset"), '0 0 0'));
			e.onClick = TouchMenu_ResetPreset_Click;
	me.TR(me);
		me.TD(me, 1, 6, e = makeXonoticCommandButton(_("Apply & refresh"), '0 0 0', "menu_cmd sync", COMMANDBUTTON_APPLY));
}

entity makeXonoticMobileAimTab()
{
	entity me = NEW(XonoticMobileAimTab);
	me.configureDialog(me);
	return me;
}

void XonoticMobileAimTab_fill(entity me)
{
	entity e;
	entity touchSensSlider;

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Look")));
	// Its own cvar, not the mouse's `sensitivity`: Settings -> Input sets that
	// one, and with a mouse and a touchscreen both in use neither moves the other.
	// Same 0.1-50 range as the mouse, with a box for values the slider is too
	// coarse to hit.
	me.TR(me);
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Sensitivity:")));
		me.TD(me, 1, 3, e = makeXonoticSlider_T(0.1, 50, 0.1, "touch_sens_base", _("Look speed (DPI-normalized)")));
			e.setValueSpace(e, 0);
			touchSensSlider = e;
		me.TD(me, 1, 1.5, e = makeXonoticInputBox_T(0, "touch_sens_base", _("Look speed (DPI-normalized)")));
			e.saveImmediately = true;
			e.allowedCharacters = "0.123456789";
			linkSensitivities(touchSensSlider, e);
	me.TR(me);
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Vertical:")));
		me.TD(me, 1, 1.5, e = makeXonoticSlider_T(0.5, 1.5, 0.05, "touch_sens_y_mult", _("Pitch vs yaw multiplier")));
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "touch_invert_y", _("Invert look Y"), string_null));

	// Engine-side (touch_aim.c), so it works on public servers too. Touch mode
	// only: with a keyboard attached none of this runs, whatever is set here.
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Aim assist")));
	me.TR(me);
		me.TD(me, 1, 2, e = makeXonoticCheckBox_T(0, "cl_touch_aimassist", _("Aim assist"),
			_("Aim slows down over an enemy, and is pulled toward one while you turn or move with a finger on the aim area")));
		me.TD(me, 1, 1, e = makeXonoticCommandButton(_("Light"), '0 0 0',
			"cl_touch_aimassist 1; cl_touch_aimassist_friction 0.25; cl_touch_aimassist_strength 0.15; cl_touch_aimassist_maxrate 20; cl_touch_aimassist_cone 6; menu_cmd sync", 0));
		me.TD(me, 1, 1, e = makeXonoticCommandButton(_("Medium"), '0 0 0',
			"cl_touch_aimassist 1; cl_touch_aimassist_friction 0.45; cl_touch_aimassist_strength 0.35; cl_touch_aimassist_maxrate 40; cl_touch_aimassist_cone 8; menu_cmd sync", 0));
		me.TD(me, 1, 1, e = makeXonoticCommandButton(_("Strong"), '0 0 0',
			"cl_touch_aimassist 1; cl_touch_aimassist_friction 0.6; cl_touch_aimassist_strength 0.55; cl_touch_aimassist_maxrate 60; cl_touch_aimassist_cone 11; menu_cmd sync", 0));
		me.TD(me, 1, 1, e = makeXonoticCommandButton(_("Off"), '0 0 0', "cl_touch_aimassist 0; menu_cmd sync", 0));
	me.TR(me);
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Slowdown:")));
			setDependent(e, "cl_touch_aimassist", 1, 1);
		me.TD(me, 1, 1.5, e = makeXonoticSlider_T(0, 0.8, 0.05, "cl_touch_aimassist_friction", _("How much aiming slows down over an enemy (0 = off)")));
			setDependent(e, "cl_touch_aimassist", 1, 1);
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Pull:")));
			setDependent(e, "cl_touch_aimassist", 1, 1);
		me.TD(me, 1, 1.5, e = makeXonoticSlider_T(0, 0.6, 0.05, "cl_touch_aimassist_strength", _("How strongly the view is pulled toward a nearby enemy while you turn or move (0 = off)")));
			setDependent(e, "cl_touch_aimassist", 1, 1);
	me.TR(me);
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Catch cone:")));
			setDependent(e, "cl_touch_aimassist", 1, 1);
		me.TD(me, 1, 1.5, e = makeXonoticSlider_T(2, 12, 1, "cl_touch_aimassist_cone", _("How many degrees from the crosshair an enemy still counts as near")));
			setDependent(e, "cl_touch_aimassist", 1, 1);
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Max pull:")));
			setDependent(e, "cl_touch_aimassist", 1, 1);
		me.TD(me, 1, 1.5, e = makeXonoticSlider_T(5, 60, 5, "cl_touch_aimassist_maxrate", _("The fastest the pull can turn the view, in degrees per second. It never snaps")));
			setDependent(e, "cl_touch_aimassist", 1, 1);
	me.TR(me);
		me.TD(me, 1, 6, e = makeXonoticCheckBox_T(0, "cl_touch_aim_ring", _("Show target ring"),
			_("Draw a ring at the crosshair while an enemy is near (accent colour) or under it (red), so you can see the helpers working")));
	me.TR(me);
		me.TD(me, 1, 6, e = makeXonoticTextLabel(0, _("Never aims at teammates or through walls.")));
	me.TR(me);
		me.TD(me, 1, 6, e = makeXonoticTextLabel(0, _("Touch mode only: off while a keyboard or mouse is playing.")));
}

entity makeXonoticMobileAutoShootTab()
{
	entity me = NEW(XonoticMobileAutoShootTab);
	me.configureDialog(me);
	return me;
}

void XonoticMobileAutoShootTab_fill(entity me)
{
	entity e;

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Auto-shoot")));
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticCheckBox_T(0, "cl_touch_autoshoot", _("Fire automatically at enemies"),
			_("Fire while the crosshair is on an enemy. Never fires at teammates, and FIRE still works by hand")));
	me.TR(me);
		me.TD(me, 1, 2, e = makeXonoticTextLabel(0, _("Reaction delay:")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);
		me.TD(me, 1, 4, e = makeXonoticSlider_T(0.05, 0.5, 0.05, "cl_touch_autoshoot_delay",
			_("How long the crosshair must stay on an enemy before firing starts, in seconds")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);
	me.TR(me);
		me.TD(me, 1, 2, e = makeXonoticTextLabel(0, _("Keep firing:")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);
		me.TD(me, 1, 4, e = makeXonoticSlider_T(0, 0.5, 0.05, "cl_touch_autoshoot_release",
			_("How long firing continues after the crosshair slips off, in seconds")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Which weapons")));
	me.TR(me);
		me.TD(me, 1, 2, e = makeXonoticRadioButton_T(4, "cl_touch_autoshoot_weapons", "0", _("Every weapon"),
			_("Auto-shoot with whatever you hold")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);
		me.TD(me, 1, 4, e = makeXonoticRadioButton_T(4, "cl_touch_autoshoot_weapons", "1", _("Instant-hit weapons only"),
			_("Shotgun, machine gun, vortex, rifle and the like. Projectile weapons then wait for the FIRE button, so rockets are not wasted on a long shot. Needs a game that reports the weapon in hand")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Conditions")));
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticCheckBox_T(0, "cl_touch_autoshoot_aimonly", _("Only while a finger is on the aim area or FIRE"),
			_("Off: auto-shoot also fires with no finger down, as soon as an enemy walks into the crosshair")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);
	me.TR(me);
		me.TD(me, 1, 2, e = makeXonoticTextLabel(0, _("Maximum range:")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);
		me.TD(me, 1, 4, e = makeXonoticSlider_T(500, 6000, 250, "cl_touch_aim_range",
			_("Enemies farther away than this many units are ignored by auto-shoot and aim assist")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);
	me.TR(me);
		me.TD(me, 1, 6, e = makeXonoticTextLabel(0, _("Fires only at a visible enemy in line of sight: never at teammates,")));
	me.TR(me);
		me.TD(me, 1, 6, e = makeXonoticTextLabel(0, _("invisible players, spectators, or through walls.")));
	me.TR(me);
		me.TD(me, 1, 6, e = makeXonoticTextLabel(0, _("Touch mode only: off while a keyboard or mouse is playing.")));
}

entity makeXonoticMobileDynamicsTab()
{
	entity me = NEW(XonoticMobileDynamicsTab);
	me.configureDialog(me);
	return me;
}

void XonoticMobileDynamicsTab_fill(entity me)
{
	entity e;

	// Each profile button runs its profile once; the values are archived from
	// then on, so a change below or in Settings -> Effects afterwards sticks
	// (the launchers apply the default profile only on the first start,
	// touch_perf_version).
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Performance profile")));
	me.TR(me);
		me.TD(me, 1, 2, e = makeXonoticCommandButton_T(_("30 fps saver"), '0 0 0', "exec touch/profiles/battery.cfg; menu_cmd sync", 0,
			_("Performance: 30 fps, half-size textures, no dynamic lights or water reflections. Coolest and longest on battery")));
		me.TD(me, 1, 2, e = makeXonoticCommandButton_T(_("60 fps"), '0 0 0', "exec touch/profiles/balanced.cfg; menu_cmd sync", 0,
			_("Performance: 60 fps with vsync, half-size textures, portals at half resolution. The default on phones")));
		me.TD(me, 1, 2, e = makeXonoticCommandButton_T(_("Best quality"), '0 0 0', "exec touch/profiles/quality.cfg; menu_cmd sync", 0,
			_("Performance: full textures, realtime lights, bloom and full-resolution portals. For a cool or plugged-in device")));

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Frame rate")));
	me.TR(me);
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Frame cap:")));
		// Mixed, with every value a profile or Settings -> Video can set: a plain
		// 20-120 slider showed no knob at the engine default (256) or at 0.
		me.TD(me, 1, 2.5, e = makeXonoticMixedSlider_T("cl_maxfps", _("Highest frame rate the game draws. Lower is cooler and lasts longer on battery")));
			e.formatString = _("%s fps");
			e.addRange(e, 20, 120, 10);
			e.addRange(e, 128, 256, 128);
			e.addRange(e, 512, 1024, 512);
			e.addRange(e, 2048, 2048, 2048);
			e.addText(e, ZCTX(_("MAXFPS^Unlimited")), 0);
			e.configureXonoticMixedSliderValues(e);
		me.TD(me, 1, 2, e = makeXonoticCheckBox_T(0, "vid_vsync", _("Vertical sync"), _("Wait for the screen to refresh: no tearing, and steadier frame times")));

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Dynamic resolution")));
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticCheckBox_T(0, "r_viewscale_fpsscaling", _("Lower the resolution to hold the target frame rate"),
			_("Renders at a lower resolution while the frame rate falls short of the target, and back to full when it recovers")));
	me.TR(me);
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Target fps:")));
			setDependent(e, "r_viewscale_fpsscaling", 1, 1);
		me.TD(me, 1, 1.5, e = makeXonoticSlider_T(20, 120, 5, "r_viewscale_fpsscaling_target", _("The frame rate dynamic resolution tries to hold")));
			setDependent(e, "r_viewscale_fpsscaling", 1, 1);
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Lowest scale:")));
			setDependent(e, "r_viewscale_fpsscaling", 1, 1);
		me.TD(me, 1, 1.5, e = makeXonoticSlider_T(0.25, 1, 0.05, "r_viewscale_fpsscaling_min", _("The lowest resolution scale it may drop to (1 = full)")));
			setDependent(e, "r_viewscale_fpsscaling", 1, 1);
	me.TR(me);
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Render scale:")));
		me.TD(me, 1, 4.5, e = makeXonoticSlider_T(0.4, 1, 0.05, "r_viewscale", _("Fixed resolution scale for the 3D view (1 = full). The controls and text stay sharp. Lower is faster")));

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Jump button")));
	me.TR(me);
		me.TD(me, 1, 1.5, e = makeXonoticRadioButton_T(3, "touch_hop_mode", "0", _("Plain"),
			_("A plain jump button: one press, one jump")));
		me.TD(me, 1, 1.5, e = makeXonoticRadioButton_T(3, "touch_hop_mode", "1", _("Latch"),
			_("Hold the jump button briefly to latch it down and keep hopping")));
		me.TD(me, 1, 1.5, e = makeXonoticRadioButton_T(3, "touch_hop_mode", "2", _("Auto-hop"),
			_("Hop automatically while the move stick is held")));
}
