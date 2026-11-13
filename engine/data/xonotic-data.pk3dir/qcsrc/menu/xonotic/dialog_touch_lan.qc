#include "dialog_touch_lan.qh"

#include "checkbox.qh"
#include "inputbox.qh"
#include "slider.qh"
#include "textslider.qh"
#include "serverlist.qh"
#include "maplist.qh"
#include <common/mapinfo.qh>

// label roles
const int TOUCH_LAN_STATUS = 0;
const int TOUCH_LAN_ADDRESSES = 1;
const int TOUCH_LAN_PLAYERS = 2;
const int TOUCH_LAN_PASSWORD_NOTE = 3;
// button roles
const int TOUCH_LAN_START = 0;
const int TOUCH_LAN_STOP = 1;

#define TOUCH_LAN_FONT (SKINFONTSIZE_NORMAL * 1.35)

string touchLan_message;    // the last Start problem or note, shown until hosting starts
entity touchLan_portBox;    // to show the port Start switched to
entity touchLan_passwordBox;

bool TouchLan_Available()
{
	// 0 in the browser (no UDP) and on an engine without LAN support.
	return cvar("touch_lan_available") != 0;
}

bool TouchLan_Hosting()
{
	return cvar("touch_lan_hosting") != 0;
}

void TouchLan_SetMessage(string s)
{
	strcpy(touchLan_message, s);
}

void TouchLan_LabelText(entity me, string t)
{
	if (t == me.lanText)
		return;
	strcpy(me.lanText, t);
	me.setText(me, me.lanText);
}

void GodnoticLanLabel_draw(entity me)
{
	string t = "", s;
	if (me.lanRole < 0)
	{
		// Text set from outside (the password prompt's message).
		if (me.lanText != "")
			SUPER(GodnoticLanLabel).draw(me);
		return;
	}
	switch (me.lanRole)
	{
		case TOUCH_LAN_STATUS:
			if (!TouchLan_Available())
				t = _("^3LAN games need UDP networking, which a web browser does not have. Host and join them in the app.");
			else if (TouchLan_Hosting())
				t = sprintf(_("^2Hosting^7 on UDP port %d%s. Players: %d"), cvar("touch_lan_port"),
					(cvar("touch_lan_locked") ? _(", password protected") : ""), cvar("touch_lan_playercount"));
			else if (touchLan_message != "")
				t = touchLan_message;
			else
				t = _("Not hosting. Others on this Wi-Fi find the game under LAN.");
			break;
		case TOUCH_LAN_ADDRESSES:
			if (TouchLan_Hosting())
			{
				s = cvar_string("touch_lan_addresses");
				t = (s != "") ? strcat(_("Join at: ^3"), s)
					: _("^1No network: join a Wi-Fi or turn on this device's hotspot");
			}
			break;
		case TOUCH_LAN_PLAYERS:
			if (TouchLan_Hosting())
				t = strcat(_("In the game: "), cvar_string("touch_lan_players"));
			break;
		case TOUCH_LAN_PASSWORD_NOTE:
			t = cvar("touch_lan_host_password_set")
				? _("^2A password is set.^7 Type a new one to change it, or Clear.")
				: _("Optional. Leave empty for an open game.");
			break;
	}
	TouchLan_LabelText(me, t);
	if (t != "")
		SUPER(GodnoticLanLabel).draw(me);
}

void GodnoticLanButton_draw(entity me)
{
	if (me.lanRole == TOUCH_LAN_START)
		me.disabled = !TouchLan_Available();
	else
		me.disabled = !TouchLan_Hosting();
	SUPER(GodnoticLanButton).draw(me);
}

entity TouchLan_label(int role)
{
	entity e = NEW(GodnoticLanLabel);
	e.configureLabel(e, "", TOUCH_LAN_FONT, 0);
	e.lanRole = role;
	return e;
}

entity TouchLan_text(string t)
{
	entity e = makeXonoticTextLabel(0, t);
	e.fontSize = TOUCH_LAN_FONT;
	e.allowColors = true;
	return e;
}

string TouchLan_clean(string s)
{
	// The name goes into a quoted console command.
	s = strreplace("\"", "'", s);
	s = strreplace("\n", " ", s);
	s = strreplace("\\", "/", s);
	return s;
}

void TouchLan_Start(entity btn, entity me)
{
	if (!TouchLan_Available())
		return;
	// Inputs keep their text until they lose focus; Start must see it now.
	if (touchLan_portBox)
		touchLan_portBox.saveCvars(touchLan_portBox);
	if (touchLan_passwordBox && touchLan_passwordBox.text != "")
	{
		cvar_set("touch_lan_host_password", touchLan_passwordBox.text);
		touchLan_passwordBox.setText(touchLan_passwordBox, "");
	}

	string portstr = cvar_string("touch_lan_host_port");
	float port = stof(portstr);
	if (portstr == "" || port != floor(port) || port < 1024 || port > 65535)
	{
		TouchLan_SetMessage(_("^1The port must be a whole number from 1024 to 65535; 26000 is the usual one."));
		return;
	}
	cvar_set("_touch_lan_portcheck", ftos(port));
	if (!cvar("_touch_lan_port_free"))
	{
		float next = cvar("_touch_lan_port_suggest");
		if (next != port)
		{
			cvar_set("touch_lan_host_port", ftos(next));
			if (touchLan_portBox)
				touchLan_portBox.loadCvars(touchLan_portBox);
			TouchLan_SetMessage(sprintf(_("^3Port %d is in use by another game or app on this device. Switched to %d: press Start again."), port, next));
		}
		else
			TouchLan_SetMessage(sprintf(_("^1Port %d is in use and none of the next ones is free. Choose another port."), port));
		return;
	}

	Gametype gt = MapInfo_Type_FromString(cvar_string("touch_lan_gametype"), false, false);
	if (!gt)
		gt = MAPINFO_TYPE_DEATHMATCH;
	MapInfo_FilterGametype(gt, 0, MapInfo_RequiredFlags(), MapInfo_ForbiddenFlags(), 0);
	if (MapInfo_count <= 0)
	{
		TouchLan_SetMessage(_("^1No map you have supports this game mode. Choose another mode."));
		return;
	}
	string want = cvar_string("touch_lan_map"), m = "";
	for (int i = 0; i < MapInfo_count; ++i)
		if (MapInfo_BSPName_ByID(i) == want)
			m = want;
	if (m == "")
	{
		if (want != "")
			TouchLan_SetMessage(sprintf(_("^3%s has no %s; a random map that does is played."), want, MapInfo_Type_ToText(gt)));
		m = MapInfo_BSPName_ByID(floor(random() * MapInfo_count));
	}

	int bots = bound(0, cvar("touch_lan_bots"), 32);
	// The bots take slots too: leave at least one for another player.
	int slots = bound(2, max(cvar("touch_lan_maxplayers"), bots + 2), 64);
	localcmd("\ndisconnect; wait; g_campaign 0\n");
	localcmd(sprintf("maxplayers %d\n", slots));
	localcmd(sprintf("port %d\n", port));
	// sv_public 0: answers the LAN search (getinfo/getstatus) and registers
	// with no master server; -1 would hide the game from the LAN search too.
	localcmd("sv_public 0\nsv_status_privacy 1\n");
	localcmd(sprintf("hostname \"%s\"\n", TouchLan_clean(cvar_string("touch_lan_hostname"))));
	localcmd(sprintf("bot_number %d\n", bots));
	localcmd(sprintf("set sv_touch_assist %d\n", bound(0, cvar("touch_lan_assist"), 7)));
	localcmd("touch_lan_host_password_apply\n");
	MapInfo_SwitchGameType(gt);
	MapInfo_LoadMap(m, 1);
}

void TouchLan_Stop(entity btn, entity me)
{
	localcmd("\ndisconnect\n");
	cvar_set("sv_touch_lan_password", "");
	TouchLan_SetMessage("");
}

void TouchLan_ClearPassword(entity btn, entity me)
{
	cvar_set("touch_lan_host_password", "");
	if (touchLan_passwordBox)
		touchLan_passwordBox.setText(touchLan_passwordBox, "");
}

void TouchLan_Close(entity btn, entity me)
{
	Dialog_Close(btn, me);
}

void GodnoticLanHostDialog_fill(entity me)
{
	entity e;
	int i;
	string m;

	me.TR(me);
		me.TD(me, 1, 1.4, TouchLan_text(_("Server name:")));
		me.TD(me, 1, 4.6, e = makeXonoticInputBox(0, "touch_lan_hostname"));
			e.fontSize = TOUCH_LAN_FONT;
			e.saveImmediately = true;
	me.TR(me);
		me.TD(me, 1, 1.4, TouchLan_text(_("Game mode:")));
		me.TD(me, 1, 1.6, e = makeXonoticTextSlider("touch_lan_gametype"));
			e.fontSize = TOUCH_LAN_FONT;
			e.valueSpace = 0.6;
			e.addValue(e, _("Deathmatch"), "dm");
			e.addValue(e, _("Team Deathmatch"), "tdm");
			e.addValue(e, _("Capture the Flag"), "ctf");
			e.addValue(e, _("Clan Arena"), "ca");
			e.addValue(e, _("Freeze Tag"), "ft");
			e.addValue(e, _("Duel"), "duel");
			e.addValue(e, _("Last Man Standing"), "lms");
			e.addValue(e, _("Keepaway"), "ka");
			e.addValue(e, _("Key Hunt"), "kh");
			e.addValue(e, _("Domination"), "dom");
			e.configureXonoticTextSliderValues(e);
		me.TD(me, 1, 0.8, TouchLan_text(_("Map:")));
		me.TD(me, 1, 2.2, e = makeXonoticTextSlider("touch_lan_map"));
			e.fontSize = TOUCH_LAN_FONT;
			e.valueSpace = 0.5;
			e.addValue(e, _("Random"), "");
			_MapInfo_FilterGametype(MAPINFO_TYPE_ALL, 0, 0, MapInfo_ForbiddenFlags(), 0);
			for (i = 0; i < MapInfo_count; ++i)
			{
				m = MapInfo_BSPName_ByID(i);
				if (m != "")
					e.addValue(e, strzone(m), strzone(m));
			}
			e.configureXonoticTextSliderValues(e);
	me.TR(me);
		me.TD(me, 1, 1.4, TouchLan_text(_("Bots:")));
		me.TD(me, 1, 1.6, e = makeXonoticSlider(0, 16, 1, "touch_lan_bots"));
			e.fontSize = TOUCH_LAN_FONT;
		me.TD(me, 1, 0.8, TouchLan_text(_("Slots:")));
		me.TD(me, 1, 2.2, e = makeXonoticSlider(2, 32, 1, "touch_lan_maxplayers"));
			e.fontSize = TOUCH_LAN_FONT;
	me.TR(me);
		me.TD(me, 1, 1.4, TouchLan_text(_("Port:")));
		me.TD(me, 1, 1.6, e = makeXonoticInputBox(0, "touch_lan_host_port"));
			e.fontSize = TOUCH_LAN_FONT;
			e.saveImmediately = true;
			e.tooltip = _("UDP; 26000 unless it is taken");
			touchLan_portBox = e;
		me.TD(me, 1, 1.1, TouchLan_text(_("Assists:")));
		me.TD(me, 1, 1.9, e = makeXonoticTextSlider("touch_lan_assist"));
			e.fontSize = TOUCH_LAN_FONT;
			e.valueSpace = 0.55;
			e.tooltip = _("The touch assists players in this game may use");
			e.addValue(e, _("All allowed"), "7");
			e.addValue(e, _("No auto-shoot"), "5");
			e.addValue(e, _("Aim assist only"), "1");
			e.addValue(e, _("None"), "0");
			e.configureXonoticTextSliderValues(e);
	me.TR(me);
		me.TD(me, 1, 1.4, TouchLan_text(_("Password:")));
		me.TD(me, 1, 1.6, e = makeXonoticInputBox(0, string_null));
			e.fontSize = TOUCH_LAN_FONT;
			e.onEnter = TouchLan_Start;
			e.onEnterEntity = me;
			touchLan_passwordBox = e;
		me.TD(me, 1, 0.8, e = makeGodnoticButton(_("Clear"), '0 0 0'));
			e.onClick = TouchLan_ClearPassword;
			e.onClickEntity = me;
		me.TD(me, 1, 2.2, e = makeXonoticCheckBox(0, "touch_lan_host_password_remember", _("Remember on this device")));
			e.fontSize = TOUCH_LAN_FONT;
	me.TR(me);
		me.TDempty(me, 1.4);
		me.TD(me, 1, 4.6, TouchLan_label(TOUCH_LAN_PASSWORD_NOTE));
	me.TR(me);
		me.TD(me, 1, me.columns, TouchLan_label(TOUCH_LAN_STATUS));
	me.TR(me);
		me.TD(me, 1, me.columns / 2, TouchLan_label(TOUCH_LAN_ADDRESSES));
		me.TD(me, 1, me.columns / 2, TouchLan_label(TOUCH_LAN_PLAYERS));

	me.gotoRC(me, me.rows - 1.3, 0);
		me.TD(me, 1.3, 2, e = NEW(GodnoticLanButton));
			e.configureXonoticButton(e, _("Start hosting"), '0.55 1 0.55', string_null);
			e.lanRole = TOUCH_LAN_START;
			e.onClick = TouchLan_Start;
			e.onClickEntity = me;
		me.TD(me, 1.3, 2, e = NEW(GodnoticLanButton));
			e.configureXonoticButton(e, _("Stop hosting"), '1 0.55 0.55', string_null);
			e.lanRole = TOUCH_LAN_STOP;
			e.onClick = TouchLan_Stop;
			e.onClickEntity = me;
		me.TD(me, 1.3, 2, e = makeGodnoticButton(_("Back"), '0 0 0'));
			e.onClick = TouchLan_Close;
			e.onClickEntity = me;
}

// ---------------------------------------------------------------- password prompt

void GodnoticLanPasswordDialog_readInputArgs(entity me, int argsbuf)
{
	string kind = bufstr_get(argsbuf, 0);
	string addr = bufstr_get(argsbuf, 1);
	strcpy(me.lanAddress, addr);
	string msg = (kind == "wrong")
		? sprintf(_("^1Wrong password^7 for %s. Try again."), addr)
		: sprintf(_("%s needs a password to join."), addr);
	TouchLan_LabelText(me.lanMessage, msg);
	me.lanInput.setText(me.lanInput, "");
}

void GodnoticLanPasswordDialog_showNotify(entity me)
{
	SUPER(GodnoticLanPasswordDialog).showNotify(me);
	// Ready to type: on a touchscreen this brings up the keyboard. Only when
	// the prompt itself has focus: the menu also notifies dialogs that are
	// not open (m_display on the in-game MENU), and setFocus in an unfocused
	// container is a menu QC error that drops to the engine menu.
	if (me.focused)
		me.setFocus(me, me.lanInput);
}

void TouchLan_PasswordJoin(entity btn, entity me)
{
	string pw = me.lanInput.text;
	if (pw == "" || me.lanAddress == "")
		return;
	cvar_set("_cl_touch_lan_password", pw);
	cvar_set("_cl_touch_lan_password_server", me.lanAddress);
	me.lanInput.setText(me.lanInput, "");
	localcmd(sprintf("\nconnect \"%s\"\n", me.lanAddress));
	Dialog_Close(btn, me);
}

void TouchLan_PasswordJoinEnter(entity box, entity me)
{
	TouchLan_PasswordJoin(box, me);
}

void GodnoticLanPasswordDialog_fill(entity me)
{
	entity e;
	me.TR(me);
		me.TD(me, 1, me.columns, e = NEW(GodnoticLanLabel));
			e.configureLabel(e, "", TOUCH_LAN_FONT, 0);
			e.lanRole = -1; // text set by readInputArgs
			me.lanMessage = e;
	me.TR(me);
		me.TD(me, 1.3, me.columns, e = makeXonoticInputBox(0, string_null));
			e.fontSize = TOUCH_LAN_FONT * 1.2;
			e.onEnter = TouchLan_PasswordJoinEnter;
			e.onEnterEntity = me;
			me.lanInput = e;
	me.gotoRC(me, 2.5, 0);
		me.TD(me, 1, me.columns, e = makeXonoticCheckBox(0, "cl_touch_lan_password_remember", _("Remember it on this device (kept as plain text)")));
			e.fontSize = TOUCH_LAN_FONT;
	me.TR(me);
		me.TD(me, 1, me.columns, TouchLan_text(_("^8Sent to this game's host only, as a proof, never as text.")));
	me.gotoRC(me, me.rows - 1.3, 0);
		me.TD(me, 1.3, me.columns / 2, e = makeGodnoticButton(_("Join"), '0.55 1 0.55'));
			e.onClick = TouchLan_PasswordJoin;
			e.onClickEntity = me;
		me.TD(me, 1.3, me.columns / 2, e = makeGodnoticButton(_("Cancel"), '0 0 0'));
			e.onClick = TouchLan_Close;
			e.onClickEntity = me;
}

// ---------------------------------------------------------------- server list hooks

bool TouchLan_IsLanAddress(string a)
{
	// IPv4 private, link-local, CGNAT-shared and loopback (touch_lan_logic.h)
	int n = tokenizebyseparator(a, ".", ":");
	if (n < 4)
		return false;
	float b0 = stof(argv(0)), b1 = stof(argv(1));
	return b0 == 10 || b0 == 127
		|| (b0 == 172 && b1 >= 16 && b1 <= 31)
		|| (b0 == 192 && b1 == 168)
		|| (b0 == 169 && b1 == 254)
		|| (b0 == 100 && b1 >= 64 && b1 <= 127);
}

bool TouchLan_Locked(int entry)
{
	string s = gethostcachestring(SLIST_FIELD_QCSTATUS, entry);
	return strstrofs(strcat(s, ":"), ":L1:", 0) >= 0;
}

int TouchLan_Category(int entry)
{
	if (TouchLan_IsLanAddress(gethostcachestring(SLIST_FIELD_CNAME, entry)))
		return CAT_LAN;
	return 0;
}

void TouchLan_DrawLock(int entry, vector pos, vector size)
{
	if (!TouchLan_Locked(entry))
		return;
	// A padlock: shackle (two posts and a bar) over a body.
	vector col = '1 0.85 0.3';
	vector body = pos + eX * size.x * 0.2 + eY * size.y * 0.45;
	vector bsz = eX * size.x * 0.6 + eY * size.y * 0.45;
	draw_Fill(body, bsz, col, 1);
	vector post = eX * size.x * 0.1 + eY * size.y * 0.3;
	draw_Fill(pos + eX * size.x * 0.3 + eY * size.y * 0.15, post, col, 1);
	draw_Fill(pos + eX * size.x * 0.6 + eY * size.y * 0.15, post, col, 1);
	draw_Fill(pos + eX * size.x * 0.3 + eY * size.y * 0.1, eX * size.x * 0.4 + eY * size.y * 0.08, col, 1);
	// keyhole
	draw_Fill(body + eX * bsz.x * 0.42 + eY * bsz.y * 0.3, eX * bsz.x * 0.16 + eY * bsz.y * 0.4, '0 0 0', 0.8);
}

bool TouchLan_Connect(string address)
{
	if (address == "")
		return false;
	int n = gethostcachevalue(SLIST_HOSTCACHEVIEWCOUNT);
	for (int i = 0; i < n; ++i)
	{
		if (gethostcachestring(SLIST_FIELD_CNAME, i) != address)
			continue;
		if (!TouchLan_Locked(i))
			return false;
		cvar_set("_touch_lan_haspw_query", address);
		if (cvar("_touch_lan_haspw"))
			return false;
		localcmd(sprintf("\nmenu_cmd directmenu TouchLanPassword required \"%s\"\n", address));
		return true;
	}
	return false;
}

void TouchLan_OpenHost(entity btn, entity me)
{
	m_goto("TouchLanHost", true);
}

void TouchLan_LanOnly_Click(entity box, entity slist)
{
	box.setChecked(box, !box.checked);
	slist.refreshServerList(slist, REFRESHSERVERLIST_RESET);
}

void TouchLan_ConfigureDialogs(entity me)
{
	entity i;
	i = NEW(GodnoticLanHostDialog);
	i.configureDialog(i);
	me.addItemCentered(me, i, i.intendedWidth * eX + i.intendedHeight * eY, SKINALPHAS_MAINMENU_z);
	i = NEW(GodnoticLanPasswordDialog);
	i.configureDialog(i);
	me.addItemCentered(me, i, i.intendedWidth * eX + i.intendedHeight * eY, SKINALPHAS_MAINMENU_z);
}

// ---------------------------------------------------------------- Create: LAN
//
// Multiplayer > Create hosts on the LAN when its LAN box is ticked: the port
// (26000 unless taken) and an optional password with a switch, an open lock
// when no password applies and a closed one when it does. Start then sets
// up the LAN server as the Host screen did and starts the chosen map.

entity touchCreate_port, touchCreate_pass;

void GodnoticLanLock_draw(entity me)
{
	bool locked = cvar("touch_lan_create_password") && ((touchCreate_pass && touchCreate_pass.text != "") || cvar("touch_lan_host_password_set"));
	vector col = locked ? '1 0.85 0.3' : '0.7 0.8 0.9';
	float a = cvar("touch_lan_create") ? 1 : 0.35;
	// A square box, centred in the item.
	float w = me.size.x, h = me.size.y;
	float side = min(w, h) * 0.8;
	vector sz = eX * (side / w) + eY * (side / h);
	vector pos = eX * (0.5 * (1 - sz.x)) + eY * (0.5 * (1 - sz.y));
	draw_Fill(pos + eX * sz.x * 0.2 + eY * sz.y * 0.45, eX * sz.x * 0.6 + eY * sz.y * 0.45, col, a);
	// The shackle: shut over the body, or lifted and open on the right.
	float lift = locked ? 0 : 0.12;
	vector post = eX * sz.x * 0.1 + eY * sz.y * 0.3;
	draw_Fill(pos + eX * sz.x * 0.3 + eY * sz.y * (0.15 - lift), post, col, a);
	if (locked)
		draw_Fill(pos + eX * sz.x * 0.6 + eY * sz.y * 0.15, post, col, a);
	draw_Fill(pos + eX * sz.x * 0.3 + eY * sz.y * (0.1 - lift), eX * sz.x * 0.4 + eY * sz.y * 0.08, col, a);
}

void TouchCreate_Start(entity btn, entity maplist)
{
	if (cvar("touch_lan_create") && TouchLan_Available())
	{
		if (touchCreate_port)
			touchCreate_port.saveCvars(touchCreate_port);
		string portstr = cvar_string("touch_lan_host_port");
		float port = stof(portstr);
		if (portstr == "" || port != floor(port) || port < 1024 || port > 65535)
		{
			TouchLan_SetMessage(_("^1The port must be a whole number from 1024 to 65535; 26000 is the usual one."));
			return;
		}
		cvar_set("_touch_lan_portcheck", ftos(port));
		if (!cvar("_touch_lan_port_free"))
		{
			float next = cvar("_touch_lan_port_suggest");
			if (next != port)
			{
				cvar_set("touch_lan_host_port", ftos(next));
				if (touchCreate_port)
					touchCreate_port.loadCvars(touchCreate_port);
				TouchLan_SetMessage(sprintf(_("^3Port %d is in use by another game or app on this device. Switched to %d: press Start again."), port, next));
			}
			else
				TouchLan_SetMessage(sprintf(_("^1Port %d is in use and none of the next ones is free. Choose another port."), port));
			return;
		}
		if (!cvar("touch_lan_create_password"))
			cvar_set("touch_lan_host_password", "");
		else if (touchCreate_pass && touchCreate_pass.text != "")
		{
			cvar_set("touch_lan_host_password", touchCreate_pass.text);
			touchCreate_pass.setText(touchCreate_pass, "");
		}
		localcmd(sprintf("\nport %d\n", port));
		// sv_public 0: answers the LAN search and registers with no master.
		localcmd("sv_public 0\nsv_status_privacy 1\n");
		localcmd("touch_lan_host_password_apply\n");
		TouchLan_SetMessage("");
	}
	MapList_LoadMap(btn, maplist);
}

void TouchCreate_Reflow(entity me)
{
	if (!touch_menu_on)
		return;
	registercvar("touch_lan_create", "0", CVAR_SAVE);
	registercvar("touch_lan_create_password", "0", CVAR_SAVE);
	// Room on the left: the game type list 4 rows shorter, what is under it
	// 4 rows up (the rows stay clear of the pinned Mutators / Start rows).
	float d = 4 * me.itemSpacing.y;
	float listbottom = me.itemOrigin.y + 12 * me.itemSpacing.y;
	float bottomrow = me.itemOrigin.y + (me.rows - 1.5) * me.itemSpacing.y;
	for (entity e = me.firstChild; e; e = e.nextSibling)
	{
		if (e.Container_origin.x > 0.45 || e.Container_origin.y > bottomrow)
			continue;
		if (e.instanceOfListBox)
			e.Container_size_y -= d;
		else if (e.Container_origin.y > listbottom)
			e.Container_origin_y -= d;
	}
	entity e;
	me.gotoRC(me, 17.5, 0);
		me.TD(me, 1, 0.9, e = makeXonoticCheckBox_T(0, "touch_lan_create", _("LAN"), _("Host for this Wi-Fi or network: others find the game under LAN")));
			setDependent(e, "touch_lan_available", 1, 1);
		me.TD(me, 1, 0.5, e = makeXonoticTextLabel(1, _("Port:")));
		me.TD(me, 1, 1.1, e = touchCreate_port = makeXonoticInputBox(0, "touch_lan_host_port"));
			e.saveImmediately = true;
			e.tooltip = _("UDP; 26000 unless it is taken");
			setDependent(e, "touch_lan_create", 1, 1);
		me.TD(me, 1, 0.4, e = NEW(GodnoticLanLock));
	me.gotoRC(me, 18.5, 0);
		me.TD(me, 1, 1.3, e = makeXonoticCheckBox_T(0, "touch_lan_create_password", _("Password"), _("Only players who know it can join")));
			setDependent(e, "touch_lan_create", 1, 1);
		me.TD(me, 1, 1.6, e = touchCreate_pass = makeXonoticInputBox(0, string_null));
			setDependent(e, "touch_lan_create_password", 1, 1);
	me.gotoRC(me, 19.5, 0);
		me.TD(me, 1, 2.9, TouchLan_label(TOUCH_LAN_STATUS));
	// Start hosts on the LAN when the box is ticked.
	if (me.mapListBox && me.mapListBox.startButton)
		me.mapListBox.startButton.onClick = TouchCreate_Start;
}

entity TouchLan_StatusLabel()
{
	return TouchLan_label(TOUCH_LAN_ADDRESSES);
}

entity TouchLan_StopButton()
{
	entity e = NEW(GodnoticLanButton);
	e.configureXonoticButton(e, _("Stop LAN"), '1 0.55 0.55', string_null);
	e.lanRole = TOUCH_LAN_STOP;
	e.onClick = TouchLan_Stop;
	e.onClickEntity = e;
	return e;
}
