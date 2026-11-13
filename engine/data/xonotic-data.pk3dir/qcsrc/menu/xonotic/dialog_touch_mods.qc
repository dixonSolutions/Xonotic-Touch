#include "dialog_touch_mods.qh"

#include "checkbox.qh"

const int TOUCH_MODS_MAX = 32;
const int TOUCH_MODS_PER_PAGE = 5;

// button roles
const int TOUCH_MODS_SWITCH = 0;
const int TOUCH_MODS_RELOAD = 1;
const int TOUCH_MODS_INFO = 2;
const int TOUCH_MODS_PREV = 3;
const int TOUCH_MODS_NEXT = 4;
const int TOUCH_MODS_RELOAD_ALL = 5;
const int TOUCH_MODS_IMPORT = 6;
// label roles
const int TOUCH_MODS_TITLE = 0;
const int TOUCH_MODS_DETAIL = 1;
const int TOUCH_MODS_SUMMARY = 2;

string touchMods_raw;
int touchMods_count;
int touchMods_page;
string touchMods_line[TOUCH_MODS_MAX];
string touchMods_name[TOUCH_MODS_MAX];
string touchMods_state[TOUCH_MODS_MAX];
string touchMods_title[TOUCH_MODS_MAX];
string touchMods_error[TOUCH_MODS_MAX];
string touchMods_desc[TOUCH_MODS_MAX];
string touchMods_importStatus;
float touchMods_importRead;

void TouchMods_refresh()
{
	string s = cvar_string("_cl_lua_mods");
	if (s == touchMods_raw)
		return;
	strcpy(touchMods_raw, s);
	// two passes: tokenize's buffer is shared, so copy the lines out first
	int n = tokenizebyseparator(s, "\n");
	int i, k = 0;
	for (i = 0; i < n && k < TOUCH_MODS_MAX; ++i)
	{
		if (argv(i) == "")
			continue;
		strcpy(touchMods_line[k], argv(i));
		++k;
	}
	touchMods_count = k;
	for (i = 0; i < touchMods_count; ++i)
	{
		tokenizebyseparator(touchMods_line[i], "\t");
		strcpy(touchMods_name[i], argv(0));
		strcpy(touchMods_state[i], argv(1));
		strcpy(touchMods_title[i], argv(2));
		strcpy(touchMods_error[i], argv(3));
		strcpy(touchMods_desc[i], argv(4));
	}
	if (touchMods_page * TOUCH_MODS_PER_PAGE >= touchMods_count)
		touchMods_page = max(0, floor((touchMods_count - 1) / TOUCH_MODS_PER_PAGE));
}

int TouchMods_pages()
{
	return max(1, ceil(touchMods_count / TOUCH_MODS_PER_PAGE));
}

// "" while the platform has no importer (not Android), else its last status line
string TouchMods_importStatus()
{
	if (time - touchMods_importRead < 1 && touchMods_importRead)
		return touchMods_importStatus;
	touchMods_importRead = time;
	string line = "";
	float fh = fopen(TOUCH_MODS_IMPORT_STATUS_FILE, FILE_READ);
	if (fh >= 0)
	{
		line = fgets(fh);
		// an empty line is "ready"; fgets gives "" for it, not a null
		// string, and "" must still say "this platform has an importer"
		if (line == "")
			line = " ";
		fclose(fh);
	}
	strcpy(touchMods_importStatus, line);
	return touchMods_importStatus;
}

bool TouchMods_on(int i)
{
	return touchMods_state[i] != "off";
}

string TouchMods_stateText(int i)
{
	string st = touchMods_state[i];
	if (st == "running")
		return strcat("^2", _("running"));
	if (st == "error")
		return strcat("^1", _("stopped by an error"));
	if (st == "enabled")
		return strcat("^3", _("on, not running (mods switched off)"));
	return strcat("^8", _("off"));
}

void TouchMods_setText(entity me, string t)
{
	if (t == me.modText)
		return;
	strcpy(me.modText, t);
	me.setText(me, me.modText);
}

void GodnoticModsButton_draw(entity me)
{
	TouchMods_refresh();
	int i = touchMods_page * TOUCH_MODS_PER_PAGE + me.modSlot;
	string t = "";
	switch (me.modRole)
	{
		case TOUCH_MODS_SWITCH: if (i < touchMods_count) t = (TouchMods_on(i) ? _("ON") : _("OFF")); break;
		case TOUCH_MODS_RELOAD: if (i < touchMods_count && TouchMods_on(i)) t = _("Reload"); break;
		case TOUCH_MODS_INFO: if (i < touchMods_count) t = _("Info"); break;
		case TOUCH_MODS_PREV: if (touchMods_page > 0) t = _("Back"); break;
		case TOUCH_MODS_NEXT: if (touchMods_page + 1 < TouchMods_pages()) t = _("More"); break;
		case TOUCH_MODS_RELOAD_ALL: if (touchMods_count) t = _("Reload all"); break;
		case TOUCH_MODS_IMPORT: if (TouchMods_importStatus() != "") t = _("Import mod"); break;
	}
	me.disabled = (t == "");
	TouchMods_setText(me, t);
	if (me.modRole == TOUCH_MODS_SWITCH && i < touchMods_count)
		me.color = (TouchMods_on(i) ? '0.55 1 0.55' : '1 1 1');
	if (t != "")
		SUPER(GodnoticModsButton).draw(me);
}

void GodnoticModsLabel_draw(entity me)
{
	TouchMods_refresh();
	int i = touchMods_page * TOUCH_MODS_PER_PAGE + me.modSlot;
	string t = "";
	if (me.modRole == TOUCH_MODS_SUMMARY)
	{
		string imp = TouchMods_importStatus();
		if (!touchMods_count)
			t = _("No mods yet. Put a mod's folder (with its init.lua) in the mods folder of the game's data folder; new folders start off.");
		else
			t = sprintf(_("%d mods, page %d of %d. A new mod starts off; turn it on here."), touchMods_count, touchMods_page + 1, TouchMods_pages());
		if (imp != "" && imp != " ")
			t = strcat(t, "  ^3", imp);
	}
	else if (i < touchMods_count)
	{
		if (me.modRole == TOUCH_MODS_TITLE)
			t = strcat("^7", touchMods_title[i], "  ", TouchMods_stateText(i));
		else if (touchMods_error[i] != "")
			t = strcat("^1", touchMods_error[i]);
		else
			t = strcat("^8", ((touchMods_desc[i] != "") ? touchMods_desc[i] : touchMods_name[i]));
	}
	TouchMods_setText(me, t);
	if (t != "")
		SUPER(GodnoticModsLabel).draw(me);
}

void TouchMods_click(entity btn, entity me)
{
	int i = touchMods_page * TOUCH_MODS_PER_PAGE + btn.modSlot;
	switch (btn.modRole)
	{
		case TOUCH_MODS_PREV:
			if (touchMods_page > 0) --touchMods_page;
			return;
		case TOUCH_MODS_NEXT:
			if (touchMods_page + 1 < TouchMods_pages()) ++touchMods_page;
			return;
		case TOUCH_MODS_RELOAD_ALL:
			localcmd("\nlua_mod_reload\n");
			return;
		case TOUCH_MODS_IMPORT:
		{
			float fh = fopen(TOUCH_MODS_IMPORT_REQUEST_FILE, FILE_WRITE);
			if (fh >= 0)
			{
				fputs(fh, "import\n");
				fclose(fh);
			}
			return;
		}
	}
	if (i >= touchMods_count)
		return;
	// names are a-z, 0-9 and _ only (the engine skips any other folder)
	string n = touchMods_name[i];
	switch (btn.modRole)
	{
		case TOUCH_MODS_SWITCH:
			localcmd(sprintf("\nlua_mod_%s %s\n", (TouchMods_on(i) ? "disable" : "enable"), n));
			break;
		case TOUCH_MODS_RELOAD:
			localcmd(sprintf("\nlua_mod_reload %s\n", n));
			break;
		case TOUCH_MODS_INFO:
			localcmd(sprintf("\nlua_mod_info %s\ntoggleconsole\n", n));
			break;
	}
}

entity TouchMods_button(int slot, int role)
{
	entity e = NEW(GodnoticModsButton);
	e.configureXonoticButton(e, "", '0 0 0', string_null);
	e.modSlot = slot;
	e.modRole = role;
	e.onClick = TouchMods_click;
	e.onClickEntity = e;
	return e;
}

entity TouchMods_label(int slot, int role)
{
	entity e = NEW(GodnoticModsLabel);
	e.configureLabel(e, "", e.fontSize, 0);
	e.modSlot = slot;
	e.modRole = role;
	return e;
}

entity makeGodnoticModsTab()
{
	entity me = NEW(GodnoticModsTab);
	me.configureDialog(me);
	return me;
}

void GodnoticModsTab_fill(entity me)
{
	entity e;
	int s;
	me.TR(me);
		me.TD(me, 1, 4, e = makeXonoticHeaderLabel(_("Lua mods")));
		me.TD(me, 1, 2, e = makeXonoticCheckBox(0, "cl_lua_enable", _("Run mods")));
	me.TR(me);
		me.TD(me, 1, me.columns, TouchMods_label(0, TOUCH_MODS_SUMMARY));
	for (s = 0; s < TOUCH_MODS_PER_PAGE; ++s)
	{
		me.gotoRC(me, 2.2 + s * 2, 0);
			me.TD(me, 1.8, 1, TouchMods_button(s, TOUCH_MODS_SWITCH));
			me.TD(me, 1, 3.2, TouchMods_label(s, TOUCH_MODS_TITLE));
			me.TD(me, 1.8, 0.9, TouchMods_button(s, TOUCH_MODS_RELOAD));
			me.TD(me, 1.8, 0.9, TouchMods_button(s, TOUCH_MODS_INFO));
		me.gotoRC(me, 3 + s * 2, 1);
			me.TD(me, 1, 3.2, TouchMods_label(s, TOUCH_MODS_DETAIL));
	}
	me.gotoRC(me, 12.4, 0);
		me.TD(me, 1.6, 1, TouchMods_button(0, TOUCH_MODS_PREV));
		me.TD(me, 1.6, 1, TouchMods_button(0, TOUCH_MODS_NEXT));
		me.TD(me, 1.6, 2, TouchMods_button(0, TOUCH_MODS_IMPORT));
		me.TD(me, 1.6, 2, TouchMods_button(0, TOUCH_MODS_RELOAD_ALL));
}
