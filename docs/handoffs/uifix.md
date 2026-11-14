# Handoff: `work/uifix` (tablet UI batch, release round 2)

Base: `origin/main` 7a8d3ee with `origin/work/tabletfix` (9fe6246 + its HANDOFF.md, 540e275)
merged in first, so the map picker's source fix and the Duel one-bot cap are underneath.
Clone `~/.cache/godnotic-work/uifix`. Only the data component changed
(`engine/data/xonotic-data.pk3dir`, menu QuakeC); `engine/darkplaces` is untouched. The merge
needs `scripts/component-push.sh data` and the pin. `component-push.sh` was not run here.

## What changed, per item

1. **Settings > Updates said "Version Godnotic".** Dropped from this branch by the
   coordinator: the Updates agent (`work/updates2`) owns the whole Updates screen, version
   string included. Nothing here touches `touch_update_util.*` or the Updates section.
2. **The pause page called a local match "Match · Online game", and its row did nothing.**
   Cause: the stock menu works `gamestatus` out in `m_draw`, and `m_draw` never runs while a
   native front end hides the menu. So `GAME_ISSERVER` was never set, or it was left over from
   whenever the menu last drew. The bridge now refreshes it itself (`UIB_GameStatus()` in
   `UIB_Context` and `UIB_Event`). The match card now reads:
   - a local game: `<map>`, `<mode> · your local game`, the map picture, no action;
   - a LAN game you host: `<mode> · your LAN game at <first address>`. Tapping it opens the LAN
     popup (`host.popup`: the addresses, the lock, who is in);
   - someone else's server: the server's name from the list, else the address you joined.
     The subtitle is `Online game` or `LAN game` (LAN/VPN address ranges, as the server list's
     `net` cell) plus the address. It has no action. `UIB_Join` now records the name and net
     (`_uib_server_name` was registered but never set), and starting a local game clears them.
   - Kotlin drew a chevron on every card without a picture, even when the card had no
     action. Now the chevron appears, and the card is clickable, only when it has a
     `primary`. GTK draws no chevron and the QuakeC renderer draws none either.
3. **Map tiles showed three mode chips, so Afterslime in Duel's list had no DUEL chip.**
   `UIB_MapRow` puts the picker's own mode first in the row's `modes` cell. Every renderer
   shows the first few of that cell (Kotlin's chips, the GTK subtitle), so all three agree
   with no renderer change.
4. **"15 bots" for Duel on the Bots stepper and the Home card.** Duel has its own archived
   setting, `touch_lan_bots_duel` (0 or 1, default 1), registered by the menu. In Duel the
   Bots stepper binds it with a 0–1 range. Every other mode keeps `touch_lan_bots` (0–15),
   and a Duel choice never changes it. Home's card and Play read the same helper
   (`UIB_ModeBots`), so the card says "1 bot" (singular now). tabletfix's `min(bots, 1)`
   stays as a backstop. The GTK spin row now also follows a range change in a diff (it used
   to update only the value). Kotlin and the QuakeC renderer rebuild the stepper from its
   props already.
5. **No join dialog for a player who joins a match already under way.** Godnotic's own game
   code, which LAN games run and newer servers run, asks for the game's stock **Welcome**
   dialog on connect, only within 5 s of joining. That is why a map change, which is "match
   start", still got the join dialog. A native front end cannot draw that stock dialog. When
   the menu's `gamestatus` read "connected" (any time the menu had drawn during an earlier
   match: Android does this on every return from the background), `TouchGameMenu_Route`
   sent `touch_join_answered`, so the join dialog was no longer owed. The invisible Welcome
   dialog then held the menu open: a bare pause page and no question. Now, with a native
   renderer (`UIB_WelcomeNative()`), the Welcome request opens nothing and answers nothing,
   and the engine's join dialog opens as on any other server. Spectate is still respected
   (`TouchJoinDlg_Answered`), and `touch_join_dialog_auto 0` still never opens it. Those
   rules (`TouchJoinDlg_Frame`) are unchanged. On a stock 0.8.6 server (no Welcome) the
   mid-match join dialog already worked here, with and without the statistics question
   first. Reproduced headless with a native front end attached.
   Files: `dialog_touch_gamemenu.qc`, `touch_uib_join.qc`, `touch_uib_touch.qh`; doc in
   `docs/CONTROLS.md` (Joining).

## Tests (added or extended)

- `tests/uibridge/engine_test.py`, a new `modes_and_match()` run with a native renderer over
  the socket and no network. It checks: the Home card in Duel says "1 bot" with
  `touch_lan_bots 15`; Duel's stepper is 0–1 showing 1 on `touch_lan_bots_duel`; every
  `maps.duel` tile leads with `duel`; Duel's 0 leaves Deathmatch's 15 and keeps its own 0;
  a local game's pause card says "your local game" and has no primary. Against the old
  menu.dat 4 of these 6 failed ("15 bots", 0–15, `ca dm dom duel…`, DM pref overwritten).
- `scripts/test-native-menu-live.sh joinmid` plus `tests/native-menu/check_joinmid.py`
  (it is now in the default case list). A Godnotic-code server with the match under way.
  The client first plays it with the QuakeC menu, attaches the native front end, leaves and
  joins again through Multiplayer's address field. It checks: the join popup opens by
  itself, Spectate closes it and it stays closed, and the pause card names the address,
  "LAN game", with no action. With the Welcome routing disabled (and fresh `gamestatus`) the
  popup never came and the pause page was bare. With the old menu.dat the stale
  `gamestatus` even kept the menu in the "game" context after leaving, so Multiplayer could
  not be opened.
- `tests/touch-hudmode/hudmode_test.c`: a mid-match join (following a player, a question
  holding the screen) gets the dialog once the screen is free, and is not asked again after
  Spectate.
- `tests/mapmode/mapmode_test.py`: Duel also sets `touch_lan_bots_duel 3`; Play must start 1.
- Fixtures regenerated with `scripts/uib-fixtures.py --write`: `create`, `map-preview`,
  `seq-map-select`, `singleplayer`, `singleplayer-create-lan`. Only the order of the
  `modes` cells changed (the picker's mode first).

## Results

On the final tree, with item 1 reverted: the engine is identical to the base, and the menu.dat
and SDL engine were rebuilt from this clone. The basedir is `.suite-base`: the stock pk3s plus
this clone's pk3dir. Everything was pinned to `taskset -c 8-11`.

| Command | Result |
|---|---|
| `scripts/test-old-name.sh` | PASS: no old project name in tracked files |
| `scripts/test-uibridge.sh` | rc 0, all passed |
| `UIB_BASEDIR=.suite-base scripts/test-uibridge.sh --engine` | rc 0, all passed (includes the new `modes_and_match` checks and the fixture drift check) |
| `scripts/test-touch-hudmode.sh` | rc 0, all checks passed |
| `scripts/test-touch-spectate.sh` | rc 0, all checks passed |
| `STOCKBASE=~/.cache/godnotic-work/controls-stock scripts/test-native-menu-live.sh` (uid2name and joinmid) | rc 0, all checks passed |
| `scripts/test-mapmode.sh` (all modes, native, native-restart and qcmenu) | rc 0, all passed |
| `cargo test --offline -p godnotic-loader-core` | 23 passed, 0 failed |
| `cargo test --offline -p godnotic-loader-android --no-default-features` | 9 passed, 0 failed |
| `loader/build-in-sdk.sh "cargo clippy --locked -p godnotic-loader -- -D warnings"` (GNOME SDK 50) | clean |
| `scripts/uib-fixtures.py --write`, then `--check` | 5 fixtures rewritten (see above); no drift after |

The engine is unchanged, so `make sv-release` and `build-web.sh --engine-only` were not needed.
The SDL engine was still rebuilt to run the suites.

CI: runs for a83f4ff on `work/uifix` and `ci/uifix` (Tests, Android, Loader, Web) were still in progress at handoff; check `gh run list --branch ci/uifix`.

## Not done / known

- Nothing was checked on the tablet. Kotlin was not compiled locally; the `ci/uifix` Android
  job compiles it.
- Someone else's server joined other than through Multiplayer (a typed `connect`, an old
  join link path) has no name or address on the pause card: it says "Match · Online game".
- The rule for re-offering the dialog after the server itself moves a player to the
  spectators mid-match is unchanged: being in the game once still settles it for that match.
