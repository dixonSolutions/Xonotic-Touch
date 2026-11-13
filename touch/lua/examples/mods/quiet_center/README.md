# Quiet centre messages

Filtering what the server shows: `centerprint` (any server) and `notification` of kind `center` (this game's client code), hidden by returning `false`; a `text` setting says what to hide.

## Tier

**Standard** (on with the mod: no switch to turn on).

## Install

The game builds every example into an installable zip: `scripts/build-example-mods.sh`
writes `build/mods/quiet_center.zip`. Then:

- **Linux**: in the game, Settings > Mods > *Install...* and pick the zip, or type
  `lua_mod_install /path/to/quiet_center.zip` in the console, then `lua_install_confirm`.
- **Android**: Settings > Mods > *Install from file* and pick the zip (the system file
  picker), then *Install*.
- **Web**: the web build has no file picker for mods yet: open the console and run
  `lua_mod_install` with the zip's path in the browser's file system, or copy the folder
  into `mods/` with the browser's developer tools.
- From the folder while developing: copy `touch/lua/examples/mods/quiet_center` to
  `<userdir>/mods/quiet_center` (a folder works as well as a zip).

Turn it on in Settings > Mods (one tap), or `lua_mod_enable quiet_center`.

## What you should see

- Centre messages containing *Prepare* (the countdown) no longer show; change the words in its settings page.
- `lua_quiet_center_count` prints how many were hidden.
