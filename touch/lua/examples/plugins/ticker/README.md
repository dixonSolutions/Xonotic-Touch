# Ticker

Continuous work: `xt.spawn` with a `while true do ... xt.sleep(1) end` loop, `xt.every` (stopping itself by returning false) and `xt.after`.

## Tier

**Standard** (on with the mod: no switch to turn on).

## Install

Server mods run on a server this game hosts (a local match, or a dedicated server).
`scripts/build-example-mods.sh` writes `build/plugins/ticker.zip`. Then:

- **Linux / Android**: Settings > Mods > Server mods > *Install...* (or the console:
  `sv_lua_plugin_install /path/to/ticker.zip`, then `lua_install_confirm`).
- **Dedicated server**: copy `touch/lua/examples/plugins/ticker` to
  `<userdir>/plugins/ticker` and run `sv_lua_plugin_enable ticker`.
- **Web**: the web build hosts local matches too; install as on Linux from the console.

Turn it on with `sv_lua_plugin_enable ticker` (or the switch in Settings > Mods).

## What you should see

- The console counts: *loop 5, loop 10, ...*, *every 1..3*, *after 4 s*.
