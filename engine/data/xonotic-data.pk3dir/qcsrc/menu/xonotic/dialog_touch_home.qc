#include "dialog_touch_home.qh"

#include <common/gametypes/_mod.qh>
#include <common/mapinfo.qh>
#include "bigbutton.qh"
#include "touchbutton.qh"
#include "textlabel.qh"
#include "mainwindow.qh"
#include "touch_startup_util.qh"

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

void XonoticTouchHomeDialog_fill(entity me)
{
	entity e;

	me.TR(me);
		me.TDempty(me, (me.columns - 3) * 0.5);
		me.TD(me, 2, 3, e = makeXonoticBigButton(_("Play now"), '0 0 0'));
			e.onClick = TouchHome_PlayNow;
			e.onClickEntity = NULL;
	me.TR(me);
		me.TD(me, 1, 6, e = makeXonoticTextLabel(0.5, _("Offline deathmatch with bots — one tap to jump in.")));
		e.allowWrap = true;
	me.TR(me);
	me.TR(me);
		me.TD(me, 2, 2, e = makeXonoticTouchCommandButton(_("Touch setup"), '0 0 0', "menu_cmd directmenu TouchSetup", 0));
		me.TD(me, 2, 2, e = makeXonoticTouchCommandButton(_("Touch controls"), '0 0 0', "menu_cmd directmenu touchsettings", 0));
		me.TD(me, 2, 2, e = makeXonoticTouchCommandButton(_("Settings"), '0 0 0', "menu_cmd directmenu settings", 0));
}
