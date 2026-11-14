# Handoff: Android native-UI follow-ups (`work/android3`)

Written 2026-11-14 by the android3 agent. Scratch clone: `~/.cache/godnotic-work/android3`.
Based on `origin/work/integration`; `origin/main` f617cab (release candidate 1, corrected Home)
merged in at 7eb1556. **Ready to merge.**

## Commits

| Commit | What | CI |
|---|---|---|
| 10afb08 | Android: the join dialog over the running match; `_touch_loading_card`; Back on a described dialog goes to the engine | Android green (run 43545401237) |
| 39eb6da | Touch (QuakeC) game menu: Resume and Join game / Spectate on a phone's first screen, Leave and Quit pinned | - |
| c792ea8 | **Resume bug**: the player-statistics question becomes its own dialog, so Resume leaves the pause page | Android, Tests, Loader green (43549270340) |
| 3f2660a | merge origin/work/integration 664916d | Tests, Loader green |
| 3cf74dc | Android: d0_blind_id + GMP for every ABI, shipped in the APK with `key_0.d0pk` | Android green on all 3 ABIs (43550768168), Tests green |
| 7eb1556 | merge origin/main f617cab. HANDOFF.md conflict: this branch's kept; main's was hometabs2's | pushed as ci/android3 |

`engine/data/xonotic-data.pk3dir` changed (c792ea8, 39eb6da), so the data pin is needed at
merge. `engine/darkplaces` was not changed. `components.json` was not touched and
`component-push.sh` was not run.

## Done and verified

### 1. Resume on a live match (owner's report, top priority)

**Cause**: on a server that keeps statistics, while `cl_allow_uid2name` is -1, Xonotic's game
code (`HUD_Vote`) sends `menu_cmd directmenu Uid2Name` on every frame the menu is not on
screen. The native front end had no Uid2Name dialog, so it showed the bare pause page. Resume
closed the menu and the next frame opened it again. This happens only after the stock menu VM
has initialised, which it does in the frames the native UI is away (a return from the
background is enough). That is why DM probes that never detach did not catch it.

**Fix**: with a native renderer, `TouchGameMenu_Route` sends Uid2Name to the bridge's own
dialog: modal `uid2name` in `touch_uid2name.qc`, with Yes / No. It cannot be dismissed by a
tap outside, and Back answers No. The answer sets the cvar, runs the stock dialog's commands
(`vyes`/`vno`, `setreport`) and closes to the match. The QuakeC renderer (web) and the classic
menu keep the stock dialog. The Rust driver (`Driver::OVER_GAME`) and the Kotlin reader show
it over the match.

- Headless regression test `scripts/test-native-menu-live.sh`: all 14 checks pass on this
  branch. On the old androidui2 build it fails with the tablet's symptom: after Resume the
  states are `ingame`, `paused`.
- **Tablet** (perf package, builds 1.2.400/401/402/403-a3 from this branch; 403 is the main
  merge; LAN server on the desktop running stock game code):
  - the question shows as the native dialog over the running match;
  - No (first run) and Yes (second run) close it;
  - Privacy then shows "Show my name in records: On";
  - MENU then Resume returns to the game, and it stays there.

### 2. Join dialog over a running match
- The engine's `app.modal == ["join"]` in context `game` is drawn alone over the match. The
  pause page under it is hidden, the engine keeps drawing (`fe screen join 0`), and the scrim
  is light.
- Back or a tap outside a described dialog now goes to the engine. Before this it only hid
  the view and left the engine's menu up.
- Rust tests replay the snapshot the engine published in the controls probe.
- **Tablet**: the dialog came up by itself for the observer. Spectate closed it and the game
  continued; on another run Join put the player in the game.

### 3. `_touch_loading_card`
- Set to 1 at attach and back to 0 on a fall back.
- It is not toggled per card, because a local map load blocks the engine from its first
  frame, before a `set` sent with the card could run. The engine applies the cvar only during
  a join or load (UI_BRIDGE.md 4.13).
- **Tablet**: a join showed only the native card ("Joining 10.1.1.239:26150 · World model
  setup", Cancel) over black. No engine status lines were visible.

### 4. Phone game menu (QuakeC touch game menu, web and fallback)
- New order: Resume, Join game | Spectate, Change team | Settings, Mobile settings | Quick
  menu, the LAN row, then Servers | Profile and Leave | Quit, which are pinned at the bottom.
- Checked headless at 2340x1080 (first three rows plus footer, no scrolling) and 1920x1200
  (everything fits).
- `tests/touch-hudmode/gamemenu_order.py`, run from test-touch-hudmode.sh, checks the order.
- The native Kotlin pause screen fits at 2340x1080 by estimate (no device at that size).

### 5. Player IDs (d0_blind_id)
- GMP 6.3.0 is built static and PIC with portable C. The source is 2 MB, checked against its
  SHA-256. GMP's configure needs `m4` on the build host; the CI runner has it.
- `libd0_blind_id.so` and `libd0_rijndael.so` are built from `engine/d0_blind_id`. GMP's
  symbols are hidden and the pages are 16 KiB.
- CI built them for armeabi-v7a, arm64-v8a and x86_64: 0.2 to 0.3 MB each in the APK.
- **Tablet**:
  - "Loaded library libd0_blind_id.so" and "Loaded public key key_0.d0pk";
  - "Generated private ID", then "Received signature" from ca.xonotic.org;
  - `key_0.d0si` saved.

### Also seen on the tablet
- Settings has no "Advanced settings" bar.
- 1.2.403-a3 (the main merge): question, then join dialog, Spectate, MENU, Resume all work.
  Leave match asks to confirm and then shows the new Home (three cards, Settings / Credits /
  Quit bar).
- The top bar (console, MENU, SCORE, CHAT) has no overlaps while playing or spectating.

## Tests run (this branch, 3cf74dc working tree)

```
cargo test --offline -p godnotic-loader-android --no-default-features   # 8 + 1 passed
cargo test --offline -p godnotic-loader-core                            # 11 + 11 passed
scripts/test-uibridge.sh                                                # all passed
scripts/test-touch-hudmode.sh                                           # all passed (+ game menu order)
scripts/test-old-name.sh                                                # PASS
ENGINE=build/bin/xonotic BASEDIR=<clone basedir> STOCKBASE=~/.cache/godnotic-work/controls-stock \
LIBS=~/.cache/godnotic-slimtest/lib scripts/test-native-menu-live.sh    # all checks passed
SKIP_BUILD=1 ~/.cache/godnotic-work/run-suites.sh <clone> <logs>
```

run-suites.sh, every step passed:
- old-name, cargo-core, cargo-android, uibridge, touch-spectate, touch-hudmode, lan-logic,
  host-resources, frontend, lan, lan-private;
- lua-sandbox, lua-match, join-world, srvlist, joinfail, uib-engine, mods (105/105),
  touch-controls, net-away.

Two notes on that run:
- `net` failed the first time only because SKIP_BUILD left no `darkplaces-dedicated`. After
  `make sv-release` it passes (gn-0, gn-100).
- touch-controls-live skipped (77): the runner sets no STOCKBASE.

After the main merge, these were run again and pass: test-native-menu-live, test-uibridge,
touch-hudmode, old-name, cargo core and android. Not rebuilt for this check: `make
sv-release` (rebuilt before the merge) and `build-web.sh --engine-only`. The merge brought no
engine C changes.

Kotlin compiles only in CI. The Gradle unit test added to `UiBridgeTest.kt`
(`joinDialogOverTheMatch`) is not run by CI, which runs only `assembleRelease`.

## Not done / known

- Not checked on a 2340x1080 device: the native pause screen with a LAN hosting card. That
  card comes first and may push the actions below the fold.
- The stock HUD's "NEXT ▸" label at the right overlaps the weapon strip while playing on a
  stock server. Seen on the tablet; not investigated, because it belongs to the HUD and
  controls agents.
- The Linux GTK front end gets the same `uid2name` dialog: the route keys on any native
  renderer. Its "over the match" presentation is untested.
- `scripts/test-native-menu-live.sh` is not wired into a workflow; run-suites.sh does not
  run it either.
- The tablet still has the perf package 1.2.403-a3 installed (it ended on the new Home), with a generated player ID
  (`key_0.d0si`) and `cl_allow_uid2name 1`. `xonotic.args` was removed.

## Tools

- `~/.cache/godnotic-work/android3-tools/`:
  - `resume/` (headless probes);
  - `tabsrv.sh` (stock LAN server for the tablet, port 26150);
  - `m4/` (m4 1.4.19, built locally because the host has none).
- APK build: `~/.cache/godnotic-work/androidui-tools/build2.sh` with `SRC=<export of HEAD>`.
  With d0 it needs `m4` on PATH.
