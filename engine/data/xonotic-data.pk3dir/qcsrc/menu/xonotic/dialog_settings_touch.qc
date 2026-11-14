#include "dialog_settings_touch.qh"
#include "dialog_touch_mods.qh"
#include "dialog_touch_esp.qh"
#include "dialog_touch_privacy.qh"

#include "touch_menu_layout.qh"
#include "touch_menu_scale.qh"
#include "touch_menu_settings.qh"
#include "textlabel.qh"
#include "slider.qh"
#include "checkbox.qh"
#include "commandbutton.qh"
#include "button.qh"
#include "mixedslider.qh"
#include "inputbox.qh"
#include "dialog_settings_input.qh"
#include "tabcontroller.qh"
#include "dialog_touch_controls.qh"

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

// Settings -> Mobile. Five sub-tabs; every control below is one cvar, and each
// cvar is on exactly one sub-tab.
//   Controls    input mode, layout, size and look of the controls, sticks and buttons
//   HUD         the weapon bar, the readouts, what stays with a keyboard attached
//   Aim         look speed, aim assist and its strength
//   Auto-shoot  auto-shoot, its timing and its conditions
//   Tactics     shot choices: secondary fire, lead, rockets, weapon switching,
//               and how shots read a moving enemy or a group
//   Dynamics    performance profile, frame rate, dynamic resolution, hop
entity makeGodnoticSettingsTab()
{
	entity me = NEW(GodnoticSettingsTab);
	me.configureDialog(me);
	return me;
}

void GodnoticSettingsTab_fillTouch(entity me)
{
	entity mc = makeXonoticTabController(me.rows - 1.5); // what every sub-tab is built for

	me.TR(me);
		me.TD(me, 1, 1, mc.makeTabButton(mc, _("Controls"),   makeXonoticMobileControlsTab()));
		me.TD(me, 1, 1, mc.makeTabButton(mc, _("HUD"),        makeXonoticMobileHudTab()));
		me.TD(me, 1, 1, mc.makeTabButton(mc, _("Aim"),        makeXonoticMobileAimTab()));
		me.TD(me, 1, 1, mc.makeTabButton(mc, _("Auto-shoot"), makeXonoticMobileAutoShootTab()));
		me.TD(me, 1, 1, mc.makeTabButton(mc, _("Tactics"),    makeXonoticMobileTacticsTab()));
		me.TD(me, 1, 1, mc.makeTabButton(mc, _("ESP"),        makeGodnoticEspTab()));
	me.TR(me);
		me.TD(me, 1, 1, mc.makeTabButton(mc, _("Dynamics"),   makeXonoticMobileDynamicsTab()));
		me.TD(me, 1, 1, mc.makeTabButton(mc, _("Loser"),      makeXonoticMobileLoserTab()));
		me.TD(me, 1, 1, mc.makeTabButton(mc, _("Privacy"),    makeGodnoticPrivacyTab()));
		me.TD(me, 1, 1, mc.makeTabButton(mc, _("Menu"),       makeGodnoticMenuSizeTab())); // menu size (touch_menu_settings.qc)
		me.TD(me, 1, 1, mc.makeTabButton(mc, _("Touch"),      makeGodnoticTouchControlsTab())); // look, stick, buttons (dialog_touch_controls.qc)
	me.gotoRC(me, 2.5, 0);
		me.TD(me, me.rows - 2.5, me.columns, mc);
}

void GodnoticSettingsTab_fill(entity me)
{
	if (touch_menu_on)
	{
		// Touch: finger-sized tabs, so two rows of six. The sub-tabs keep
		// their 14 rows in the 13 left under them (rows 7% shorter).
		GodnoticSettingsTab_fillTouch(me);
		return;
	}
	// One row of ten sub-tab buttons, then the sub-tab itself: 15.5 = 1.5 + 14.
	// Menu is here too: the desktop layout has to be able to switch itself off.
	float w = me.columns / 11;
	entity mc = makeXonoticTabController(me.rows - 1.5);
	me.TR(me);
		me.TD(me, 1, w, mc.makeTabButton(mc, _("Controls"),   makeXonoticMobileControlsTab()));
		me.TD(me, 1, w, mc.makeTabButton(mc, _("Touch"),      makeGodnoticTouchControlsTab())); // look, stick, buttons (dialog_touch_controls.qc)
		me.TD(me, 1, w, mc.makeTabButton(mc, _("HUD"),        makeXonoticMobileHudTab()));
		me.TD(me, 1, w, mc.makeTabButton(mc, _("Aim"),        makeXonoticMobileAimTab()));
		me.TD(me, 1, w, mc.makeTabButton(mc, _("Auto-shoot"), makeXonoticMobileAutoShootTab()));
		me.TD(me, 1, w, mc.makeTabButton(mc, _("Tactics"),    makeXonoticMobileTacticsTab()));
		me.TD(me, 1, w, mc.makeTabButton(mc, _("ESP"),        makeGodnoticEspTab())); // offline practice helpers (dialog_touch_esp.qc)
		me.TD(me, 1, w, mc.makeTabButton(mc, _("Dynamics"),   makeXonoticMobileDynamicsTab()));
		me.TD(me, 1, w, mc.makeTabButton(mc, _("Loser"),      makeXonoticMobileLoserTab()));
		me.TD(me, 1, w, mc.makeTabButton(mc, _("Privacy"),    makeGodnoticPrivacyTab())); // stats, name, terms (dialog_touch_privacy.qc)
		me.TD(me, 1, w, mc.makeTabButton(mc, _("Menu"),       makeGodnoticMenuSizeTab())); // menu size, desktop layout (touch_menu_settings.qc)
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
		me.TD(me, 1, 1.2, e = makeXonoticCommandButton(_("Standard"), '0 0 0', "exec touch/profiles/standard.cfg; exec touch/profiles/helpers.cfg; menu_cmd sync", 0));
		me.TD(me, 1, 1.2, e = makeXonoticCommandButton(_("Casual"), '0 0 0', "exec touch/profiles/casual.cfg; menu_cmd sync", 0));
		me.TD(me, 1, 1.2, e = makeXonoticCommandButton(_("Competitive"), '0 0 0', "exec touch/profiles/competitive.cfg; menu_cmd sync", 0));
		me.TD(me, 1, 1.2, e = makeXonoticCommandButton(_("Left hand"), '0 0 0', "exec touch/profiles/left.cfg; exec touch/profiles/helpers.cfg; menu_cmd sync", 0));
		me.TD(me, 1, 1.2, e = makeXonoticCommandButton_T(_("Minimal"), '0 0 0', "exec touch/profiles/minimal.cfg; menu_cmd sync", 0,
			_("For small phones: move stick, FIRE and HOP only")));

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
	// Bunny-hopping needs jump held. The default keeps HOP a plain button and
	// puts the hold on its own KEEP switch, apart from it, so a jump never
	// turns into hopping by accident.
	me.TR(me);
		me.TD(me, 1, 1.2, e = makeXonoticTextLabel(0, _("HOP button:")));
		me.TD(me, 1, 1.2, e = makeXonoticRadioButton_T(3, "touch_hop_mode", "3", _("+ KEEP switch"),
			_("HOP jumps while held. Tap the KEEP switch beside it to keep jumping with no finger down, and again to stop")));
		me.TD(me, 1, 1.2, e = makeXonoticRadioButton_T(3, "touch_hop_mode", "0", _("Plain"),
			_("HOP jumps while held, and nothing else")));
		me.TD(me, 1, 1.2, e = makeXonoticRadioButton_T(3, "touch_hop_mode", "1", _("Hold to keep"),
			_("Hold HOP briefly and it keeps jumping after you let go; tap it to stop")));
		me.TD(me, 1, 1.2, e = makeXonoticRadioButton_T(3, "touch_hop_mode", "2", _("Auto-hop"),
			_("Hop automatically while the move stick is held")));

	me.TR(me);
		me.TD(me, 1, 1.5, e = makeXonoticCommandButton(_("Customize layout"), '0 0 0', "touch_customize 1; togglemenu 0", COMMANDBUTTON_CLOSE));
		me.TD(me, 1, 1.5, e = makeXonoticButton(_("Save layout"), '0 0 0'));
			e.onClick = TouchMenu_SaveLayout_Click;
		me.TD(me, 1, 1.5, e = makeXonoticButton(_("Reset preset"), '0 0 0'));
			e.onClick = TouchMenu_ResetPreset_Click;
		me.TD(me, 1, 1.5, e = makeXonoticCommandButton(_("Apply & refresh"), '0 0 0', "menu_cmd sync", COMMANDBUTTON_APPLY));
}

entity makeXonoticMobileHudTab()
{
	entity me = NEW(XonoticMobileHudTab);
	me.configureDialog(me);
	return me;
}

void XonoticMobileHudTab_fill(entity me)
{
	entity e;

	// The size is the engine's cl_touch_weplist_scale, archived in config.cfg
	// rather than the touch layout, so a size dragged on a server is kept; the
	// position is the layout's. Both HUDs (the port's and the engine's on
	// servers) draw the bar from the same three cvars. Across reaches 0.97 so
	// the default, 0.962, is on the slider.
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Weapon bar")));
	me.TR(me);
		me.TD(me, 1, 1.2, e = makeXonoticTextLabel(0, _("Size:")));
		me.TD(me, 1, 1.8, e = makeXonoticSlider_T(0.7, 2.4, 0.1, "cl_touch_weplist_scale",
			_("Size of the weapon bar on the right edge. In a match you can also drag the grip under the bar: down to grow it, up to shrink it")));
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "touch_weplist_visible", _("Show the weapon bar"),
			_("The list of weapons you hold; tap one to switch to it")));
	me.TR(me);
		me.TD(me, 1, 1.2, e = makeXonoticTextLabel(0, _("Across:")));
			setDependent(e, "touch_weplist_visible", 1, 1);
		me.TD(me, 1, 1.8, e = makeXonoticSlider_T(0.04, 0.97, 0.01, "touch_weplist_x",
			_("Where the weapon bar sits across the screen, from the left edge (mirrored when left-handed)")));
			setDependent(e, "touch_weplist_visible", 1, 1);
		me.TD(me, 1, 1.2, e = makeXonoticTextLabel(0, _("Down:")));
			setDependent(e, "touch_weplist_visible", 1, 1);
		me.TD(me, 1, 1.8, e = makeXonoticSlider_T(0.15, 0.85, 0.01, "touch_weplist_y",
			_("Where the middle of the weapon bar sits down the screen. A long bar is kept on screen")));
			setDependent(e, "touch_weplist_visible", 1, 1);
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticTextLabel(0, _("Drag the grip under the bar in a match to resize it there.")));

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Readouts")));
	me.TR(me);
		me.TD(me, 1, 1.2, e = makeXonoticTextLabel(0, _("Readout size:")));
		me.TD(me, 1, 1.8, e = makeXonoticSlider_T(0.6, 1.4, 0.05, "touch_hud_scale",
			_("Size of the health, armor and ammo readouts. The weapon bar has its own size above")));

	// The engine's (touch_hook.c), so both work on public servers too.
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Grappling hook")));
	me.TR(me);
		me.TD(me, 1, 1.2, e = makeXonoticTextLabel(0, _("Hook button:")));
		me.TD(me, 1, 1.2, e = makeXonoticRadioButton_T(5, "touch_hook_visible", "0", _("Off"),
			_("Never show the HOOK button")));
		me.TD(me, 1, 1.8, e = makeXonoticRadioButton_T(5, "touch_hook_visible", "2", _("When available"),
			_("Show HOOK while you have a grappling hook: the server's offhand hook, or the Hook weapon with fuel. Hold it to hook, let go to release")));
		me.TD(me, 1, 1.8, e = makeXonoticRadioButton_T(5, "touch_hook_visible", "1", _("Always"),
			_("Always show HOOK. Where there is no hook it sends what the hook key would")));
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticCheckBox_T(0, "cl_touch_hook_rescue", _("Catch me with the hook when I fall off the map"),
			_("Falling with nothing below to land on, the hook fires at something it can catch, pulls you back and lets go over ground. Only with a hook, only in touch mode")));

	// TURN (touch_turn.c): one tap turns the player round.
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Quick turn")));
	me.TR(me);
		me.TD(me, 1, 2, e = makeXonoticCheckBox_T(0, "touch_turn_visible", _("TURN button"),
			_("A button by the move stick: one tap turns you round to face what was behind you")));
		me.TD(me, 1, 1, e = makeXonoticTextLabel(0, _("Turn by:")));
			setDependent(e, "touch_turn_visible", 1, 1);
		me.TD(me, 1, 2.2, e = makeXonoticSlider_T(90, 360, 45, "touch_turn_angle",
			_("How far one tap turns you, in degrees. 180: face the other way")));
			setDependent(e, "touch_turn_visible", 1, 1);

	// Offhand grenades (touch_nade.c), on servers that have them (g_nades).
	me.TR(me);
		me.TD(me, 1, 1.2, e = makeXonoticTextLabel(0, _("Grenades:")));
		me.TD(me, 1, 2.2, e = makeXonoticCheckBox_T(0, "cl_touch_nade_row", _("NADE row"),
			_("A NADE row under the weapons on servers with offhand grenades: hold it to cook, let go to throw, drag off it to aim")));
		me.TD(me, 1, 2.6, e = makeXonoticCheckBox_T(0, "cl_touch_nade_autothrow", _("Throw before it blows"),
			_("A grenade held too long is thrown by itself just before it would blow up in your hand")));
			setDependent(e, "cl_touch_nade_row", 1, 1);

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("With a keyboard attached")));
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticCheckBox_T(0, "touch_desk_pills", _("Keep MENU, CHAT and SCORE on screen with a keyboard"),
			_("With a keyboard attached the touch controls go away and the mouse aims. The weapon bar stays tappable; this keeps the MENU, CHAT, SCORE and CONSOLE buttons too, dimmed")));
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
			// A tap beside the knob moves it a page. The default page for this
			// range is 10, a fifth of the slider per tap; half a unit can be
			// tuned by tapping, and dragging the knob goes in steps of 0.1.
			e.valuePageStep = 0.5;
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
			_("Aim slows down over an enemy, and is pulled toward one while a finger is on the aim area")));
		me.TD(me, 1, 1, e = makeXonoticCommandButton(_("Light"), '0 0 0',
			"cl_touch_aimassist 1; cl_touch_aimassist_friction 0.25; cl_touch_aimassist_strength 0.3; cl_touch_aimassist_maxrate 45; cl_touch_aimassist_cone 5; menu_cmd sync", 0));
		me.TD(me, 1, 1, e = makeXonoticCommandButton(_("Medium"), '0 0 0',
			"cl_touch_aimassist 1; cl_touch_aimassist_friction 0.45; cl_touch_aimassist_strength 0.6; cl_touch_aimassist_maxrate 90; cl_touch_aimassist_cone 8; menu_cmd sync", 0));
		me.TD(me, 1, 1, e = makeXonoticCommandButton(_("Strong"), '0 0 0',
			"cl_touch_aimassist 1; cl_touch_aimassist_friction 0.6; cl_touch_aimassist_strength 0.85; cl_touch_aimassist_maxrate 140; cl_touch_aimassist_cone 11; menu_cmd sync", 0));
		me.TD(me, 1, 1, e = makeXonoticCommandButton(_("Off"), '0 0 0', "cl_touch_aimassist 0; menu_cmd sync", 0));
	me.TR(me);
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Slowdown:")));
			setDependent(e, "cl_touch_aimassist", 1, 1);
		me.TD(me, 1, 1.5, e = makeXonoticSlider_T(0, 0.8, 0.05, "cl_touch_aimassist_friction", _("How much aiming slows down over an enemy (0 = off)")));
			setDependent(e, "cl_touch_aimassist", 1, 1);
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Pull:")));
			setDependent(e, "cl_touch_aimassist", 1, 1);
		me.TD(me, 1, 1.5, e = makeXonoticSlider_T(0, 1, 0.05, "cl_touch_aimassist_strength", _("How strongly the view is pulled toward a nearby enemy while a finger is on the aim area (0 = off)")));
			setDependent(e, "cl_touch_aimassist", 1, 1);
	me.TR(me);
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Catch cone:")));
			setDependent(e, "cl_touch_aimassist", 1, 1);
			setDependentAND(e, "cl_touch_aim_mode", 0, 0);
		me.TD(me, 1, 1.5, e = makeXonoticSlider_T(1, 60, 1, "cl_touch_aimassist_cone", _("How many degrees from the crosshair an enemy still counts as near (Targets: Radius, Settings > Auto-shoot). Up to 60: the whole view")));
			setDependent(e, "cl_touch_aimassist", 1, 1);
			setDependentAND(e, "cl_touch_aim_mode", 0, 0);
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Max pull:")));
			setDependent(e, "cl_touch_aimassist", 1, 1);
		me.TD(me, 1, 1.5, e = makeXonoticSlider_T(0, 180, 10, "cl_touch_aimassist_maxrate", _("The fastest the pull can turn the view, in degrees per second. It never snaps")));
			setDependent(e, "cl_touch_aimassist", 1, 1);
	me.TR(me);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_aim_ring", _("Show target ring"),
			_("Draw a ring at the crosshair while an enemy is near (accent colour) or under it (red), so you can see the helpers working")));
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_aimassist_track", _("Keep tracking the enemy I am shooting"),
			_("While auto-shoot fires at an enemy the pull keeps following them, even with your thumb resting. Off: only while a finger is on the aim area and moving")));
			setDependent(e, "cl_touch_aimassist", 1, 1);
	// Engine-side (touch_dodge.c), like the aim helpers: its own section.
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Auto dodge")));
	me.TR(me);
		me.TD(me, 1, 2, e = makeXonoticCheckBox_T(0, "cl_touch_autododge", _("Auto dodge"),
			_("Step aside from an enemy rocket, grenade, bolt, orb, mine or crylink about to hit you, while you keep aiming. Left alone when your own path already clears it; never off a ledge or into a mine")));
		me.TD(me, 1, 2, e = makeXonoticRadioButton_T(4, "cl_touch_autododge_scope", "0", _("Servers and local"),
			_("Dodge on servers and in local matches")));
			setDependent(e, "cl_touch_autododge", 1, 1);
		me.TD(me, 1, 2, e = makeXonoticRadioButton_T(4, "cl_touch_autododge_scope", "1", _("Local only"),
			_("Dodge only in local matches against bots")));
			setDependent(e, "cl_touch_autododge", 1, 1);
	me.TR(me);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_autododge_rear", _("Shots from behind too"),
			_("Also dodge shots from behind and the side, out of view. Never one behind a wall")));
			setDependent(e, "cl_touch_autododge", 1, 1);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_autododge_cue", _("Show an arrow when it dodges"),
			_("Flash an arrow at the edge of the screen the way a dodge goes")));
			setDependent(e, "cl_touch_autododge", 1, 1);
	// Engine-side (touch_ledge.c): keeps the stick from walking you off a ledge.
	me.TR(me);
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Ledge guard:")));
		me.TD(me, 1, 1, e = makeXonoticRadioButton_T(6, "cl_touch_ledgeguard", "0", _("Off"),
			_("Walk off any ledge")));
		me.TD(me, 1, 1.7, e = makeXonoticRadioButton_T(6, "cl_touch_ledgeguard", "1", _("Deadly falls"),
			_("Stop, slow or turn you along the edge before you walk into the void, lava or a hurt zone. Jumping is never held back")));
		me.TD(me, 1, 1.8, e = makeXonoticRadioButton_T(6, "cl_touch_ledgeguard", "2", _("Any long drop"),
			_("Also keep you from walking off a drop deeper than cl_touch_ledgeguard_drop units (192 unless changed). Jumping is never held back")));
	me.TR(me);
		me.TD(me, 1, 6, e = makeXonoticTextLabel(0, _("Touch mode only: off while a keyboard or mouse is playing. Never through walls.")));
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
		me.TD(me, 1, 4, e = makeXonoticSlider_T(0, 0.5, 0.05, "cl_touch_autoshoot_delay",
			_("How long the crosshair must stay on an enemy before firing starts, in seconds. 0: at once")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);
	me.TR(me);
		me.TD(me, 1, 2, e = makeXonoticTextLabel(0, _("Shoot radius:")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);
		me.TD(me, 1, 4, e = makeXonoticSlider_T(0, 15, 0.5, "cl_touch_autoshoot_radius",
			_("Fire when the crosshair is within this many degrees of an enemy. 0: only when it is on the body")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);
	me.TR(me);
		me.TD(me, 1, 2, e = makeXonoticTextLabel(0, _("Keep firing:")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);
		me.TD(me, 1, 4, e = makeXonoticSlider_T(0, 0.5, 0.05, "cl_touch_autoshoot_release",
			_("How long firing continues after the crosshair slips off, in seconds")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);

	me.TR(me);
		me.TD(me, 1, 1.2, e = makeXonoticTextLabel(0, _("Weapons:")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);
		me.TD(me, 1, 1.8, e = makeXonoticRadioButton_T(4, "cl_touch_autoshoot_weapons", "0", _("Every weapon"),
			_("Auto-shoot with whatever you hold")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);
		me.TD(me, 1, 3, e = makeXonoticRadioButton_T(4, "cl_touch_autoshoot_weapons", "1", _("Instant-hit weapons only"),
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
		me.TD(me, 1, 4, e = makeXonoticTextSlider_T("cl_touch_aim_range",
			_("Enemies farther away than this many units are ignored by auto-shoot and aim assist. Unlimited: any enemy in sight")));
			e.addValue(e, "500", "500");
			e.addValue(e, "1000", "1000");
			e.addValue(e, "1500", "1500");
			e.addValue(e, "2000", "2000");
			e.addValue(e, "3000", "3000");
			e.addValue(e, "4000", "4000");
			e.addValue(e, "6000", "6000");
			e.addValue(e, _("Unlimited"), "0");
			e.configureXonoticTextSliderValues(e);
			setDependent(e, "cl_touch_autoshoot", 1, 1);
	me.TR(me);
		me.TD(me, 1, 1.2, e = makeXonoticTextLabel(0, _("Targets:")));
		me.TD(me, 1, 1.2, e = makeXonoticRadioButton_T(5, "cl_touch_aim_mode", "0", _("Radius"),
			_("Enemies within the catch cone (Settings > Aim) of the crosshair")));
		me.TD(me, 1, 1.6, e = makeXonoticRadioButton_T(5, "cl_touch_aim_mode", "1", _("Field of view"),
			_("Any enemy on screen; the catch cone is not used")));
		me.TD(me, 1, 2, e = makeXonoticRadioButton_T(5, "cl_touch_aim_mode", "2", _("Full 360 (offline only)"),
			_("Enemies all around you, in offline matches against bots only. Anywhere else it works as Field of view")));
	me.TR(me);
		me.TD(me, 1, 2, e = makeXonoticTextLabel(0, _("Keep the target:")));
		me.TD(me, 1, 4, e = makeXonoticSlider_T(0, 10, 0.5, "cl_touch_aimassist_hysteresis",
			_("The enemy already targeted stays the target unless another is this many degrees nearer the crosshair. 0: always the nearest")));
	me.TR(me);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_aimassist_priority", _("Pick the best of several enemies"),
			_("Nearest the crosshair first, then the one your weapon hurts most, one facing you up close, one already hurt, a group for a blast weapon")));
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_autoshoot_teamsafe", _("Never with a teammate in the way"),
			_("Hold fire while a teammate is on the line of the shot or would be inside its blast")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);
	me.TR(me);
		me.TD(me, 1, 6, e = makeXonoticTextLabel(0, _("Only visible enemies, never teammates or through walls. Touch mode only.")));
}

entity makeXonoticMobileTacticsTab()
{
	entity me = NEW(XonoticMobileTacticsTab);
	me.configureDialog(me);
	return me;
}

void XonoticMobileTacticsTab_fill(entity me)
{
	entity e;

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Shot choice")));
	me.TR(me);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_autoshoot_alt", _("Use secondary fire when it is the better shot"),
			_("Auto-shoot picks primary or secondary fire for the weapon and the distance, like the shotgun's melee up close")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_autoshoot_lead", _("Lead moving targets"),
			_("With a projectile weapon, aim and fire where a moving enemy will be when the shot gets there")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);
	me.TR(me);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_autoshoot_steer", _("Guide and detonate rockets"),
			_("Keep a fired Devastator rocket on the enemy and set it off beside them")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_autoswitch", _("Switch to the best weapon for the fight"),
			_("Switch to a better weapon you hold for the enemy in sight and its distance. Never right after you chose one yourself")));
	me.TR(me);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_autoshoot_safe", _("Hold fire when the blast would hit me"),
			_("Do not fire when the shot would catch you in its own blast, the enemy is out of the weapon's reach, or the weapon is not one to fire by itself (Port-O-Launch, Hook, Tuba)")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Reading the fight")));
	me.TR(me);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_autoshoot_air", _("Lead enemies in the air"),
			_("Aim where a jumping or falling enemy will be on its way down, or where it lands if the shot arrives later")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_autoshoot_predict", _("Allow for ping and walls"),
			_("Lead by the time your ping and the screen add, stop a lead at a wall the enemy will run into, and lead instant-hit weapons when the ping is beyond what the server makes up for")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);
	me.TR(me);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_autoshoot_feet", _("Aim explosives at the feet"),
			_("Rockets, grenades and other blast weapons aim at the floor under the enemy, so a near miss still hurts")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_autoshoot_group", _("Aim explosives at groups"),
			_("Aim a blast where it catches the most enemies you can see, never teammates, and set off rockets where they hurt all of them most")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);
	me.TR(me);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_autoswitch_smart", _("Pick weapons by expected damage"),
			_("Choose the weapon for the distance, how the enemy moves and how many stand together, counting the time a switch takes")));
			setDependent(e, "cl_touch_autoswitch", 1, 1);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_autoswitch_combo", _("Finish knocked-up enemies"),
			_("After your blast throws an enemy into the air, switch at once to an instant-hit weapon to finish it")));
			setDependent(e, "cl_touch_autoswitch", 1, 1);
	me.TR(me);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_autoshoot_clear", _("Fly rockets around walls"),
			_("Steer a Devastator rocket over ledges and around pillars on its way, and to where the enemy was last seen if it hides")));
			setDependent(e, "cl_touch_autoshoot_steer", 1, 1);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_autoshoot_volley", _("Fire rockets in volleys"),
			_("Launch the next rocket while one that will hit by itself is still flying, and set them off for the most damage")));
			setDependent(e, "cl_touch_autoshoot_steer", 1, 1);
	me.TR(me);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_autoswitch_lastresort", _("Blaster only as a last resort"),
			_("Never switch to the Blaster while another weapon you hold can shoot, and leave it at once when your weapon runs dry")));
			setDependent(e, "cl_touch_autoswitch", 1, 1);
		me.TD(me, 1, 1.6, e = makeXonoticCheckBox_T(0, "cl_touch_autoshoot_head", _("Head shots"),
			_("Aim at the head with a weapon that does more damage there. In standard Xonotic none does, the Vortex included: only a server that gives the Rifle a headshot bonus")));
			setDependent(e, "cl_touch_autoshoot", 1, 1);
		me.TD(me, 1, 1.4, e = makeXonoticTextSlider_T("cl_touch_autoshoot_head_rifle",
			_("The server's Rifle headshot multiplier, which the game cannot read: none in standard Xonotic, 1.25 with the Nexuiz 2.5 balance")));
			e.addValue(e, _("No bonus"), "0");
			e.addValue(e, "x1.25", "1.25");
			e.addValue(e, "x1.5", "1.5");
			e.addValue(e, "x2", "2");
			e.configureXonoticTextSliderValues(e);
			setDependent(e, "cl_touch_autoshoot_head", 1, 1);
	me.TR(me);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_autoshoot_combo", _("Electro combos"),
			_("Lob Electro orbs at an enemy standing or coming at you, then set them off with the bolt")));
			setDependent(e, "cl_touch_autoshoot_alt", 1, 1);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_aimassist_lastseen", _("Hold where they were last seen"),
			_("When the enemy you are fighting goes out of sight, keep the crosshair where it was last seen, for up to 3 seconds")));
			setDependent(e, "cl_touch_aimassist", 1, 1);
	me.TR(me);
		me.TD(me, 1, 6, e = makeXonoticTextLabel(0, _("See-through walls, Full 360 and instant turn are in the ESP tab: offline matches against bots only.")));
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

}

entity makeXonoticMobileLoserTab()
{
	entity me = NEW(XonoticMobileLoserTab);
	me.configureDialog(me);
	return me;
}

void XonoticMobileLoserTab_fill(entity me)
{
	entity e;

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("(Development) Loser mode")));
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticTextLabel(0, _("Enables all mobile auto-shoot, aim assist, ESP and wallhack helpers on a desktop machine. Off by default. For development and testing only.")));
	me.TR(me);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_loser", _("Loser mode (desktop)"),
			_("Development: run the touch aim assist, auto-shoot and see-through-wall helpers without a touch screen")));
}
