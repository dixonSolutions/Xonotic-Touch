# Next-weapon button

A touch button (`xt.touch.button`) with press, hold and release callbacks, and a key bind (`xt.bind`) whose key is a `key` setting in `mod.txt`.

## Tier

**Standard** (on with the mod: no switch to turn on).

## Install

The game builds every example into an installable zip: `scripts/build-example-mods.sh`
writes `build/mods/touch_weapon.zip`. Then:

- **Linux**: in the game, Settings > Mods > *Install...* and pick the zip, or type
  `lua_mod_install /path/to/touch_weapon.zip` in the console, then `lua_install_confirm`.
- **Android**: Settings > Mods > *Install from file* and pick the zip (the system file
  picker), then *Install*.
- **Web**: the web build has no file picker for mods yet: open the console and run
  `lua_mod_install` with the zip's path in the browser's file system, or copy the folder
  into `mods/` with the browser's developer tools.
- From the folder while developing: copy `touch/lua/examples/mods/touch_weapon` to
  `<userdir>/mods/touch_weapon` (a folder works as well as a zip).

Turn it on in Settings > Mods (one tap), or `lua_mod_enable touch_weapon`.

## What you should see

- A NEXT button on the right edge, below the scores, clear of the fire button. Tap: next weapon. Hold: best weapon.
- F7 (or the key you set in its settings page) switches to the next weapon.
- Without a touchscreen: `lua_mod_simulate finger 0 <x> <y> 1` presses it at x, y (2D units; `_lua_hud_panels` says where it is), `... 0` lifts it; `lua_mod_simulate key F7 1` presses the key.
