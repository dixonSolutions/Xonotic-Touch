# Handoff: work/mapmode ("No map you have supports this game mode" was false)

Base: `origin/main` f617cab. QuakeC and tests only, no engine change. Do not run
component-push from here; the coordinator merges and releases (data component).

## Cause
Under the native front end (Android Kotlin, GTK launcher) the menu VM never builds
MapInfo. `MapInfo_Enumerate()` (the `maps/*.bsp` glob) runs only in `preMenuInit()`,
which is called from `m_draw`, and `MP_Draw` returns early while
`TouchFE_MenuHidden()` is true. A front end that attaches before the menu's first frame,
or a `menu_restart` while it is attached, leaves `_MapInfo_globcount` at 0. Then
`MapInfo_FilterGametype` gives 0 maps for every mode and `UIB_StartGame` refuses.
Evidence (desktop, f617cab, stock data, Create > CTF > Play over `fe ui`):
`_MapInfo_globcount: 0`, `_MapInfo_globopen: 0`, `menuInitialized: 0`, and the toast
"No map you have supports this game mode". Attached 3 s later (after the menu drew
frames): `_MapInfo_globcount: 30`, and the match starts. The data is not the cause: the
released slim set has 30 .bsp and 30 .mapinfo files, and every offered mode has 9 to 20 maps.

Two smaller faults, fixed too:
- The picker (engine source `maps:<mode>`) read only explicit `gametype` lines. MapInfo
  adds Duel on every DM map (`g_duel_not_dm_maps 0`), so Duel listed 11 of 18 maps on
  stock data.
- When the chosen map was not in MapInfo's list for the mode, `UIB_StartGame` silently
  played a random map of that mode.

## Done and verified
`qcsrc/menu/xonotic/touch_uib_screens.qc` (Touch-owned):
- `UIB_MapInfoEnumerate` / `UIB_FilterMaps`: the bridge builds MapInfo itself (cache and
  enumerate) the first time it needs it, independent of `m_draw`.
- `UIB_StartGame` re-enumerates once when the mode has no maps or the chosen map is not
  among them (a map downloaded since). After that it refuses, with a toast that says
  why: "<map> cannot be played in <mode>" or "<map> is not installed". It never starts
  a different map. Only Quick play (map "") picks a random map.
- The picker is source `maps.<mode>` (static rows set by the menu VM from MapInfo's
  filter: name, title, author, description, modes). It lists exactly the maps Play can
  start. The engine's `maps` / `maps:<mode>` sources stay for the map details.
- A mode card with no startable map is not offered (`UIB_ModeMaps`).
- Choosing a mode whose list lacks the chosen map moves the choice to that mode's first
  map (`UIB_SetMode`).
- docs/UI_BRIDGE.md: the `maps.<mode>` source. Fixtures regenerated
  (`scripts/uib-fixtures.py --write`, stock 0.8.6 data, the usual fixture base): only the
  source name and the rows' `modes`/`description` changed. seed_fixtures.py follows.
- New `scripts/test-mapmode.sh` (`tests/mapmode/mapmode_test.py`): one engine per path
  and mode. The paths are native (front end attached before the first frame),
  native-restart (plus `menu_restart`) and qcmenu; the modes are dm tdm ctf ca duel ft dom
  ka kh lms. Per case it checks that the card is offered and that the picker rows equal
  MapInfo, computed independently from the packs (gametype lines, implied Duel, .bsp
  present, no hidden/forbidden/frustrating). Play on a map the mode cannot start must
  give a toast and start nothing. Play on a picker map must have the server report that
  map and gametype (`prvm_globalget server mapname`, the `MapInfo_LoadedGametype` mdl).
  For Duel the test plays a map with implied Duel only.

## Tests (this clone; basedir .suite-base = stock pk3s + this clone's pk3dir)
- `scripts/test-mapmode.sh` before the fix (the old menu.dat over the same basedir):
  FAILED 72. The native paths fail with "server: None" and the false toast; qcmenu
  plays a random map; Duel's picker misses 7 maps.
- After the fix: stock 0.8.6 passes (230 ok). The released slim set (repack/out5, the
  data-slim assets) passes (230 ok). The full 20261107 set passed 224 checks and failed 1
  (qcmenu lms: Home was not described yet). The test now waits for Home; rerun on that
  data: passes.
- `scripts/test-uibridge.sh`: all passed. `--engine`: all passed (store, engine, input,
  no fixture drift).
- `cargo test --offline -p godnotic-loader-core`: ok (fixtures replayed).
- `scripts/test-frontend.sh`: all checks passed (41 ok).
- `scripts/test-old-name.sh`: PASS.
- `scripts/test-lan-private.sh`: PASS (35 ok). It hosts through Create > LAN > Host,
  which goes through the changed `UIB_StartGame`.
- No engine change, so no SDL, sv-release or web rebuild was needed. The web build runs
  the same menu QC.

## Not done / not verified
- Not checked on the tablet. Read-only adb found nothing usable: logcat has no engine
  console, and qconsole.log is from an older run. Input on the tablet was refused by the
  permission system in this session. Steps for the tablet owner: install this branch's
  ci/mapmode APK as the `.perf` package, open Singleplayer, tap CTF, tap a map, tap Play.
  Expected: the match starts on that map in CTF, with no toast. Also try Duel on
  Afterslime (implied Duel).
- The front-end bridge's own `fe_dump maps` topic (touch_frontend.c) still has its own
  mode logic. The Kotlin UI reads the UI bridge tree, not that topic.
