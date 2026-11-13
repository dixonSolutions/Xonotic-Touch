/* Godnotic: never sign on to an unrenderable world after a map download fails. */
#ifndef TOUCH_JOIN_H
#define TOUCH_JOIN_H

static void TouchJoin_RequireWorld(qbool renderable, const char *mapname)
{
	if (gamemode == GAME_XONOTIC && !renderable)
	{
		// A pre-signon disconnect has no in-game dialog yet. Leave the
		// console open so the recovery message remains visible and usable.
		key_consoleactive |= KEY_CONSOLEACTIVE_USER;
		Host_Error("Godnotic: cannot load map %.48s. Enable server downloads and reconnect.\n", mapname && *mapname ? mapname : "(unknown)");
	}
}

#endif
