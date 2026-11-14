# HANDOFF: work/polish (four small polish bugs)

Branch `work/polish`, based on `origin/main` f617cab (main had not moved when this was
finished). Clone `~/.cache/godnotic-work/polish`. Screenshots under
`~/.cache/godnotic-work/polish-shots/{before,after}/`.

## Done and verified

| Commit | What |
|---|---|
| 8ab6161 | **Sharp key labels on a phone** (`engine/darkplaces/touch_ui.c`). The engine's own keys drew their labels in the chat font, which `font-xolonium.cfg` loads at one size (`con_chatsize` 10). The web on a phone renders at 1.25 pixels per CSS pixel (`web/shell.js`, deliberate), so that is a 9 px glyph map stretched to about 19 px: blurred, misshapen letters ("CLCEE", "FELP"). Desktop stretched it too (12 px map for 17 to 36 px labels), less visibly. Labels now use `user1`, the game's text font (4 to 32 virtual px, each size rendered when first drawn), so the nearest map is within a few pixels; the chat font only if `user1` is missing. New command `touch_ui_labelfont [h]` and test `scripts/test-touch-labels.sh`. |
| 2b0612e | **Fixture shots get real map previews** (`loader/scripts/shot.sh`, `drive.sh`, new `data-dirs.sh`, `crates/gtk/src/backend.rs`). Fixture mode does still read `GODNOTIC_DATA_DIRS`, but the scripts always passed it, empty when unset, and an empty value meant "no folders at all"; the stock pk3s here (Xonotic Flatpak in `/var/lib/flatpak`) were also outside the sandbox. Now: `GODNOTIC_DATA_DIRS` when set, otherwise the app's own data folders (`paths.rs`) and an installed Xonotic Flatpak's data that exist, each exposed read-only to the sandbox (works from distrobox too). Empty `GODNOTIC_DATA_DIRS` counts as unset. |
| 5cb7f0f | **GTK Home fits 400 px** and **the map picker fits a phone on its side** (`crates/gtk/src/widgets.rs`, `data/style.css`). Home: cards stay in one row and shrink. Their minimum widths summed to 620 px (a title word, a subtitle word, and Adwaita's 150 px progress bar minimum in Campaign). Now the picture shrinks, subtitle (2 lines) and title end in an ellipsis, the progress bar has no minimum, and narrow windows get a 15 px title, 8 px padding and 8 px spacing. Content needs 342 px at 400x860; titles are whole at 400, ellipsized only at 360. Singleplayer under 540 px high: the mode strip is chips (name only, help as tooltip, like the QuakeC renderer's short page) and the sideways map list shows fewer, taller rows when two would be under the smallest legible tile. New UI script command `measure` (window size and the width its content needs, map pictures arrived, node boxes) in `script.rs` (+ `Images::arrived_count`), test `scripts/test-gtk-layout.sh`. Notes in `docs/UI_BRIDGE.md` (`rows`) and `docs/LAUNCHER.md`. |

Kept off what work/menu5 changed: `split()`, the card subtitle lines in `card()` (the new
featured-card block sits after `v.append(&t)`, a separate hunk; it calls `set_lines(2)` and
`set_ellipsize(End)` on featured subtitles, the same calls menu5 makes for every card) and
the map preview. `script.rs`: `measure` goes before `quit`, menu5's `labels` after `snapshot`.

### Bug 4: where the cramping came from

Not the shared description. At 915x480:
- **GTK** was the cramped one: mode cards 239 px tall (88 px emblem plus a three-line help
  subtitle) and a tile height formula that assumed that strip, so the map list started at
  y=374 and the action bar at 410 covered all but a 36 px sliver. Fixed (see above): list
  172 px tall, y 234 to 406.
- **QuakeC (web)** already compacts the strip below 540 dp (art and name, 96 dp) and shows
  one whole row of 136 dp map tiles above Play, the second row by scrolling
  (`before/web-sp-915x480-menu.png`). Left as it is.
- **Kotlin**: mode cards carry no artwork (`isPhoto` is maps only), about 88 dp, and the map
  list takes the page's visible height. Not changed; not checked on a device.

## Tests (this branch, own basedir `~/.cache/godnotic-work/polish/.suite-base`)

- `scripts/test-touch-labels.sh` (new): all passed. Phone-web 1144x515, conheight 600: labels of 12.0 / 18.9 / 25.8 px draw from 12 / 17 / 27 px maps (`user1`). Desktop 1280x720: 16.8 / 26.4 / 36.0 from 17 / 24 / 38. With the old font forced back it fails: every label from a 9 px (phone) or 12 px (desktop) `chat` map.
- `scripts/test-gtk-layout.sh` (new): all passed. Home 400x860 needs 342 px, cards end at 381; 18 map pictures arrived; Singleplayer 915x480 picker 172 px, ends at 406 above Play at 420. On main's GTK code it fails: Home needs 620 px, 0 map pictures, picker cut (y 374, 246 tall, Play at 420).
- `scripts/test-uibridge.sh`: all passed. `--engine` (UIB_BASEDIR = own basedir): all passed (engine_test, input_test, fixtures not drifted). **No fixture regenerated.**
- `cargo test --offline -p godnotic-loader-core`: ok (11 + 11 + 0).
- `loader/build.sh --check` (SDK 50; 48 not installed here): ok, clippy `-D warnings` clean, core tests 11 + 14 + 11.
- `scripts/test-gtk-screens.sh`: not on main yet (it is on work/menu5), not run.
- `scripts/test-old-name.sh`: PASS.
- Builds: SDL `build/bin/xonotic`, `make sv-release`, `scripts/build-web.sh --engine-only` (XT_WEB_DEPS from the integration clone): all ok.
- Web, headless GPU Chrome (`~/.cache/godnotic-work/polish-tools/run.sh` + `keys.mjs`): console sheet at 915x412 DPR 2.75 touch and at 1280x720 DPR 1, before and after (screenshots below).
- `tests/uibridge/web_menu.mjs` on this build and, for comparison, on main's engine: the same checks fail on both. main's script: the 3 tap/click checks (the welcome dialog of a fresh profile covers Home; menu5 fixes the script). menu5's script: "screen keys with the field above" (a menu5 feature) and the 2 classic-dialog checks. Nothing differs between main and this branch.

## Not done / open

- Engine change (`touch_ui.c`) needs `scripts/component-push.sh darkplaces` at merge (coordinator).
- This file replaces work/hometabs2's HANDOFF.md that main carried; other branches' handoffs will conflict with it at merge.
- GTK Home at 915x412 gets a 480 px tall window (its content's minimum height); it did before too, not part of this batch.
- Mode emblems in GTK fixture shots are drawn with their black TGA background (visible now that pictures load); not looked into.
- Nothing checked on the tablet or on Android (no Android code changed).

## Screenshots

`~/.cache/godnotic-work/polish-shots/`:
- Key labels, phone web: `before/web-phone-console.png`, `after/web-phone-console.png` (crops `*/web-phone-keys-crop.png`); desktop web: `*/web-desktop-console.png` (crops `*/web-desktop-keyboard-crop.png`).
- Map previews: `before/gtk-home-1280x800.png` (main's shot.sh, empty cards), `after/gtk-home-1280x800.png`, `after/gtk-singleplayer-1280x800.png`, `after/gtk-map-preview-1280x800.png`.
- Home 400 px: `before/gtk-home-400x860.png` (cut off at the right), `after/gtk-home-400x860.png`, also `after/gtk-home-360x780.png`, `after/gtk-home-915x412.png`.
- Singleplayer 915x480: `before/gtk-sp-915x480-pictures.png` (with pictures) and `before/gtk-sp-915x480.png`, `after/gtk-sp-915x480.png`; web, unchanged: `before/web-sp-915x480-menu.png`.
