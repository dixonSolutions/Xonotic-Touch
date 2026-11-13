// SPDX-License-Identifier: GPL-3.0-or-later
#include "dialog_touch_privacy.qh"

#include "dialog_touch_credits.qh"
#include "textlabel.qh"
#include "checkbox.qh"
#include "button.qh"
#include "inputbox.qh"
#include "commandbutton.qh"
#include "util.qh"

entity makeGodnoticPrivacyTab()
{
	entity me = NEW(GodnoticPrivacyTab);
	me.configureDialog(me);
	return me;
}

void GodnoticPrivacyTab_fill(entity me)
{
	entity e;

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Player statistics (XonStat, stats.xonotic.org)")));
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticCheckBox_T(0, "cl_allow_uidtracking", _("Let servers send my match statistics to XonStat"),
			_("Servers that report to XonStat send your results with your player ID. Off: they record you as an anonymous player")));
	me.TR(me);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_allow_uid2name", _("Show my player name there"),
			_("Your name appears with your statistics and in race and CTF records")));
			setDependent(e, "cl_allow_uidtracking", 1, 1);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "cl_allow_uidranking", _("Rank me in leaderboards"),
			_("Your statistics count toward Elo rankings and leaderboards")));
			setDependent(e, "cl_allow_uidtracking", 1, 1);
	me.TR(me);
		me.TD(me, 2, me.columns, e = makeXonoticTextLabel(0, _("Off on a new install. Matches you play on this device are never sent. A change applies the next time you join a server.")));
			e.allowWrap = true;
	me.TR(me);
	me.TR(me);
		me.TD(me, 1, 2, e = makeXonoticTextLabel(0, _("Player name:")));
		me.TD(me, 1, 3, e = makeXonoticInputBox_T(1, "_cl_name", _("The name other players see. Also in Multiplayer > Profile")));
			e.forbiddenCharacters = "\r\n\\\"$";
			e.maxLength = -127;
		me.TD(me, 1, 1, e = makeXonoticCommandButton(_("Apply"), '0 0 0', "name \"$_cl_name\"", 0));
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Player ID")));
	me.TR(me);
		me.TD(me, 2, me.columns, e = makeXonoticTextLabel(0, _("The game makes a key on this device as your player ID. Servers see it when you join (for records and bans); with statistics off they do not pass it on to XonStat.")));
			e.allowWrap = true;
	me.TR(me);
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Terms")));
	me.TR(me);
		me.TD(me, 1.5, 3, e = makeXonoticButton_T(_("Read Xonotic's terms of service"), '0 0 0',
			_("Fetched from update.xonotic.org when you open them")));
			e.onClick = GodnoticTerms_Open;
			e.onClickEntity = e;
		me.TD(me, 1.5, 3, e = makeXonoticTextLabel(0, _("They govern XonStat. Nothing here asks you to accept them to play.")));
			e.allowWrap = true;
}
