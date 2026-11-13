# Bot control

`xt.bots.add`, `xt.bots.remove`, `xt.bots.skill`, with chat commands, and `xt.ent(id).bot` to tell bots from people.

## Tier

**Game rules** (a switch in Settings > Mods, or `sv_lua_plugin_permission bot_control grant gamerules`): it changes the game in matches you host.

## Install

Server mods run on a server this game hosts (a local match, or a dedicated server).
`scripts/build-example-mods.sh` writes `build/plugins/bot_control.zip`. Then:

- **Linux / Android**: Settings > Mods > Server mods > *Install...* (or the console:
  `sv_lua_plugin_install /path/to/bot_control.zip`, then `lua_install_confirm`).
- **Dedicated server**: copy `touch/lua/examples/plugins/bot_control` to
  `<userdir>/plugins/bot_control` and run `sv_lua_plugin_enable bot_control`.
- **Web**: the web build hosts local matches too; install as on Linux from the console.

Turn it on with `sv_lua_plugin_enable bot_control` (or the switch in Settings > Mods).

## What you should see

- `!bots 4` makes it four bots; `!skill 7` sets their skill.
