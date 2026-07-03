#include "touchbutton.qh"

entity makeXonoticTouchButton(string theText, vector theColor)
{
	entity me = NEW(XonoticTouchButton);
	me.configureXonoticButton(me, theText, theColor, string_null);
	return me;
}

entity makeXonoticTouchCommandButton(string theText, vector theColor, string theCommand, int theFlags)
{
	entity me = NEW(XonoticTouchCommandButton);
	me.configureXonoticCommandButton(me, theText, theColor, theCommand, theFlags, string_null);
	return me;
}
