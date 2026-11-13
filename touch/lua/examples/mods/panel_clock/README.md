# Clock panel

A HUD panel (`xt.hud.panel`) showing the time, placed by the engine clear of the touch controls and movable by the player, with settings declared in `mod.txt` (`setting.<name>` with `desc` and `group`) and a settings page of its own (`xt.settings.page`) with a switch and a slider bound to those settings and a button.

## Tier

**Standard** (on with the mod: no switch to turn on).

## Install

The game builds every example into an installable zip: `scripts/build-example-mods.sh`
writes `build/mods/panel_clock.zip`. Then:

- **Linux**: in the game, Settings > Mods > *Install...* and pick the zip, or type
  `lua_mod_install /path/to/panel_clock.zip` in the console, then `lua_install_confirm`.
- **Android**: Settings > Mods > *Install from file* and pick the zip (the system file
  picker), then *Install*.
- **Web**: the web build has no file picker for mods yet: open the console and run
  `lua_mod_install` with the zip's path in the browser's file system, or copy the folder
  into `mods/` with the browser's developer tools.
- From the folder while developing: copy `touch/lua/examples/mods/panel_clock` to
  `<userdir>/mods/panel_clock` (a folder works as well as a zip).

Turn it on in Settings > Mods (one tap), or `lua_mod_enable panel_clock`.

## What you should see

- In a match, a small clock at the left edge below the chat.
- Settings > Mods > Clock panel: the generated page (Show seconds, Text size, Colour) and the mod's own page with *Reset to defaults*.
- Turning *Show seconds* off changes the clock at once; the value is kept in your config (`lua_panel_clock_seconds`).
- Console: `lua_pages` lists both pages; `lua_page_json panel_clock.main` prints the description renderers draw.
