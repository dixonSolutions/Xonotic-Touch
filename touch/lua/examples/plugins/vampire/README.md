# Vampire

A game rule: the `Damage_Calculate` hook changes the damage (an output of the hook), and an entity handle (`xt.ent`) heals the attacker by writing `health`. Both need the Game rules tier; without it the hook still runs, but changes are ignored and the write is refused.

## Tier

**Game rules** (a switch in Settings > Mods, or `sv_lua_plugin_permission vampire grant gamerules`): it changes the game in matches you host.

## Install

Server mods run on a server this game hosts (a local match, or a dedicated server).
`scripts/build-example-mods.sh` writes `build/plugins/vampire.zip`. Then:

- **Linux / Android**: Settings > Mods > Server mods > *Install...* (or the console:
  `sv_lua_plugin_install /path/to/vampire.zip`, then `lua_install_confirm`).
- **Dedicated server**: copy `touch/lua/examples/plugins/vampire` to
  `<userdir>/plugins/vampire` and run `sv_lua_plugin_enable vampire`.
- **Web**: the web build hosts local matches too; install as on Linux from the console.

Turn it on with `sv_lua_plugin_enable vampire` (or the switch in Settings > Mods).

## What you should see

- Hitting someone heals you by half the damage (the `share` setting), up to 200.
- Your own rockets do half damage to you.
- The console says how much was healed every 10 s.
