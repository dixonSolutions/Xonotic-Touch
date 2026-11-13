// Godnotic: mount a pk3 that arrived while the game runs (fs_mountpack).
// Included by fs.c just before FS_Init_Commands, which calls
// TouchFS_Init_Commands (one hook line each).
//
// The phone data comes as a core pack, per-map packs, music and extras
// (docs/PHONE_ASSETS.md, "Split set"). The app starts the game on the core
// and the first map, then downloads the rest in the background and mounts
// each pk3 as it lands with "fs_mountpack <name.pk3>".
//
// fs_rescan would do it too, but it closes and reopens every pack in the
// search path. This adds the one pack at the END of the search path: the
// lowest priority, so a pack mounted late can only add names, never shadow a
// file the game already uses (the app's own progs and configs in
// xonotic-data.pk3dir, or anything in an earlier pack). The split packs share
// no names, so for them the order makes no other difference.
//
// fs_mountpack_count counts successful mounts; the menu QuakeC can watch it
// to refresh its map list (maps whose pack is installed become playable).

static cvar_t fs_mountpack_count = {CF_CLIENT | CF_SERVER | CF_READONLY, "fs_mountpack_count", "0", "number of packs fs_mountpack added since start-up (Godnotic)"};

static void TouchFS_MountPack_f(cmd_state_t *cmd)
{
	const char *name;
	char fullpath[MAX_OSPATH];
	searchpath_t *search, *last;
	pack_t *pak;
	int index;
	size_t len;

	if (Cmd_Argc(cmd) != 2)
	{
		Con_Printf("usage: fs_mountpack <name.pk3>  (a pk3 in the data directory, added at the lowest priority)\n");
		return;
	}
	name = Cmd_Argv(cmd, 1);
	len = strlen(name);
	if (len < 5 || strcasecmp(name + len - 4, ".pk3") || strchr(name, '/') || strchr(name, '\\') || strstr(name, ".."))
	{
		Con_Printf("fs_mountpack: \"%s\" is not a pk3 file name\n", name);
		return;
	}

	// the plain directory it is in, as FS_AddPack finds it
	search = FS_FindFile(name, &index, NULL, true);
	if (!search || search->pack)
	{
		Con_Printf("fs_mountpack: could not find \"%s\"\n", name);
		return;
	}
	dpsnprintf(fullpath, sizeof(fullpath), "%s%s", search->filename, name);

	last = NULL;
	for (search = fs_searchpaths; search; search = search->next)
	{
		if (search->pack && !strcasecmp(search->pack->filename, fullpath))
		{
			Con_Printf("fs_mountpack: %s is already mounted\n", name);
			return;
		}
		last = search;
	}

	pak = FS_LoadPackPK3(fullpath);
	if (!pak)
	{
		Con_Printf("fs_mountpack: %s is not a readable pk3\n", name);
		return;
	}
	dp_strlcpy(pak->shortname, name, sizeof(pak->shortname));
	search = (searchpath_t *)Mem_Alloc(fs_mempool, sizeof(searchpath_t));
	search->pack = pak;
	search->next = NULL;
	if (last)
		last->next = search;
	else
		fs_searchpaths = search;
	Cvar_SetValueQuick(&fs_mountpack_count, fs_mountpack_count.integer + 1);
	Con_DPrintf("fs_mountpack: %s (%i files)\n", name, pak->numfiles);
}

static void TouchFS_Init_Commands(void)
{
	Cvar_RegisterVariable(&fs_opentrace);
	Cvar_RegisterVariable(&fs_mountpack_count);
	Cmd_AddCommand(CF_SHARED, "fs_mountpack", TouchFS_MountPack_f, "add a pk3 from the data directory to the end of the search path without a restart, so it adds files but overrides none (Godnotic: packs downloaded while the game runs)");
}
