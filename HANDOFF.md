# Handoff: progressive data download (`work/datafetch`)

Scratch clone `~/.cache/godnotic-work/datafetch`. The branch is `origin/main`
(e7c8e9d) with `origin/work/data` (8dbd0e9) merged in, plus this work. The
contract every client implements is `docs/DATA_FETCH.md`. Nothing is
published; nothing ran on a device.

## What is done, and how it was checked

| Area | State | Check |
| --- | --- | --- |
| Engine (`touch_datafetch.c`, `touch_fs_mount.h`, `touch_join.h`) | status file or `touch_data_status`, cvars, mounts `landed` packs, `touch_data_want` (request file; `Module.xt.dataWant` on the web), holds `map`/`changelevel`/a join until the map is here, cdtracks on the music pack, `fs_rescan` counts | `scripts/test-datafetch-engine.sh SPLIT`: PASS, 14 checks. Builds: `sdl-release`, `sv-release`, wasm (5,484,423 bytes, built by the web agent from a mirror) |
| First-play claim (task 7) | A stormkeep bot match on the 14 `first_play` packs prints no missing-file line the whole set does not; `boil` mounted late likewise; a join to a server on `xoylent` reconnects by itself and signs on | same script |
| Rust loader core (`fetch/manifest.rs`, `sha256.rs`, `datasets::fetch_all`, `examples/datafetch`) | complete | `cargo test --offline -p godnotic-loader-core --features http-ureq`: 12 + 14 + 15 + 9 pass (the 15 manifest tests also 100 times in 4 parallel loops); clippy `-D warnings` clean with and without the feature; `cargo check` for the three Android targets; CI job Loader green |
| End to end (loader + real set + engine) | complete | `scripts/test-datafetch.sh SPLIT PHONE`: PASS, 15 checks |
| GTK popover (`loader/crates/gtk/src/data_ui.rs`) | uses `fetch_all`, resumes an unfinished split install at start | `loader/build.sh --check` passes; not run in a window |
| Android (`Data*.java`, `GameData`, `BootActivity`) | complete in code | 24 JVM tests by plain `javac` + JUnit (also after the package move); earlier `gradle --offline :app:testReleaseUnitTest :app:lintVitalRelease` in a scratch copy; CI job Android (assembleRelease) green. Not on a device |
| Web (stager, page, tests) | complete | `scripts/test-web-cache.sh`: 34 node tests, stager checks, 31 browser steps (Chromium and Firefox, headless). Real engine, n=3, below |
| Menu QuakeC (`touch_data.qc/.qh`) | state helpers, status line, toast while a map or join is held, map cache follows `fs_mountpack_count` | compiles; exercised only through the engine tests (no screenshot test) |
| Sound | Opus on the `snd_lazyload_async` workers; `snd_lazyload_maxlength` 4 s by decoded length | timings below |

Measured (desktop, `taskset`, load 6-10):

- Time to first play, loader, one shared link (`TIMINGS="100 20 5" RUNS=3`):
  6.41 s (6.28-6.53) at 100 Mbit/s, 30.04 s (29.93-30.11) at 20, 119.63 s
  (119.58-119.99) at 5. The link alone needs 6.0 / 29.9 / 119.5 s for the
  74.7 MB. Unthrottled local: 1.8-1.9 s. Bytes served before the game may
  start: exactly 74,669,605.
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
- **Menu**: the bridge screens do not draw the `state`/`progress` cells yet
  (screens agent). The classic Xonotic map dialog lists only maps whose
  `.bsp` is here. No Cancel button for a held map or join (`touch_data_cancel`
  exists).
- **Sound**: `.opus`-named files (not `.ogg` containers) do not take the
  async path; tablet timings missing.

## Known issues

- `gfx/hud/default/progressbar` was asked for once during a join on the
  first_play packs and is in `extras`; it belongs in the core (data split).
- A server that has the `detail` pack names `models/player/*_lod1/2.iqm`;
  a client without it yet prints 17 "not found" lines. Not seen to break
  anything; not examined further.
- The web status line sends no `set` (the engine prints `set ""`).
- `vercel-build.sh` pins the two local set ids (`XT_SPLIT_SET_ASTC`,
  `XT_SPLIT_SET_DXT`); CI builds other bytes, so the pins must move (or be
  emptied) when the real sets are published.
- The Tests and Web workflows failed on db878d3: `test-web-cache.sh` died
  where it should skip the browser part. Fixed after that push (verified here
  with no browser visible: exit 0); see the last CI run of the branch.
- `fetch_all` writes `<data>/touch/music-ready.txt`: never link the repo's
  `touch/` into a test data dir that a downloader writes to.

## Test commands

```sh
cd ~/.cache/godnotic-work/datafetch
export PATH=~/.cache/xt-assets/bin:$PATH DP_LINK_JPEG=dlopen
. scripts/lib/xonotic-shlib.sh && xonotic_compile_qc_only && xonotic_compile_engine_only; cd ~/.cache/godnotic-work/datafetch
S=~/.cache/godnotic-work/data-out/split-G2; P=~/.cache/godnotic-work/data-out/packs-G2
export LD_LIBRARY_PATH=~/.cache/godnotic-slimtest/lib TMPDIR=~/.cache/godnotic-work/datafetch-tmp/t
taskset -c 12-19 scripts/test-datafetch-engine.sh $S          # about 5 min
taskset -c 12-19 scripts/test-datafetch.sh $S $P              # about 3 min; TIMINGS="100 20 5" RUNS=3 adds 9 min
(cd loader && cargo test --offline -p godnotic-loader-core --features http-ureq \
  && cargo test --offline -p godnotic-loader-core \
  && cargo clippy --offline -p godnotic-loader-core --all-targets -- -D warnings && ./build.sh --check)
bash scripts/test-web-cache.sh && bash scripts/test-old-name.sh
(cd engine/darkplaces && make sv-release DP_LINK_JPEG=dlopen)
# Android logic on the JVM (JDK in ~/.local/opt/jdk-17.0.21+9, junit and hamcrest jars from ~/.gradle/caches):
cd android/app/src; P=main/java/io/github/dixonsolutions/godnotic
javac -d $OUT -cp $JUNIT:$HAMCREST $P/Data{Manifest,Install,Status,Fetch,Session}.java test/java/io/github/dixonsolutions/godnotic/DataFetchTest.java
java -cp $OUT:$JUNIT:$HAMCREST org.junit.runner.JUnitCore io.github.dixonsolutions.godnotic.DataFetchTest
```

The sound timing rig is `~/.cache/godnotic-work/datafetch-tmp/snd/lt.sh`
(`xonotic-before` beside it is the engine without the sound change).

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

## Publish order

1. Merge `work/data`, then this branch, into `main`; run
   `component-push.sh` for both components; note the sha. Old clients are
   unaffected: no manifest exists yet.
2. In Godnotic-data: commit `workflows/slim-data.yml`, run `mode=build` with
   `tools_ref=<sha>`, validate the artifacts (`scripts/test-data-split.sh`,
   then `scripts/test-datafetch-engine.sh` and `scripts/test-datafetch.sh` on
   CI's bytes).
3. Release the clients (Android, Flatpak/loader, web with the set pins moved
   to CI's set ids or emptied). They use the single archive until step 4.
4. `mode=publish`: the immutable releases `data-astc-20261107-r1` and
   `data-dxt-20261107-r1` (archives first), then `godnotic-data-astc.json`
   and `godnotic-data-dxt.json` on `data-slim` last. From that moment new
   installs are split; complete old installs record `legacy <set>` and fetch
   nothing. The web deploy must be built after the manifests exist (it stages
   the split at build time) and its `archive.url` must be reachable from
   Vercel.
5. `update_compat=true` only after a tablet load with a released app is
   acceptable; `--sfx-precache-kib 0` only in a set published after every
   client in use has this engine, and not for the web profile.

Never publish a manifest whose `archive.url` asset is not up: clients that
accept the manifest do not fall back to the single archive on a 404 of the
archive, they wait and retry.

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
