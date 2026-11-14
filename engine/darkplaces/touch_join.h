/* Godnotic: never sign on to an unrenderable world after a map download fails. */
#ifndef TOUCH_JOIN_H
#define TOUCH_JOIN_H

#include "touch_datafetch.h"

static void TouchJoin_RequireWorld(qbool renderable, const char *mapname)
{
	if (gamemode == GAME_XONOTIC && !renderable)
	{
		char text[256];
		// a map of the data set that is still downloading: no error, the join
		// carries on when it is here (touch_datafetch.c)
		if (TouchData_JoinNeedsMap(mapname, text, sizeof(text)))
			Host_Error("%s", text);
		// A pre-signon disconnect has no in-game dialog yet. Leave the
		// console open so the recovery message remains visible and usable.
		key_consoleactive |= KEY_CONSOLEACTIVE_USER;
		Host_Error("Godnotic: cannot load map %.48s. Enable server downloads and reconnect.\n", mapname && *mapname ? mapname : "(unknown)");
	}
}

#endif
