# Round timer (server half)

A server mod that runs a game mode and ships its HUD to the players: a coroutine counts rounds down, `xt.channel` sends the state to every player's `round_hud` client mod and answers each one's hello, and `!round` in chat shows a page from the server (`xt.page.show`) whose button comes back to the mod. Install `round_hud` (client) too.

## Tier

**Standard** (on with the mod: no switch to turn on).

## Install

Server mods run on a server this game hosts (a local match, or a dedicated server).
`scripts/build-example-mods.sh` writes `build/plugins/round_timer.zip`. Then:

- **Linux / Android**: Settings > Mods > Server mods > *Install...* (or the console:
  `sv_lua_plugin_install /path/to/round_timer.zip`, then `lua_install_confirm`).
- **Dedicated server**: copy `touch/lua/examples/plugins/round_timer` to
  `<userdir>/plugins/round_timer` and run `sv_lua_plugin_enable round_timer`.
- **Web**: the web build hosts local matches too; install as on Linux from the console.

Turn it on with `sv_lua_plugin_enable round_timer` (or the switch in Settings > Mods).

## What you should see

- Every 60 s (the `length` setting) chat says *Round N!*.
- Players with `round_hud` see the round and the seconds left at the top of their screen.
- `!round` in chat opens a page marked as from the server; its button ends the round.
