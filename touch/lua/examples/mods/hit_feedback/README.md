# Hit feedback and kill feed

Client events (`damage`, `hit`, `kill`, `notification`) and drawing: floating damage numbers, a hit sound, and the kill feed restyled (the stock death messages are hidden by returning `false` from `notification`).

## Tier

**Standard** (on with the mod: no switch to turn on).

## Install

The game builds every example into an installable zip: `scripts/build-example-mods.sh`
writes `build/mods/hit_feedback.zip`. Then:

- **Linux**: in the game, Settings > Mods > *Install...* and pick the zip, or type
  `lua_mod_install /path/to/hit_feedback.zip` in the console, then `lua_install_confirm`.
- **Android**: Settings > Mods > *Install from file* and pick the zip (the system file
  picker), then *Install*.
- **Web**: Settings > Mods > *Install* > *Pick file* and pick the zip (the browser's file
  picker), or drop the zip on the page; then *Install*.
- From the folder while developing: copy `touch/lua/examples/mods/hit_feedback` to
  `<userdir>/mods/hit_feedback` (a folder works as well as a zip).

Turn it on in Settings > Mods (one tap), or `lua_mod_enable hit_feedback`.

## What you should see

- Red numbers near the crosshair when you are hurt, green ones and a click when you hit someone.
- The kill feed as a compact list at the right edge instead of the stock lines.
- On a server that sends its own client code, `hit` and the restyled feed do not happen (they need this game's client code: `xt.csqc()` says `foreign`); damage numbers still work.
