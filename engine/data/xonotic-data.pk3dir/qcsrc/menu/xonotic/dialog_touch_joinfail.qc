#include "dialog_touch_joinfail.qh"

#include "dialog_touch_lan.qh"
#include "touchbutton.qh"

#define TOUCH_JOINFAIL_FONT (SKINFONTSIZE_NORMAL * 1.25)

// The detail sentence over two lines: cut at the last space that fits.
void TouchJoinFailed_Split(entity me, string s)
{
	float limit = 70; // characters that fit a line of this dialog at this font
	if (strlen(s) <= limit)
	{
		TouchLan_LabelText(me.joinDetail1, s);
		TouchLan_LabelText(me.joinDetail2, "");
		return;
	}
	float cut = limit;
	while (cut > 20 && substring(s, cut, 1) != " ")
		--cut;
	TouchLan_LabelText(me.joinDetail1, substring(s, 0, cut));
	TouchLan_LabelText(me.joinDetail2, substring(s, cut + 1, -1));
}

void GodnoticJoinFailedDialog_readInputArgs(entity me, int argsbuf)
{
	string addr = bufstr_get(argsbuf, 1);
	strcpy(me.joinAddress, addr);
	string text = cvar_string("_join_fail_text");
	if (text == "")
		text = sprintf(_("Could not join %s"), addr);
	TouchLan_LabelText(me.joinText, strcat("^3", text));
	TouchJoinFailed_Split(me, cvar_string("_join_fail_detail"));
}

void TouchJoinFailed_Retry(entity btn, entity me)
{
	if (me.joinAddress == "")
		return;
	string addr = me.joinAddress;
	Dialog_Close(btn, me);
	localcmd(sprintf("\nconnect \"%s\"\n", addr));
}

void TouchJoinFailed_Close(entity btn, entity me)
{
	Dialog_Close(btn, me);
}

entity TouchJoinFailed_Line(entity me)
{
	entity e = NEW(GodnoticLanLabel);
	e.configureLabel(e, "", TOUCH_JOINFAIL_FONT, 0);
	e.lanRole = -1; // text set by readInputArgs
	return e;
}

void GodnoticJoinFailedDialog_fill(entity me)
{
	entity e;
	me.TR(me);
		me.TD(me, 1, me.columns, me.joinText = TouchJoinFailed_Line(me));
	me.TR(me);
		me.TD(me, 1, me.columns, me.joinDetail1 = TouchJoinFailed_Line(me));
	me.TR(me);
		me.TD(me, 1, me.columns, me.joinDetail2 = TouchJoinFailed_Line(me));
	me.gotoRC(me, me.rows - 1.4, 0);
		me.TD(me, 1.4, me.columns / 2, e = makeGodnoticButton(_("Retry"), '0.55 1 0.55'));
			e.onClick = TouchJoinFailed_Retry;
			e.onClickEntity = me;
		me.TD(me, 1.4, me.columns / 2, e = makeGodnoticButton(_("Close"), '0 0 0'));
			e.onClick = TouchJoinFailed_Close;
			e.onClickEntity = me;
}

void TouchJoinFailed_Configure(entity me)
{
	entity i = NEW(GodnoticJoinFailedDialog);
	i.configureDialog(i);
	me.addItemCentered(me, i, i.intendedWidth * eX + i.intendedHeight * eY, SKINALPHAS_MAINMENU_z);
}
