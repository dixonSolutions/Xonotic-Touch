# Player stats across maps

Persistent storage: `xt.data.save` / `xt.data.load` (a table as JSON in the mod's `data/` folder), saved on `map` and `unload`, with the `death` event and a chat command.

## Tier

**Standard** (on with the mod: no switch to turn on).

## Install

Server mods run on a server this game hosts (a local match, or a dedicated server).
`scripts/build-example-mods.sh` writes `build/plugins/player_stats.zip`. Then:

- **Linux / Android**: Settings > Mods > Server mods > *Install...* (or the console:
  `sv_lua_plugin_install /path/to/player_stats.zip`, then `lua_install_confirm`).
- **Dedicated server**: copy `touch/lua/examples/plugins/player_stats` to
  `<userdir>/plugins/player_stats` and run `sv_lua_plugin_enable player_stats`.
- **Web**: the web build hosts local matches too; install as on Linux from the console.

Turn it on with `sv_lua_plugin_enable player_stats` (or the switch in Settings > Mods).

## What you should see

- `!stats`: your kills, deaths and maps, kept across map changes and restarts.
- The file: `<userdir>/plugins/player_stats/data/stats.txt`.
