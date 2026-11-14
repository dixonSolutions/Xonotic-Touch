# HANDOFF: work/updates (Settings > Updates on Android, AndySU)

Based on `origin/work/integration` at 44e0595. Three commits on top.

## Done and verified

- Settings > Updates on an AndySU install shows: what delivers updates (AndySU, signed, rolls
  back an unhealthy build), the running version with channel and ABI, a download progress
  bar (bytes, speed, time left), a staged release with **Apply now**, a found release that
  waits (Wi-Fi, or automatic off) with **Download now** (asks first on a metered network),
  **Check now** with last-checked time and result, and switches **Automatic updates**,
  **Update on Wi-Fi only**, **Apply live**. Linux, web and APKs without an AndySU host show
  only the version and What is new, as before.
- Android side: `AndySUBridge` (gather and write), `AndySUState` (decisions, plain Java),
  `AndySUCompat` (feature-detected host calls: apply policy, core state, status listener),
  `UpdateNotice` (progress notification on the "Game downloads" channel).
- QuakeC: `touch_uib_settings.qc` (screen, actions, switch values), `touch_update_util.qc/qh`
  (status reader with key=value extras after the eight old lines), one hook in `UIB_Tick`
  (`touch_uib_screens.qc`) that rebuilds the screen when the status file changes.
- What comes from AndySU and what is app-side: table in `docs/ANDROID.md`, "Updates under
  AndySU".
- Tablet (SM-X306B, side-by-side `.perf` package with a test-only fake AndySU host that
  simulates host API 3 locally, no network): Check now -> downloading with live progress
  (real `.part` file accounting) -> staged -> Apply now -> up to date; notification with a
  progress bar while downloading, gone after; Apply live off -> policy next_start;
  Automatic off -> host mode check_only; Check now with automatic off -> download_only for
  that check, back to check_only when it ends; preferences survive a restart.

## Not done / not verified

- The metered path (Wi-Fi only holding a download, "Download now" confirm on mobile data)
  was not run on a device: the tablet reports an unmetered network and its network must not
  be toggled. Covered by the JVM tests and the offscreen QuakeC screenshots only.
- Not run against a real AndySU host build (the andysu-live CI job builds those).
- `work/updates` is not merged with `origin/main` yet: `work/integration` is being rebased onto
  main (cac2da0). My commits apply onto `origin/main` without source conflicts; only
  `tests/uibridge/fixtures/settings-updates.json` must be regenerated on the merged tree.

## Tests

- `scripts/test-android-updates.sh` (JVM, AndySUState): OK (11 tests)
- `scripts/test-host-resources.sh`: host resources: ok
- `scripts/test-uibridge.sh`: all passed; `tests/uibridge/engine_test.py`: all passed
- `scripts/uib-fixtures.py --check`: settings-updates same; servers and welcome drift on the
  base commit too (not from this branch)
- `cargo test --offline -p godnotic-loader-core`: 11 + 11 passed
- CI on `ci/updates` (Android, Tests, Loader): green for 55f732a; re-run for the head.

## Known issues

- The Updates pane's subtitle still says "What is new" (changing it would regenerate every
  settings fixture).
- While the game is closed the host's periodic job runs on any network (its frozen config);
  the app leaves the host in check_only when it goes to the background unless a release is
  already staged (a staged release is promoted at the next start only in auto mode).
