# Chat commands and a vote

`xt.chatcommand`: commands players type as `!name` in chat (the line is not shown to others) or `cmd name` in the console; `xt.match.vote` starts a vote on a stock vote command as that player (Game rules).

## Tier

**Game rules** (a switch in Settings > Mods, or `sv_lua_plugin_permission chat_vote grant gamerules`): it changes the game in matches you host.

## Install

Server mods run on a server this game hosts (a local match, or a dedicated server).
`scripts/build-example-mods.sh` writes `build/plugins/chat_vote.zip`. Then:

- **Linux / Android**: Settings > Mods > Server mods > *Install...* (or the console:
  `sv_lua_plugin_install /path/to/chat_vote.zip`, then `lua_install_confirm`).
- **Dedicated server**: copy `touch/lua/examples/plugins/chat_vote` to
  `<userdir>/plugins/chat_vote` and run `sv_lua_plugin_enable chat_vote`.
- **Web**: the web build hosts local matches too; install as on Linux from the console.

Turn it on with `sv_lua_plugin_enable chat_vote` (or the switch in Settings > Mods).

## What you should see

- `!time` answers with the server's clock, the map and the game type.
- `!shuffle` starts a vote to shuffle the teams (in team games).
