# Notes page

A page of its own in the Mods menu (`xt.menu.page`) with a text field, buttons, a list and a progress bar, updated with `page:set`; notes kept with `xt.data.save`; and an entry under Settings > Mods from the `setting.*` lines of `mod.txt`.

## Tier

**Standard** (on with the mod: no switch to turn on).

## Install

The game builds every example into an installable zip: `scripts/build-example-mods.sh`
writes `build/mods/mod_menu.zip`. Then:

- **Linux**: in the game, Settings > Mods > *Install...* and pick the zip, or type
  `lua_mod_install /path/to/mod_menu.zip` in the console, then `lua_install_confirm`.
- **Android**: Settings > Mods > *Install from file* and pick the zip (the system file
  picker), then *Install*.
- **Web**: the web build has no file picker for mods yet: open the console and run
  `lua_mod_install` with the zip's path in the browser's file system, or copy the folder
  into `mods/` with the browser's developer tools.
- From the folder while developing: copy `touch/lua/examples/mods/mod_menu` to
  `<userdir>/mods/mod_menu` (a folder works as well as a zip).

Turn it on in Settings > Mods (one tap), or `lua_mod_enable mod_menu`.

## What you should see

- Mods > Notes page: type a note, *Add*, and it appears in the list; the bar fills up.
- Notes survive a restart (`<userdir>/mods/mod_menu/data/notes.txt`).
- Settings > Mods > Notes page: the page title and how many notes to keep.
