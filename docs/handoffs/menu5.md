# HANDOFF: work/menu5 (menu and in-match UI follow-ups)

Branch `work/menu5`, based on `origin/work/integration` 44e0595, with
`origin/work/integration` 664916d (main's Home tab bar) merged in (8159017; one
conflict in `touch_uib_render.qc`: both new functions kept). It includes the
input_test welcome fix e922614 (cherry-picked as a02ff28).

## Done and verified

| Commit | What |
|---|---|
| d76f946 | **Over play, chat only.** Lines a game puts in the chat box without a chat sound (control character 3: "connected", "is now playing", frags) become ordinary console lines that never show over play (`TouchUIB_ChatboxMask` in `touch_uib.c`, one `// Touch:` line in `Con_MaskPrint`). Player chat (control character 1: say, team, tell, votes) is untouched. `touch_chat_system_lines 1` restores stock behaviour. The HUD pickup panel ("Small health", "Bullets", under the move stick) is off on touchscreens (`Touch_ApplyNotifyPanel`). New test command `uib_chatfeed`. |
| b150402 | **First start through the default menu.** While `start.sh`/godnotic-fetchd still download the data (Ubuntu Touch, Linux tarball), the bridge menu is one `setup` screen (progress, message, Download in background, Close window, Try again, then relaunch) instead of Home with nothing to play; the classic download dialog never opened over the bridge and was cut off on a landscape phone. A tap on a text field now always asks for the screen keyboard on a touchscreen (it only asked if something had already asked), and the keyboard goes away with the field. |
| 7e8ade9 | The field being edited moves above the engine's own keys (touch-only devices, the web on a phone): the keys covered the welcome's Name field. |
| 99d4ea7 | **web_menu.mjs Escape flake, root cause.** The page keeps the last 200 console lines (`xt.log`); the test sliced at a length taken before each command, which stops moving once the tail is full, so every query after that point (the first one is the Escape check) read nothing. Markers now. The test also goes through the welcome with real input. `[skip ci]` |
| af19a62 | **GTK.** Settings rendered one blank row and an empty page at every size (vertical-tabs read as list-detail): now sections with subtitles and the chosen section's controls, one column on a phone. Loading showed the step, bar and Cancel twice plus the banner: one card (QuakeC `UIB_BuildLoading` too; seed fixture `seq-download` regenerated, the only fixture that changed). Map preview on a phone on its side puts the picture beside the text so Play is in view. Card subtitles at most two lines with an ellipsis. Script command `labels <file>` for tests. |

## Tests (all on this branch after the merge, menu5's own basedir)

- `scripts/test-uibridge.sh --engine`: all passed (engine_test, input_test, new firstrun_test, fixtures not drifted).
- `cargo test --offline -p godnotic-loader-core`: ok (11 + 11 fixture replays).
- `scripts/test-notify-lines.sh` (new): local map, stock switch, dedicated server, pack download over HTTP: all passed.
- `scripts/test-gtk-screens.sh` (new): Settings wide and narrow, one loading card, map preview landscape: all passed.
- `tests/uibridge/web_menu.mjs` in headless GPU Chrome: desktop 1280x720 passed 4 times in a row; touch 915x412 passed.
- Builds: `build/bin/xonotic`, `make sv-release`, `scripts/build-web.sh --engine-only`, gtk clippy `-D warnings`: all ok.
- `scripts/test-old-name.sh`: pass.

Test basedir: `~/.cache/godnotic-work/menu5-base` (stock pk3 links + this clone's pk3dir).
Web data: own files staged with `stage-web-data.sh --shell stock/`, `build/web/data/stock`
linked to `screens/build/web/data` (local only, no download).

## Not done / open

- Engine changes (`console.c`, `touch_uib.c/.h`) need `component-push.sh darkplaces` at merge (coordinator).
- On the web the engine keys' labels are blurry (FONT_CHAT drawn far above its loaded size at dpr 2.75; crisp on desktop). `touch_ui.c` is under an older lock; not touched.
- `loader/scripts/shot.sh` passes `GODNOTIC_DATA_DIRS`, which nothing reads any more: fixture screenshots show no map pictures.
- Singleplayer on a phone on its side (915x480) is still cramped: the mode strip, Play and Home's tab bar leave the map grid little room. The tab bar belongs to the Home redesign.
- web_menu `--touch` must run in landscape: portrait shows "Turn your device sideways".
- Nothing checked on the tablet or Android (no Android code changed).

## Screenshots

`~/.cache/godnotic-work/menu5-shots/`: `n1` (before) / `n2` (after) local match, `c1`/`c2`
connect, `fr1` welcome, `dl3` classic download dialog cut off, `su1`/`su2` setup screen,
`kb` field above the engine keys, `firstrun/`, `web-final/`, `web-final-touch/`, GTK `gtk1`
(before), `gtk2`, `gtk3` (after; `sheet-*.png` contact sheets).
