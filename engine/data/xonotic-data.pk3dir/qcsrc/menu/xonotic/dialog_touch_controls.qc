#include "dialog_touch_controls.qh"

#include "textlabel.qh"
#include "slider.qh"
#include "checkbox.qh"
#include "radiobutton.qh"
#include "commandbutton.qh"

entity makeGodnoticTouchControlsTab()
{
	entity me = NEW(GodnoticTouchControlsTab);
	me.configureDialog(me);
	return me;
}

void GodnoticTouchControlsTab_fill(entity me)
{
	entity e;
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Look")));
	me.TR(me);
		me.TD(me, 1, 1.4, e = makeXonoticTextLabel(0, _("Swipe sensitivity:")));
		me.TD(me, 1, 1.6, e = makeXonoticSlider_T(1, 8, 0.1, "touch_sens_base",
			_("How far the view turns for a swipe. The same swipe turns the same angle on every screen size")));
		me.TDempty(me, 0.2);
		me.TD(me, 1, 1.2, e = makeXonoticTextLabel(0, _("Up/down ratio:")));
		me.TD(me, 1, 1.6, e = makeXonoticSlider_T(0.4, 1.4, 0.05, "touch_sens_y_mult",
			_("Vertical look speed compared with horizontal")));
	me.TR(me);
		me.TD(me, 1, 1.4, e = makeXonoticTextLabel(0, _("Quick swipes:")));
		me.TD(me, 1, 1, e = makeXonoticRadioButton_T(4, "touch_look_accel", "0", _("Same"),
			_("No acceleration: a swipe turns the same whatever its speed")));
		me.TD(me, 1, 1, e = makeXonoticRadioButton_T(4, "touch_look_accel", "0.8", _("Faster"),
			_("A slow drag aims exactly; a quick swipe turns further")));
		me.TD(me, 1, 1, e = makeXonoticRadioButton_T(4, "touch_look_accel", "1.4", _("Much faster"),
			_("A turn-around in one short flick")));
		me.TD(me, 1, 1.6, e = makeXonoticCheckBox_T(0, "touch_invert_y", _("Invert up/down"), string_null));

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Move stick")));
	me.TR(me);
		me.TD(me, 1, 1.4, e = makeXonoticTextLabel(0, _("Size:")));
		me.TD(me, 1, 1.6, e = makeXonoticSlider_T(0.2, 0.45, 0.01, "touch_move_size", _("Size of the move stick")));
		me.TDempty(me, 0.2);
		me.TD(me, 1, 1.2, e = makeXonoticTextLabel(0, _("Dead zone:")));
		me.TD(me, 1, 1.6, e = makeXonoticSlider_T(0, 0.35, 0.01, "touch_stick_deadzone", _("How far the thumb moves before you do")));
	me.TR(me);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "touch_stick_float", _("Stick appears under your thumb"),
			_("The stick starts wherever your left thumb lands and follows it. Off: it stays where it is drawn")));
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "touch_stick_analog", _("Move at any angle"),
			_("Off: eight directions, like keys")));

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Buttons")));
	me.TR(me);
		me.TD(me, 1, 1.4, e = makeXonoticTextLabel(0, _("Size:")));
		me.TD(me, 1, 1.6, e = makeXonoticSlider_T(0.7, 1.4, 0.05, "touch_scale", _("Size of the buttons and the stick")));
		me.TDempty(me, 0.2);
		me.TD(me, 1, 1.2, e = makeXonoticTextLabel(0, _("Opacity:")));
		me.TD(me, 1, 1.6, e = makeXonoticSlider_T(0.3, 1.0, 0.05, "touch_opacity", _("Control overlay opacity")));
	me.TR(me);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "touch_fire_drag_look", _("Aim while firing"),
			_("Drag your thumb off FIRE to turn while it keeps firing")));
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "touch_button_drag_look", _("Aim from HOP, DUCK and ALT too"),
			_("A thumb dragged off these buttons turns the view while they stay held")));
	me.TR(me);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_touch_aimassist", _("Aim assist"),
			_("In matches on this device, and on servers that allow it")));
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "touch_haptics", _("Vibrate on button presses"),
			_("On devices whose app can vibrate")));
			setDependent(e, "_touch_haptics_available", 1, 1);
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticTextLabel(0, _("HOP keeps jumping while held: that is how you gain speed. Its modes are on the Controls tab.")));

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Layout")));
	me.TR(me);
		me.TD(me, 1, 2, e = makeXonoticCheckBox_T(0, "touch_handedness", _("Left-handed"),
			_("Mirror the layout: the move stick on the right, aim and buttons on the left")));
		me.TD(me, 1, 2, e = makeXonoticCommandButton_T(_("Edit layout"), '0 0 0', "touch_customize 1; togglemenu 0", COMMANDBUTTON_CLOSE,
			_("In a match: drag the controls where you want them")));
		me.TD(me, 1, 2, e = makeXonoticCommandButton_T(_("Reset to defaults"), '1 0.6 0.6',
			"exec touch_defaults.cfg; exec touch/profiles/standard.cfg; exec touch/profiles/helpers.cfg; touch_topbar_auto 1; menu_cmd sync", 0,
			_("Every touch control setting back to its default")));
}
