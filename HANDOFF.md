# Handoff: work/hometabs2 (Home: banner, three cards, bottom bar)

Base: `origin/work/integration` 664916d. Do not run component-push from here; the
coordinator merges and releases.

## Done and checked
- **Home** is the banner at the top (scaled to the window: it takes the height the cards
  leave, up to 45% of the page), the three cards Singleplayer / Campaign / Multiplayer side
  by side in one row at every size (ids `home/tiles/*` unchanged), and a bar along the
  bottom: `home/nav`, type `tabbar`, three joined `button`s Settings, Credits, Quit. Quit
  keeps its confirm. The header has no actions any more; the console button stays.
- The same bar is on **Settings** and **Credits** opened from Home (not over a match), with
  `current` set: that segment is darker with an ember line and does nothing when tapped. A
  tap on the other swaps the screen in place (`UIB_OpenTab`), Back goes Home. A pressed or
  focused segment is darker too.
- The mode screens (Singleplayer, Campaign, Multiplayer) no longer carry a bar (cac2da0's
  mode tabs are gone).
- **QuakeC renderer**: bar = region 7 (56 dp, 48 dp on a short page), cards always one row,
  cards centred in the space under the banner.
- **GTK**: bar as the last bottom bar; banner and card pictures expand from small minimums,
  so 800x400 fits without scrolling. **Kotlin**: bar segments as flat buttons with a
  pressed/focused/current dark state; c359a71's one-row FlowGrid is back (the integration
  merge had dropped it); banner height rule from before cac2da0.
- Events from Home's actions must name `home/nav/<id>` (the engine refuses an action the
  node does not offer): uib-fixtures.py, engine_test.py, web_menu.mjs and the GTK scripts
  are updated. input_test now taps the bar: Settings, Credits (in place), Settings back.

## Tests (all in this clone, basedir ~/.cache/godnotic-work/hometabs-base with this clone's pk3dir)
- `scripts/test-uibridge.sh`: all passed
- `UIB_BASEDIR=… UIB_LIBDIR=~/.cache/godnotic-slimtest/lib scripts/test-uibridge.sh --engine`:
  all passed (engine_test, input_test 1280x720 mouse, fixtures have not drifted)
- `tests/uibridge/input_test.py 800x360 touch` and `2340x1080 touch`: all passed (27 checks)
- `cargo test --offline -p godnotic-loader-core` and `-p godnotic-loader-android`: passed
- `scripts/uib-fixtures.py --write` + `tests/uibridge/seed_fixtures.py`: regenerated
- GTK `loader/scripts/drive.sh tests/navigate.script` and `builtin.script`: ran
- CI on `ci/hometabs2`: see the report (Tests, Loader, Android)

## Not done / known
- Nothing checked on the tablet or a real phone; Android only compiled in CI.
- web_menu.mjs (the web build test) not run here.
- GTK at 400 px wide portrait: the window grows wider than 400 (three cards' minimum widths);
  portrait phones on Linux are not a target.
- `navigate.script` still clicks `settings/sections/mods`, which no longer exists (Settings
  is a split now): already broken before this branch.
