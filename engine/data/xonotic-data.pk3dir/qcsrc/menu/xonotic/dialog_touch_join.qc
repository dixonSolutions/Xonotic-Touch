#include "dialog_touch_join.qh"

#include "touch_menu_scale.qh"

bool TouchJoin_Watching()
{
	string m = cvar_string("_touch_hud_mode");
	return m == "spectating" || m == "following";
}

void GodnoticTeamButton_draw(entity me)
{
	if (me.disabled)
		return;
	SUPER(GodnoticTeamButton).draw(me);
}

entity makeGodnoticTeamButton(string theText, vector theColor, string theCommand)
{
	entity me = NEW(GodnoticTeamButton);
	me.configureXonoticCommandButton(me, theText, theColor, theCommand, COMMANDBUTTON_CLOSE, string_null);
	return me;
}

void GodnoticJoinDialog_configureDialog(entity me)
{
	// rows two units tall: 60 dp buttons with the touch layer's row height
	if (touch_menu_on)
		me.rowHeight = TouchMenuScale_dpToUnits(30 * TouchMenuScale_size());
	SUPER(GodnoticJoinDialog).configureDialog(me);
}

void GodnoticJoinDialog_showNotify(entity me)
{
	SUPER(GodnoticJoinDialog).showNotify(me);
	float teams = cvar("_teams_available");
	bool teamgame = (cvar("_touch_hud_flags") & 4) || teams;
	me.team1.disabled = !(teams & 1);
	me.team2.disabled = !(teams & 2);
	me.team3.disabled = !(teams & 4);
	me.team4.disabled = !(teams & 8);
	me.autoButton.setText(me.autoButton, (teamgame ? _("Join (auto-select team)") : _("Join")));
	me.wasWatching = TouchJoin_Watching();
}

// Closes itself once the player is in the game (a choice here, the server's
// own auto-join, another way in): it was up to get them there.
void GodnoticJoinDialog_draw(entity me)
{
	if (me.wasWatching && cvar_string("_touch_hud_mode") == "playing")
	{
		me.wasWatching = false;
		me.close(me);
		return;
	}
	SUPER(GodnoticJoinDialog).draw(me);
}

void GodnoticJoinDialog_fill(entity me)
{
	entity e;
	me.TR(me);
		me.TD(me, 2, 4, e = me.autoButton = makeGodnoticCommandButton(_("Join"), '0 0 0', "touch_join auto", COMMANDBUTTON_CLOSE));
			e.preferredFocusPriority = 1;
	me.TR(me);
	me.TR(me);
	me.gotoRC(me, 2.5, 0);
		me.TD(me, 2, 1, me.team1 = makeGodnoticTeamButton(_("red"), '1 0.5 0.5', "touch_join red"));
		me.TD(me, 2, 1, me.team2 = makeGodnoticTeamButton(_("blue"), '0.5 0.5 1', "touch_join blue"));
		me.TD(me, 2, 1, me.team3 = makeGodnoticTeamButton(_("yellow"), '1 1 0.5', "touch_join yellow"));
		me.TD(me, 2, 1, me.team4 = makeGodnoticTeamButton(_("pink"), '1 0.5 1', "touch_join pink"));
	me.gotoRC(me, 5.5, 0);
		me.TD(me, 2, 4, makeGodnoticCommandButton(_("Spectate"), '0 0 0', "touch_join spectate", COMMANDBUTTON_CLOSE));
}
