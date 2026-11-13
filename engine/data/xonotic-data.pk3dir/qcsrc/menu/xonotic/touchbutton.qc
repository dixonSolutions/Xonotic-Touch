#include "touchbutton.qh"

entity makeGodnoticButton(string theText, vector theColor)
{
	entity me = NEW(GodnoticButton);
	me.configureXonoticButton(me, theText, theColor, string_null);
	return me;
}

entity makeGodnoticCommandButton(string theText, vector theColor, string theCommand, int theFlags)
{
	entity me = NEW(GodnoticCommandButton);
	me.configureXonoticCommandButton(me, theText, theColor, theCommand, theFlags, string_null);
	return me;
}
