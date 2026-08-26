#include "dialog_touch_home.qh"

#include <common/gametypes/_mod.qh>
#include <common/mapinfo.qh>
#include "bigbutton.qh"
#include "touchbutton.qh"
#include "textlabel.qh"
#include "button.qh"
#include "mainwindow.qh"
#include "touch_startup_util.qh"
#include "touch_update_util.qh"

void TouchHome_PlayNow(entity btn, entity dummy)
{
	if (!TouchStartup_assetsReady())
	{
		DialogOpenButton_Click_withCoords(NULL, main.assetFetchDialog, '0 0 0', eX * conwidth + eY * conheight);
		return;
	}
	cvar_set("timelimit_override", "10");
	MapInfo_SwitchGameType(MAPINFO_TYPE_DEATHMATCH);
	MapInfo_FilterGametype(MapInfo_CurrentGametype(), MapInfo_CurrentFeatures(), MapInfo_RequiredFlags(), MapInfo_ForbiddenFlags(), 0);

	string s = "stormkeep";
	if (MapInfo_count > 0)
	{
		float m = floor(random() * MapInfo_count);
		s = MapInfo_BSPName_ByID(m);
	}

	localcmd("disconnect\n");
	localcmd("g_campaign 0\n");
	makeServerSingleplayer();
	MapInfo_LoadMap(s, 1);
	localcmd("bot_number 3\n");
	cvar_set("lastlevel", "1");
}

void TouchHome_OpenCampaign(entity btn, entity dummy)
{
	if (!TouchStartup_assetsReady())
	{
		DialogOpenButton_Click_withCoords(NULL, main.assetFetchDialog, '0 0 0', eX * conwidth + eY * conheight);
		return;
	}
	localcmd("menu_cmd directmenu Singleplayer\n");
}

void TouchHome_OpenMultiplayer(entity btn, entity dummy)
{
	if (!TouchStartup_assetsReady())
	{
		DialogOpenButton_Click_withCoords(NULL, main.assetFetchDialog, '0 0 0', eX * conwidth + eY * conheight);
		return;
	}
	localcmd("menu_cmd directmenu Multiplayer\n");
}

void TouchHome_OpenUpdates(entity btn, entity dummy)
{
	localcmd("menu_cmd directmenu TouchUpdates\n");
}

void XonoticTouchHomeDialog_fill(entity me)
{
	entity e;

	me.TR(me);
		me.TDempty(me, (me.columns - 4) * 0.5);
		me.TD(me, 2, 4, e = makeXonoticBigButton(_("Play now"), '0 0 0'));
			e.onClick = TouchHome_PlayNow;
			e.onClickEntity = NULL;
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticTextLabel(0.5, _("Offline deathmatch with bots — one tap to jump in.")));
		e.allowWrap = true;
	me.TR(me);
	me.TR(me);
		me.TD(me, 2, 3, e = makeXonoticBigButton(_("Campaign"), '0 0 0'));
			e.onClick = TouchHome_OpenCampaign;
			e.onClickEntity = NULL;
		me.TD(me, 2, 3, e = makeXonoticBigButton(_("Multiplayer"), '0 0 0'));
			e.onClick = TouchHome_OpenMultiplayer;
			e.onClickEntity = NULL;
	me.TR(me);
	me.TR(me);
		me.TD(me, 1.5, 2, e = makeXonoticTouchCommandButton(_("Profile"), '0 0 0', "menu_cmd directmenu profile", 0));
		me.TD(me, 1.5, 2, e = makeXonoticTouchCommandButton(_("Controls"), '0 0 0', "menu_cmd directmenu touchsettings", 0));
		me.TD(me, 1.5, 2, e = makeXonoticTouchCommandButton(_("Settings"), '0 0 0', "menu_cmd directmenu settings", 0));

	// Version footer. Stock Xonotic shows its own git build tag here, which
	// names the engine rather than the build the player installed and cannot
	// answer "am I up to date". This shows the Touch version instead, and is
	// the way in to the update screen — the place people look for it.
	me.gotoRC(me, me.rows - 1, 0);
		me.TD(me, 1, me.columns, me.versionButton = makeXonoticButton(TouchUpdate_versionLine(), '0 0 0'));
			me.versionButton.onClick = TouchHome_OpenUpdates;
			me.versionButton.onClickEntity = NULL;
}

void XonoticTouchHomeDialog_draw(entity me)
{
	// Cheap poll: the status file changes at most once a second and only while
	// the updater is working, but the version has to be right the first frame
	// this screen is seen, before anyone has opened the update dialog.
	if (time >= me.nextVersionPoll)
	{
		me.nextVersionPoll = time + 2;
		TouchUpdate_read();

		string caption = TouchUpdate_versionLine();
		string status = TouchUpdate_shortStatus();
		// Only badge the button when there is something to act on. "Up to date"
		// on the home screen is noise; "4 versions behind" is not.
		if (TouchUpdate_isBehind() && status != "")
			caption = strcat(caption, " - ", status);
		if (caption != me.versionCaption)
		{
			strcpy(me.versionCaption, caption);
			me.versionButton.setText(me.versionButton, caption);
		}
	}

	SUPER(XonoticTouchHomeDialog).draw(me);
}
