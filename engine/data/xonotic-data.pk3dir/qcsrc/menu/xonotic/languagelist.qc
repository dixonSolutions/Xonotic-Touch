#include "languagelist.qh"

#include "../item/modalcontroller.qh"
#include "mainwindow.qh"

const int LANGPARM_ID = 0;
const int LANGPARM_NAME = 1;
const int LANGPARM_NAME_LOCALIZED = 2;
const int LANGPARM_PERCENTAGE = 3;
const int LANGPARM_COUNT = 4;

const string PERCENTAGE_FULL_ICON = "/gfx/scoreboard/player_ready";

entity makeXonoticLanguageList()
{
	entity me = NEW(XonoticLanguageList);
	me.configureXonoticLanguageList(me);
	return me;
}

void XonoticLanguageList_configureXonoticLanguageList(entity me)
{
	me.configureXonoticListBox(me);
	me.getLanguages(me);
	me.loadCvars(me);
}

void XonoticLanguageList_drawListBoxItem(entity me, int i, vector absSize, bool isSelected, bool isFocused)
{
	if(isSelected)
		draw_Fill('0 0 0', '1 1 0', SKINCOLOR_LISTBOX_SELECTED, SKINALPHA_LISTBOX_SELECTED);
	else if(isFocused)
	{
		me.focusedItemAlpha = getFadedAlpha(me.focusedItemAlpha, SKINALPHA_LISTBOX_FOCUSED, SKINFADEALPHA_LISTBOX_FOCUSED);
		draw_Fill('0 0 0', '1 1 0', SKINCOLOR_LISTBOX_FOCUSED, me.focusedItemAlpha);
	}

	string p = me.languageParameter(me, i, LANGPARM_PERCENTAGE);
	float alpha_factor;
	if(p == "") // 100%
		alpha_factor = 1;
	else
	{
		float percent = stof(p);
		if (percent >= 90)
			alpha_factor = 1;
		else if (percent >= 50)
			alpha_factor = 0.65;
		else
			alpha_factor = 0.3;
		p = ftos_decimals_percentage(percent / 100, 0);
	}

	string s = me.languageParameter(me, i, LANGPARM_NAME_LOCALIZED);

	vector save_fontscale = draw_fontscale;
	float f = draw_CondensedFontFactor(s, false, me.realFontSize, 1);
	draw_fontscale.x *= f;
	vector fs = me.realFontSize;
	fs.x *= f;
	draw_Text(me.realUpperMargin * eY + me.columnNameOrigin * eX, s, fs, SKINCOLOR_TEXT * alpha_factor, SKINALPHA_TEXT, 0);
	draw_fontscale = save_fontscale;

	if(p == "")
	{
		float x_ofs = me.columnPercentageOrigin + (me.columnPercentageSize - me.columnIconSize);
		draw_Picture(me.realUpperMargin * eY + x_ofs * eX, PERCENTAGE_FULL_ICON, me.columnIconSize * eX + eY, '1 1 1', SKINALPHA_LISTBOX_SELECTED);
	}
	else
	{
		vector save_fontscale = draw_fontscale;
		float f = draw_CondensedFontFactor(p, false, me.realFontSize, 1);
		draw_fontscale.x *= f;
		vector fs = me.realFontSize;
		fs.x *= f;
		float x_ofs = me.columnPercentageOrigin + (me.columnPercentageSize - draw_TextWidth(p, 0, fs));
		draw_Text(me.realUpperMargin * eY + x_ofs * eX, p, fs, SKINCOLOR_TEXT, SKINALPHA_TEXT * alpha_factor, 0);
		draw_fontscale = save_fontscale;
	}
}

void XonoticLanguageList_resizeNotify(entity me, vector relOrigin, vector relSize, vector absOrigin, vector absSize)
{
	SUPER(XonoticLanguageList).resizeNotify(me, relOrigin, relSize, absOrigin, absSize);

	me.itemAbsSize.y = absSize.y * me.itemHeight;
	me.itemAbsSize.x = absSize.x * (1 - me.controlWidth);
	me.realFontSize.y = me.fontSize / (absSize.y * me.itemHeight);
	me.realFontSize.x = me.fontSize / (absSize.x * (1 - me.controlWidth));
	me.realUpperMargin = 0.5 * (1 - me.realFontSize.y);

	vector sz = draw_PictureSize(PERCENTAGE_FULL_ICON);
	float aspect_ratio = sz.y ? sz.x / sz.y : 1;
	me.columnIconSize = me.itemAbsSize.y / me.itemAbsSize.x * aspect_ratio;

	me.columnPercentageSize = me.realFontSize.x * 3;
	me.columnPercentageOrigin = 1 - me.columnPercentageSize - 0.5 * me.realFontSize.x;
	me.columnNameOrigin = 0.5 * me.realFontSize.x;
	me.columnNameSize = me.columnPercentageOrigin - me.columnNameOrigin;
}

void XonoticLanguageList_setSelected(entity me, float i)
{
	SUPER(XonoticLanguageList).setSelected(me, i);
	me.saveCvars(me);
}

void XonoticLanguageList_loadCvars(entity me)
{
	string s;
	float i, n;
	s = cvar_string("_menu_prvm_language");
	n = me.nItems;

	// default to English
	for(i = 0; i < n; ++i)
	{
		if(me.languageParameter(me, i, LANGPARM_ID) == "en")
		{
			SUPER(XonoticLanguageList).setSelected(me, i);
			break;
		}
	}

	// otherwise, find the language
	for(i = 0; i < n; ++i)
	{
		if(me.languageParameter(me, i, LANGPARM_ID) == s)
		{
			SUPER(XonoticLanguageList).setSelected(me, i);
			break;
		}
	}

	// save it off (turning anything unknown into "en")
	me.saveCvars(me);
}

void XonoticLanguageList_saveCvars(entity me)
{
	cvar_set("_menu_prvm_language", me.languageParameter(me, me.selectedItem, LANGPARM_ID));
}

void XonoticLanguageList_doubleClickListBoxItem(entity me, float i, vector where)
{
	m_play_click_sound(MENU_SOUND_EXECUTE);
	me.setLanguage(me);
}

float XonoticLanguageList_keyDown(entity me, float scan, float ascii, float shift)
{
	if(scan == K_ENTER || scan == K_KP_ENTER)
	{
		m_play_click_sound(MENU_SOUND_EXECUTE);
		me.setLanguage(me);
		return 1;
	}
	else
		return SUPER(XonoticLanguageList).keyDown(me, scan, ascii, shift);
}

void XonoticLanguageList_destroy(entity me)
{
	buf_del(me.languagelist);
}

void XonoticLanguageList_getLanguages(entity me)
{
	float buf, i, n, fh;
	string s;

	buf = buf_create();

	fh = fopen("languages.txt", FILE_READ);
	i = 0;
	while((s = fgets(fh)))
	{
		n = tokenize_console(s);
		if(n < 3)
			continue;
		bufstr_set(buf, i * LANGPARM_COUNT + LANGPARM_ID, argv(0));
		bufstr_set(buf, i * LANGPARM_COUNT + LANGPARM_NAME, argv(1));
		bufstr_set(buf, i * LANGPARM_COUNT + LANGPARM_NAME_LOCALIZED, argv(2));
		string percent = argv(3);
		if(percent && percent != "100%")
			bufstr_set(buf, i * LANGPARM_COUNT + LANGPARM_PERCENTAGE, percent);
		++i;
	}
	fclose(fh);

	me.languagelist = buf;
	me.nItems = i;
}

void XonoticLanguageList_setLanguage(entity me)
{
	if(prvm_language != cvar_string("_menu_prvm_language"))
	{
		if(!(gamestatus & GAME_CONNECTED))
			localcmd("\nprvm_language \"$_menu_prvm_language\"; menu_restart; menu_cmd languageselect\n");
		else
			DialogOpenButton_Click(me, main.languageWarningDialog);
	}
}

string XonoticLanguageList_languageParameter(entity me, float i, float key)
{
	return bufstr_get(me.languagelist, i * LANGPARM_COUNT + key);
}

void SetLanguage_Click(entity btn, entity me)
{
	me.setLanguage(me);
}

void XonoticLanguageList_focusedItemChangeNotify(entity me)
{
	if (me.focusedItem >= 0)
		setZonedTooltip(me, me.languageParameter(me, me.focusedItem, LANGPARM_NAME), string_null);
	else
		clearTooltip(me);
}

/** "pl_PL" -> "pl"; "ast" -> "ast". */
string LanguageList_baseLanguage(string id)
{
	float u = strstrofs(id, "_", 0);
	return (u > 0) ? substring(id, 0, u) : id;
}

string LanguageList_bestSystemMatch(string tags)
{
	// Tags as browsers and systems write them ("pl-PL", "en_AU.UTF-8"), most preferred first.
	float want = buf_create();
	float m = tokenizebyseparator(tags, ",");
	for (int i = 0; i < m; ++i)
	{
		string tag = strtolower(strreplace("-", "_", argv(i)));
		float dot = strstrofs(tag, ".", 0);
		if (dot > 0) tag = substring(tag, 0, dot);
		bufstr_set(want, i, tag);
	}

	float ids = buf_create();
	float n = 0;
	float fh = fopen("languages.txt", FILE_READ);
	if (fh >= 0)
	{
		string line;
		while ((line = fgets(fh)))
		{
			if (tokenize_console(line) < 3) continue;
			bufstr_set(ids, n, argv(0));
			++n;
		}
		fclose(fh);
	}

	// For each preferred tag in turn: the exact language and region, then the language
	// itself, then any regional variant of it ("id" finds "id_ID").
	string found = "";
	for (int i = 0; i < m && found == ""; ++i)
	{
		string tag = bufstr_get(want, i);
		if (tag == "") continue;
		string base = LanguageList_baseLanguage(tag);
		for (int pass = 0; pass < 3 && found == ""; ++pass)
		{
			for (int k = 0; k < n; ++k)
			{
				string id = bufstr_get(ids, k);
				string lid = strtolower(id);
				if ((pass == 0 && lid == tag)
					|| (pass == 1 && lid == base)
					|| (pass == 2 && LanguageList_baseLanguage(lid) == base))
				{
					found = id;
					break;
				}
			}
		}
	}

	buf_del(want);
	buf_del(ids);
	return found;
}

bool LanguageList_applySystemLanguage()
{
	// Once per install: after this the player's own choice (saved with the profile) stands.
	if (cvar("_menu_language_from_system")) return false;
	cvar_set("_menu_language_from_system", "1");

	string tags = cvar_string("_menu_system_languages");
	if (tags == "") return false;
	string lang = LanguageList_bestSystemMatch(tags);
	if (lang == "" || lang == prvm_language) return false;

	cvar_set("_menu_prvm_language", lang);
	localcmd("\nprvm_language \"$_menu_prvm_language\"; menu_restart\n");
	return true;
}
