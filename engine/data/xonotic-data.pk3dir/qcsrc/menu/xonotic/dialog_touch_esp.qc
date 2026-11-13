// SPDX-License-Identifier: GPL-3.0-or-later
#include "dialog_touch_esp.qh"

#include "textlabel.qh"
#include "checkbox.qh"
#include "checkbox_string.qh"
#include "slider.qh"
#include "util.qh"

entity makeGodnoticEspTab()
{
	entity me = NEW(GodnoticEspTab);
	me.configureDialog(me);
	return me;
}

// A sub-option: greyed out while ESP is off.
entity GodnoticEsp_option(entity me, float width, string theCvar, string theText, string theTooltip)
{
	entity e;
	me.TD(me, 1, width, e = makeXonoticCheckBox_T(0, theCvar, theText, theTooltip));
	setDependentStringNotEqual(e, "cl_touch_esp_enabled", "off");
	return e;
}

float GodnoticEsp_magnetOn(entity e)
{
	return cvar_string("cl_touch_esp_enabled") != "off" && cvar("cl_touch_practice_magnet");
}

void GodnoticEspTab_fill(entity me)
{
	entity e;

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("ESP (offline practice against bots)")));
	me.TR(me);
		me.TD(me, 1, 3, e = makeXonoticCheckBoxString("local", "off", "cl_touch_esp_enabled", _("Enable ESP and practice helpers")));
			e.tooltip = _("On: the helpers below work in a match against bots on this device. Off: never, anywhere");
		me.TD(me, 1, 3, e = makeXonoticTextLabel(0, _("Takes effect at once and is saved.")));
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticTextLabel(0, _("Only in a match against bots on this device with nobody else connected. Never on servers or with other players.")));
			e.allowCut = true;

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("What is shown")));
	me.TR(me);
		GodnoticEsp_option(me, 3, "cl_touch_practice_seethrough", _("See enemies through walls"),
			_("An orange square with the distance marks an enemy behind a wall; the aim pre-aims at it, and splash weapons and rockets reach it round cover"));
		GodnoticEsp_option(me, 3, "cl_touch_practice_intel", _("Enemy health, armour and weapon"),
			_("Show every enemy's health, armour and weapon, take the target worth taking first, and switch to a weapon that finishes it"));

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Aim")));
	me.TR(me);
		GodnoticEsp_option(me, 2, "cl_touch_practice_360", _("Full 360 targets"),
			_("With Targets set to Full 360 (Auto-shoot tab), enemies all around you can be the target, with a marker at the screen edge"));
		GodnoticEsp_option(me, 2, "cl_touch_practice_instant", _("Instant turn"),
			_("The aim moves onto the target at once instead of at the assist's turn rate"));
		GodnoticEsp_option(me, 2, "cl_touch_practice_exact", _("Exact lead"),
			_("Lead shots from each enemy's exact velocity on the local server, and aim at any distance"));
	me.TR(me);
		GodnoticEsp_option(me, 3, "cl_touch_practice_burst", _("Burst splash beside hidden enemies"),
			_("A splash shot at an enemy behind cover aims where its simulated flight bursts next to the enemy, and fires only when it would"));

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Autopilot (while the stick is idle)")));
	me.TR(me);
		GodnoticEsp_option(me, 2, "cl_touch_practice_pilot", _("Move for me"),
			_("Keep the weapon's best range and strafe, chase a nearly dead enemy, back into cover when outnumbered. Touching the stick takes over"));
		GodnoticEsp_option(me, 2, "cl_touch_practice_aimdodge", _("Dodge aimed shots"),
			_("Step out of the line of an enemy that is aiming at you"));
		GodnoticEsp_option(me, 2, "cl_touch_practice_threat", _("Size up enemies"),
			_("Stay out of reach of a weapon you outrange, close in when stronger, back off to health when one more hit would kill"));
	me.TR(me);
		GodnoticEsp_option(me, 2, "cl_touch_practice_objectives", _("Play the objective"),
			_("In CTF fetch the enemy flag and carry it home; in team games help your mates first"));
		GodnoticEsp_option(me, 2, "cl_touch_practice_detour", _("Grab pickups nearby"),
			_("During a fight, step aside for a pickup close by that is worth it now"));
		GodnoticEsp_option(me, 2, "cl_touch_practice_position", _("Best spot to fight"),
			_("Move toward a nearby spot in sight of the enemy, hidden from the others, on higher ground"));
	me.TR(me);
		GodnoticEsp_option(me, 2, "cl_touch_practice_exposure", _("Hide from the others"),
			_("While fighting one enemy, prefer spots the other enemies cannot see"));
		GodnoticEsp_option(me, 2, "cl_touch_practice_magnet", _("Pickup magnet"),
			_("Pickups within reach and in clear view are picked up as if touched. Changes the game's rules, so off by default"));
		me.TD(me, 1, 2, e = makeXonoticSlider_T(32, 512, 16, "cl_touch_practice_magnet_radius", _("How far the pickup magnet reaches, in game units")));
			setDependentWeird(e, GodnoticEsp_magnetOn);
}
