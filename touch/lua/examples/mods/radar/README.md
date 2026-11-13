# Practice radar

`xt.entities` with `xt.view` and `xt.offline`: a radar panel. Offline against bots it shows every player; online only the ones you can see (on screen, not behind a wall), which the engine enforces. Needs this game's client code for players to be listed.

## Tier

**Standard** (on with the mod: no switch to turn on).

## Install

The game builds every example into an installable zip: `scripts/build-example-mods.sh`
writes `build/mods/radar.zip`. Then:

- **Linux**: in the game, Settings > Mods > *Install...* and pick the zip, or type
  `lua_mod_install /path/to/radar.zip` in the console, then `lua_install_confirm`.
- **Android**: Settings > Mods > *Install from file* and pick the zip (the system file
  picker), then *Install*.
- **Web**: the web build has no file picker for mods yet: open the console and run
  `lua_mod_install` with the zip's path in the browser's file system, or copy the folder
  into `mods/` with the browser's developer tools.
- From the folder while developing: copy `touch/lua/examples/mods/radar` to
  `<userdir>/mods/radar` (a folder works as well as a zip).

Turn it on in Settings > Mods (one tap), or `lua_mod_enable radar`.

## What you should see

- In a local match against bots: a round radar at the right edge with a dot for every bot, bright when you can see it, dim when it is behind a wall.
- On a server with other people: only the players you can see anyway.
