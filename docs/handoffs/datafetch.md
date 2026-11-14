# Handoff: progressive data download (`work/datafetch`)

Scratch clone `~/.cache/godnotic-work/datafetch`. The branch is `origin/main`
(e7c8e9d) with `origin/work/data` (8dbd0e9) and `origin/work/integration`
(44e0595, release candidate 1) merged in, plus this work. The contract every
client implements is `docs/DATA_FETCH.md`. Nothing is published. Android ran
in CI (assembleRelease) and in the JVM tests only, not on a device (round 2:
another agent was using the tablet's test package at the time).

## Round 2 (2026-11-14): what changed

- Merged `origin/work/integration` (1c6eb62; one conflict, `BootActivity`:
  `DataService.start` kept next to the join-link forwarding).
- Menu (f5951e0): every bridge renderer draws a map tile that is not
  `ready` dimmed with a label on its picture (`Downloading`, the wanted
  map's `42%` with a bar along the picture's bottom, `Not downloaded`):
  `touch_uib_render.qc` (`RB_MapTileState`), GTK `widgets.rs` (`map_state`,
  an overlay on the picture, 3 CSS rules), Kotlin `ViewRenderer.kt`
  (`mapState`, a frame over the picture). The hold toast is gone: a held map
  or join opens the dialog `data.hold` (`touch_data.qc`): "Getting the map
  boil", then starting or joining, MB and bar, Cancel (`data.cancel` ->
  `touch_data_cancel`). Back hides it; the command still runs. Hooks in
  `touch_uib_screens.qc`: three lines (build, action, tick). Home untouched.
- Sound (7ccd47c): `.opus`-named files take the worker path (`TouchSnd_IsOgg`).
- Data split (7dcf0dc): `gfx/hud/default/progressbar*` in the core
  (`HUD_DEFAULT_ALWAYS`; hud.qc names it by path); the set id hashes the split
  packs too, so a rule change alone is a new set; the split self-test runs in
  `tests.yml`. First play is now 74,676,756 bytes (+7,151).
- `_lod1/2.iqm` "not found": examined, harmless. The server precaches the LODs
  it has (`precache_playermodel` with `fexists`), the client loads the
  precache list and warns, and the client QuakeC uses a LOD only where its own
  `fexists` finds it, so the full model stands in. Left in the test's known list.
- Web (990c530): the status line sends `set <id>`; `vercel-build.sh` split pins
  are `unpublished` (see Publish order).
- Fixtures (1363957): map rows carry `state`/`progress`.

## What is done, and how it was checked

| Area | State | Check |
| --- | --- | --- |
| Engine (`touch_datafetch.c`, `touch_fs_mount.h`, `touch_join.h`) | status file or `touch_data_status`, cvars, mounts `landed` packs, `touch_data_want` (request file; `Module.xt.dataWant` on the web), holds `map`/`changelevel`/a join until the map is here, cdtracks on the music pack, `fs_rescan` counts | `scripts/test-datafetch-engine.sh SPLIT`: PASS, 17 checks (round 2: + hold dialog and map rows, Cancel, `.opus` on the workers). Builds: `sdl-release`, `sv-release`, `build-web.sh --engine-only` (round 2, all rc 0) |
| First-play claim (task 7) | A stormkeep bot match on the 14 `first_play` packs prints no missing-file line the whole set does not; `boil` mounted late likewise; a join to a server on `xoylent` reconnects by itself and signs on | same script |
| Rust loader core (`fetch/manifest.rs`, `sha256.rs`, `datasets::fetch_all`, `examples/datafetch`) | complete | `cargo test --offline -p godnotic-loader-core --features http-ureq`: 12 + 14 + 15 + 11 pass (round 2; 12 + 11 without the feature; `-p godnotic-loader-android --no-default-features` 0 failed) (the 15 manifest tests also 100 times in 4 parallel loops); clippy `-D warnings` clean with and without the feature; `cargo check` for the three Android targets; CI job Loader green |
| End to end (loader + real set + engine) | complete | `scripts/test-datafetch.sh SPLIT PHONE`: PASS, 15 checks |
| GTK popover (`loader/crates/gtk/src/data_ui.rs`), map tiles (`widgets.rs`) | uses `fetch_all`, resumes an unfinished split install at start; tiles show state and bar | `XT_GNOME_VERSION=50 loader/build.sh --check` passes (SDK 50 is the one installed); `map_tile_download_state` unit test in the SDK; not run in a window |
| Android (`Data*.java`, `GameData`, `BootActivity`, map tiles in `ViewRenderer.kt`) | complete in code | 24 JVM tests by plain `javac` + JUnit (round 2: OK); CI Android (assembleRelease, 3 ABIs) green on 990c530 (the Kotlin tile change compiles). Not on a device |
| Web (stager, page, tests) | complete; status line sends `set` | `scripts/test-web-cache.sh`: 34 node tests, stager checks, 31 browser steps (Chromium and Firefox, headless), round 2 all pass. Real engine, n=3, below |
| Menu (`touch_data.qc/.qh`, tiles in all three renderers) | state helpers, status line, `data.hold` dialog with Cancel, tiles with state and bar, map cache follows `fs_mountpack_count` | engine test checks the dialog, the rows and Cancel; screenshots of the QuakeC renderer looked at by hand (dialog with 40% bar, picker with dimmed tiles); fixtures reseeded |
| Sound | Opus on the `snd_lazyload_async` workers, also for `.opus` names; `snd_lazyload_maxlength` 4 s by decoded length | timings below; engine test checks an 8 s `.opus` goes to a worker and a 1 s one does not |

Measured (desktop, `taskset`, load 6-10):

- Time to first play, loader, one shared link (`TIMINGS="100 20 5" RUNS=3`):
  6.41 s (6.28-6.53) at 100 Mbit/s, 30.04 s (29.93-30.11) at 20, 119.63 s
  (119.58-119.99) at 5. The link alone needs 6.0 / 29.9 / 119.5 s for the
  74.7 MB. Unthrottled local: 1.8-1.9 s. Bytes served before the game may
  start: exactly 74,669,605 (round 2, with the progressbar in the core:
  74,676,756 of packs; the test passes on split-G3).
- Web, real wasm engine, headless GPU Chrome, CDP 20 Mbit/s, n=3 medians:
  first play stored 38.0 s, menu 39.7 s (88.3 MB received), stormkeep bot
  match 46.7 s, `map boil` on demand 5.5 s, everything stored 176 s. Before
  the split the menu took 176 s.
- Sound phase of a stormkeep load, 3 runs each, old engine / new engine:
  set G2 as built 137-170 ms / 112-147 ms; G2 with the 30 effects that
  `--sfx-precache-kib 48` keeps as Vorbis turned into Opus 794-1127 ms /
  107-292 ms. So `--sfx-precache-kib 0` is safe for engines with threads and
  this change. It is not for the web build (no threads: those files decode at
  precache) nor for released engines, and the tablet was not measured.

## Not done, per client

- **Android**: nothing on a device (service start rules, `onTimeout`,
  notification, metered gating, JNI `fs_mountpack`, the status file read by a
  real engine). CI compiles but runs no unit tests. No real GitHub download
  was made (metered uplink). An old install with data and maps but no music
  is asked for first_play before it starts (contract as written; could start
  on the old packs instead). `paused` is never written (no pause control).
- **Rust loader**: the GTK page was not opened by hand; no pause/resume
  button beyond Stop; GPU ASTC detection is still `None` in `data_ui.rs`
  (so the dxt manifest, else the stock set), as before this work. The loader
  mounts through the status file only (the engine polls it once a second),
  not through the UI-bridge stdin.
- **Web**: `vercel-build.sh` not run end to end (only `fetch_split` and the
  stager); DXT set not run in a browser; Safari and phones untested; real
  engine run was Chrome only. Existing visitors re-download the whole set
  once (0 chunks in common); a whole old set keeps playing until the new one
  is complete.
- **Menu**: the classic Xonotic map dialog lists only maps whose `.bsp` is
  here. The GTK and Kotlin tiles were not seen rendered (GTK compiled and
  unit-tested, Kotlin compiled in CI).
- **Sound**: tablet timings missing.

## Known issues

- **Merge forward with `main` is not done.** `main` gained cac2da0 (Home tab
  bar) after RC1; RC1 (`work/integration`, in this branch) conflicts with it
  in `touch_uib_screens.qc`, `touch_uib_render.qc`, `widgets.rs`,
  `ViewRenderer.kt`, `UI_BRIDGE.md` and most fixtures: the Home conflict the
  integration agent is resolving. This branch's own work merges cleanly with
  `main` (checked with `git merge-tree`: 502f8a1 into cac2da0, no conflict).
  Merge the re-based integration branch into this one when it exists (if it
  is a true rebase with new shas rather than a merge, redo this branch's
  merge on top of it instead of merging both RC1 histories).
- `scripts/test-uibridge.sh --engine`: `input_test` fails 14 checks (taps on
  Home cards, map picker, join flow) here and identically on the RC1
  integration run (`~/.cache/godnotic-work/integ-logs/uib-engine.log`): not
  from this branch. Fixture drift after reseeding only the map rows:
  `welcome` (`minItem` 240 -> 200, RC1's Home) and, timing-dependent,
  `servers` (`Looking for servers…`); left for the integration owner.
- `_lod1/2.iqm` "not found" when joining a server that has the detail pack:
  harmless (see round 2), kept in the test's known list.
- `fetch_all` writes `<data>/touch/music-ready.txt`: never link the repo's
  `touch/` into a test data dir that a downloader writes to.
- Local split sets: `data-out/split-G3` is this branch's ASTC split of
  `packs-G2` (set `20261107-7f8d55483341`); `split-G2` is the older cut
  (no core progressbar) and `split-D2` the DXT one (old set id rule).

## Test commands

```sh
cd ~/.cache/godnotic-work/datafetch
export PATH=~/.cache/xt-assets/bin:$PATH DP_LINK_JPEG=dlopen
. scripts/lib/xonotic-shlib.sh && xonotic_compile_qc_only && xonotic_compile_engine_only; cd ~/.cache/godnotic-work/datafetch
S=~/.cache/godnotic-work/data-out/split-G3; P=~/.cache/godnotic-work/data-out/packs-G2
export LD_LIBRARY_PATH=~/.cache/godnotic-slimtest/lib TMPDIR=$PWD/.suite-tmp; mkdir -p $TMPDIR
taskset -c 0-7 scripts/test-datafetch-engine.sh $S          # about 6 min; PASS 17
taskset -c 0-7 scripts/test-datafetch.sh $S $P              # about 3 min; PASS 15; TIMINGS="100 20 5" RUNS=3 adds 9 min
bash scripts/test-data-split.sh && taskset -c 0-7 bash scripts/test-data-split.sh $S $P
UIB_BASEDIR=~/.cache/godnotic-work/datafetch-base bash scripts/test-uibridge.sh [--engine]   # basedir with THIS clone's pk3dir
(cd loader && cargo test --offline -p godnotic-loader-core --features http-ureq \
  && cargo test --offline -p godnotic-loader-core \
  && cargo clippy --offline -p godnotic-loader-core --all-targets -- -D warnings && XT_GNOME_VERSION=50 ./build.sh --check)
bash scripts/test-web-cache.sh && bash scripts/test-old-name.sh
(cd engine/darkplaces && make sv-release DP_LINK_JPEG=dlopen)
# Android logic on the JVM (JDK in ~/.local/opt/jdk-17.0.21+9, junit and hamcrest jars from ~/.gradle/caches):
cd android/app/src; P=main/java/io/github/dixonsolutions/godnotic
javac -d $OUT -cp $JUNIT:$HAMCREST $P/Data{Manifest,Install,Status,Fetch,Session}.java test/java/io/github/dixonsolutions/godnotic/DataFetchTest.java
java -cp $OUT:$JUNIT:$HAMCREST org.junit.runner.JUnitCore io.github.dixonsolutions.godnotic.DataFetchTest
```

The round-1 sound timing rig (`datafetch-tmp/snd/lt.sh`) was deleted with
`datafetch-tmp` at the end of round 2; `lt.sh` is short: a cold `map` with
`touch_loadtime 1`, reading `LOADTIME   sound load/decode` from the log.

## Edits in other agents' files (each a small hunk, separate commits)

- `XonoticActivity.java` (one line: `DataService.start(this)`),
  `AndroidManifest.xml` (one `<service>`), `strings.xml` (two strings).
- `touch_uib_src.c`: maps rows get `state` and `progress`; the source polls
  every 0.5 s and rebuilds when `TouchData_Generation()` changes.
- `touch_joinfail.c`: no failure report while `TouchData_JoinPending()`.
- `touch_uib_screens.qc`: `UIB_StartGame` refuses a downloading map with a
  toast and asks for it; selecting a map row asks for it.
- `menu.qc`, `host.c`, `fs.c`: one `// Touch:` line each (two in `host.c`).
- `loader/crates/gtk/src/data_ui.rs`: `fetch_all`, resume at start.
- `web/appdata.js`: accepts a folder in the split layout (`kind: 'set'`).
- Round 2: `touch_uib_screens.qc` (3 hook lines: build `data.hold`,
  `TouchData_Action`, `TouchData_UIBTick`), `touch_uib_render.qc`
  (`RB_MapTileState`, two calls, `state progress` added to the fetched cells),
  `widgets.rs` (`map_state`, overlay in `card_grid`, a unit test),
  `style.css` (3 rules), `ViewRenderer.kt` (`mapState`, the tile's picture in
  a frame with label and bar), `UI_BRIDGE.md` (the `maps` cells),
  `tests.yml` (one step: the split self-test), map-row fixtures. Nothing on
  Home.
- `engine/darkplaces` and `qcsrc` are components: `component-push.sh` was not
  run and `components.json` is untouched; the integrator does both.

## Per-map state contract

- Engine `maps` / `maps:<mode>` rows: `state` = `ready` | `downloading` |
  `missing`, `progress` = 0..1 for the wanted map, -1 otherwise. The source
  revision bumps when `fs_mountpack_count` changes, the downloader becomes
  active or idle, the wanted map changes, or its progress moves by 256 KiB;
  the source polls twice a second, so at most 2 bumps a second.
- Select: `touch_data_want <map>` (QuakeC `TouchData_Want`), which puts the
  map's `needs` first. Activate: refused until `ready` in `UIB_StartGame`;
  below that, the engine holds any `map`/`changelevel` for such a map in
  `touch_data_pending` and runs it when the `.bsp` appears.
- Whole-set progress: `touch_data_state`, `touch_data_done_kb`,
  `touch_data_total_kb`, `touch_data_message`; wanted map:
  `touch_data_want`, `touch_data_want_done_kb`, `touch_data_want_total_kb`.
  C: `TouchData_MapState()`, `TouchData_Generation()`. QuakeC:
  `touch_data.qh` (`TouchData_StatusLine()`).

## Web folder and the app's packs

`GameDataProvider` has to expose every finished `godnotic-*.pk3` under its
`install_as` name with its real size, never `.part` files. The web folder
source and `appdata.js` accept a folder in which every first_play pack of the
profile's manifest is present by name and size; packs present are not
downloaded. The app's set must be the same `set` as the web profile's.

## Publish order (nothing here was dispatched)

The workflow is `~/.cache/godnotic-work/data-out/workflows/slim-data.yml`
("Data sets"); it replaces `.github/workflows/slim-data.yml` in
Godnotic-data (today that file is the older one-archive "Slim data").
Godnotic-data is also the game-code component: a commit there touching only
`.github/` is never imported, but commit it when no component push is racing.

1. **Merge this branch into `main`** (it carries `work/data` and RC1), run
   `component-push.sh all`, release. Nothing changes for anyone: no manifest
   is published, every client falls back to the single archive, and the web
   build sees 404 for the manifests and stages as today (pins `unpublished`
   are not consulted on a 404). Note the sha: `tools_ref`.
2. **Build**: in Godnotic-data, commit the workflow, dispatch `Data sets`
   with `mode=build`, `source_tag=data-20261107`, `tools_ref=<sha>`,
   `profiles=astc,dxt`. It downloads `data-20261107` (1.2 GB, inside
   GitHub), repacks per profile (`repack-phone-assets.py`, default
   `--sfx-precache-kib 48`), splits (`split-data-packs.py`, runs
   `test-data-split.sh` on the result) and uploads artifacts `data-astc`
   (~0.75 GB: `Godnotic-data-astc.zip`, `godnotic-data-astc.json`,
   `Godnotic-data.zip` old layout, `phone-manifest.json`, sizes, SHA256SUMS)
   and `data-dxt` (~0.85 GB). Publishes nothing. ~35-45 min.
3. **Check CI's bytes** before publishing, here or on a runner (they are not
   these local bytes; libopus differs): `scripts/test-data-split.sh`,
   `scripts/test-datafetch-engine.sh`, `scripts/test-datafetch.sh` on the
   unpacked artifact (PUBLISH.md step 3 has the unzip lines). Downloading
   1.6 GB here needs an unmetered link; otherwise run them in a workflow.
   Read the two set ids: `jq -r .set godnotic-data-astc.json` and
   `... dxt.json` (form `20261107-<12 hex>`; the hash covers the source packs
   and every split pack's sha256, so it names exactly those bytes).
4. **Move the web pins** to those ids in `scripts/vercel-build.sh`
   (`SPLIT_SET_ASTC`, `SPLIT_SET_DXT`; or the Vercel env vars
   `XT_SPLIT_SET_ASTC` / `XT_SPLIT_SET_DXT`) and release that. Until the pins
   match, a Vercel build after step 5 stops with "the published astc split
   set is X, the build is pinned to unpublished" and the previous deployment
   stays up (it carries its own staged data). Android and the loader have no
   pin: they take whatever `set` the manifest names.
5. **Publish**: dispatch `mode=publish`, `run_id=<build run>`, `revision=1`,
   `update_compat=false`. Per profile: creates the immutable prerelease
   `data-astc-20261107-r1` / `data-dxt-20261107-r1` (refused if the tag holds
   other bytes), uploads the archive, re-downloads and compares it, then the
   manifest (with `archive.url` set to that immutable release); then on
   `data-slim` uploads `godnotic-data-astc.json` and `godnotic-data-dxt.json`
   beside, swaps by rename, verifies. Archives before manifests: a client
   that accepted a manifest does not fall back to the single archive.
   `data-slim/Godnotic-data.zip` and `data-slim-20261107-1` are not touched.
6. Then: new installs fetch 74.7 MB (ASTC; 229.6 MB DXT) and play, the rest
   streams in. Complete old installs record `legacy <set>` and fetch nothing.
   Web visitors holding today's set download the new set once in the
   background (373 MB ASTC, 846 MB DXT; no chunk in common, only the fork's
   13.5 MB is shared) while the old set keeps playing. Avoiding that would
   need per-entry chunks shared by both layouts: not cheap, not done.
7. Later, separate decisions: `update_compat=true` (same run, replaces
   `data-slim/Godnotic-data.zip` for released apps) only after a tablet load
   with a released app is acceptable. `--sfx-precache-kib 0` never as a
   default, and only in a set built after every engine in use has the
   threaded Opus path, and never for the web build (no threads).

**What stays working for released clients**: released apps read
`data-slim/Godnotic-data.zip` (moving, untouched unless `update_compat`);
the released web build pins `data-slim-20261107-1/Godnotic-data.zip` by
sha256 (immutable, never touched). Released clients never read the manifests.

**Rollback**: delete `godnotic-data-astc.json` and `godnotic-data-dxt.json`
from `data-slim` (asset delete; or upload a non-format-1 file). New installs
and the web build then take the single archive as today. Clients that already
stored a manifest keep it in charge on a 404 (`Manifest::stored`,
`DataSession.refresh` ABSENT) and keep fetching from the immutable
`data-*-20261107-r1` archives, so **never delete those releases**. A bad set
is replaced by a new `revision` (new tags, new set id), never by changing
bytes under an existing tag.

## Tablet checklist

`adb logcat -s Godnotic`; `D=/storage/emulated/0/Android/data/<package>/files`.
Needs the manifest served (the `adb reverse` test build) and this engine.

1. Fresh install on Wi-Fi: the setup step shows about 75 MB and says the rest
   follows; logcat `Data manifest: set ..., 72 packs`, 14 `Installed
   godnotic-...`; the game starts; `ls $D/data` has 14 `godnotic-*.pk3`,
   `.godnotic-set`, `.godnotic-data.json`, no `.part`.
2. In the menu: `Data download: fetching`, a silent notification "Getting
   the rest of the maps: N of 355 MB"; `cat
   $D/userdata/data/data/touch/data-status.txt` shows `state fetching`,
   rising `done_kb`, `landed` lines; at the end `state complete`, no
   notification, 72 packs.
3. During 2, the map list: maps appear without a restart; music starts once
   its pack lands.
4. During 2, pick a map late in the queue: logcat `The game asks for map`;
   the status file gains `want`; its map pack lands last; the toast shows
   progress; `data-request.txt` reads `want <map> <n>`.
5. During 2, join a server on a map not yet here: no "cannot load map", the
   toast, then the client reconnects by itself.
6. `am force-stop` at about half of first play: `.part` and `.part.state` in
   `$D/data`; the next start carries on without asking.
7. Force-stop during 2: straight into the game, `fetching` resumes, no pack
   is installed twice.
8. Home during 2: the notification stays and progress continues with the
   screen off. Swipe the app away: the next start resumes.
9. Airplane mode: before first play a Retry screen; during 2 `state
   waiting`, no notification, and `fetching` again when the network is back;
   with first_play in and no network the game starts at once.
10. Wi-Fi set to metered: the setup offers "Use mobile data" / "Wait for
    Wi-Fi"; after "Wait", the rest stops on a metered network (`waiting`) and
    resumes on an unmetered one.
11. An old full install updated to this build (the most important one):
    straight into the game; logcat `A complete install from before the split
    stands for set`; `.godnotic-set` is exactly `legacy <set>`; every
    `xonotic-*.pk3` still there, no `godnotic-*.pk3`; data use a few tens of
    KB; the same on two more starts.
12. No manifest served: the setup and download behave as the current release.
13. AndySU host with a new payload: logcat `This host does not declare
    DataService`; no notification, packs still land and mount while the game
    is open; no crash.
14. Load time with the new set (`touch_loadtime 1`): the sound phase, before
    deciding on `--sfx-precache-kib 0`.
