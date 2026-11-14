# Round timer (HUD half)

The client half of `round_timer`: `xt.channel("round_timer")` receives the server mod's messages and sends one hello back (`channel:ready()` says when the server is ready). Messages only come from servers you join, and `cl_lua_channels 0` or `lua_channel_block current` refuses them.

## Tier

**Standard** (on with the mod: no switch to turn on).

## Install

The game builds every example into an installable zip: `scripts/build-example-mods.sh`
writes `build/mods/round_hud.zip`. Then:

- **Linux**: in the game, Settings > Mods > *Install...* and pick the zip, or type
  `lua_mod_install /path/to/round_hud.zip` in the console, then `lua_install_confirm`.
- **Android**: Settings > Mods > *Install from file* and pick the zip (the system file
  picker), then *Install*.
- **Web**: Settings > Mods > *Install* > *Pick file* and pick the zip (the browser's file
  picker), or drop the zip on the page; then *Install*.
- From the folder while developing: copy `touch/lua/examples/mods/round_hud` to
  `<userdir>/mods/round_hud` (a folder works as well as a zip).

Turn it on in Settings > Mods (one tap), or `lua_mod_enable round_hud`.

## What you should see

- On a server running `round_timer`: *Round N  S s* under the timer at the top.
- On any other server: nothing (and nothing is sent).
