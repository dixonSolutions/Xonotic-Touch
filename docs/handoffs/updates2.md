# HANDOFF: work/updates2 (Settings > Updates is never empty)

Base: `origin/main` 7a8d3ee (release round 2, v1.2.204). Clone `~/.cache/godnotic-work/updates2`.
Only the data component changed under `engine/` (menu QuakeC: `touch_uib_settings.qc`,
`touch_update_util.qc/qh`), so the merge needs `scripts/component-push.sh data` and the pin.
`engine/darkplaces` is untouched. `component-push.sh` was not run here.

## What the owner saw, and why

"Update still has no update info and actions" on the tablet: the side-by-side `.perf` build has no
AndySU host (logcat "No AndySU host"; its `andysu-update-status.txt` said `unavailable`). The
screen built in `work/updates` showed rows only under a host, so it showed "Version Godnotic"
(`g_xonoticversion` holds the name) and one caption. The app's own updater was running all along
(`update-status.txt` said `error / Could not reach the update server`: that build's feed was a dead
port) but the screen never read it.

### Which builds have an AndySU host (evidence)

| Build | Host? | Evidence |
| --- | --- | --- |
| `Godnotic-live-1.2.204-arm64.apk` (release v1.2.204 = 7a8d3ee, run 43566377088) | yes | host dex has `com.andysu.api.{Host,HostApi,UpdateClient,UpdateClient$Listener}` with every method the bridge calls; manifest `appComponentFactory=com.andysu.host.AndySUComponentFactory`, `InitProvider` (main process); `assets/andysu/config.json` channel `live-arm64`, host_api 3; the payload dex (`assets/andysu-factory/dex/classes.dex`) carries no copy of `com.andysu.*` (only the reflection strings), and `PayloadClassLoader` loads `com.andysu.api.*` from the parent, so `Class.forName("com.andysu.api.Host")` in `AndySUCompat` reaches the class `InitProvider` filled. Detection works on a real host (static check; not run on a device: the brief allows the perf package only). |
| `Godnotic-1.2.204-arm64-v8a.apk` (same release; the one the release notes list) | **no** | zip directory (range request, 256 KB): no `assets/andysu/`, one `classes.dex`, no host component factory |
| local `.perf` builds (`~/.cache/xt-perf/build-apk.sh`, scripts/android-build.sh) | no | pulled `base.apk` from the tablet: no `assets/andysu`, no AndySU in the manifest |
| F-Droid-style / any Gradle-built APK | no | the host only comes from `andysu split` in the `andysu-live` CI job |
| Linux (Flatpak, Click), web | no (not Android) | `start.sh` publishes `managed`; the web publishes nothing |

So the release notes point Android players at the APK without a host. Those installs update
through `AppUpdater`, which prefers the release's live APK (`ReleaseAsset`, Android 11+), so their
first in-app update moves them onto AndySU. Not changed here (workflow notes are the
coordinator's): consider listing `Godnotic-live-*` in the release notes.

## Done and verified

- **Android without a host** (`UIB_SecUpdatesApp`): "Updates come from Godnotic's releases on
  GitHub...", **Version 1.2.204**, "Channel stable · arm64-v8a", the release a check found with
  size and why it waits, **Download and install** (confirm on a metered connection), a note when
  the found APK is the live one ("This version brings AndySU..."), a progress bar with
  "7.4 MB of 34.5 MB · 990 KB/s · 28 s left", **Install now** for a downloaded release (also after
  Android's install-permission screen), **Check now**, "Last checked N min ago: result",
  **Automatic updates**, **Update on Wi-Fi only** (new), and **Apply live** shown off and
  disabled with "Needs AndySU, in the Godnotic-live APK".
  - `UpdateBridge` appends key=value details after its eight lines (format in
    `touch_update_util.qh`): via, channel, abi, wifi, live=na, net, hold, size, asset, done,
    total, speed, eta, checked_min, checked_result. Decisions in `AppUpdateState` (plain Java).
  - `AppUpdater`: `wifi_only` (default on; off lets a background download use mobile data, never
    offline), `checked_at` / `checked_result` recorded by every check that ran.
  - `XonoticActivity`: `wifi-on/off` requests; `auto-*` and `wifi-*` rewrite the record
    (`UpdateBridge.refresh()`, keeps the state lines) and start a due download; the update
    service refreshes the record every 30 s (minutes since the check, network); downloads report
    bytes. All writers of `update-status.txt` share one lock and skip identical records.
- **Android with a host**: unchanged screen; the version fallback no longer ends at the game's
  name (`TouchUpdate_versionNumber`: host version, then the APK version, then "development
  build").
- **Linux** (`UIB_SecUpdatesManaged`): how updates arrive (`start.sh` now writes `via=flatpak`,
  `click` or `local`), the version, and **Check now**, which asks `changelog.txt` for the newest
  release (also once when the screen opens, unless `cl_curl_enabled 0`) and says "Version X is
  out" with where to update. A source build (no status) shows only "development build".
- **Web**: `web/shell.js` passes `+set _godnotic_platform web`; the screen says a reload brings
  the newest release and shows the build (QuakeC watermark, the commit). No check.
- The Updates pane's subtitle is "Version, checks and downloads" (was "What is new"); every
  settings fixture carries it.
- `docs/ANDROID.md`: which APK has a host, and a table of the four screens.

### Tablet (SM-X306B, `.perf` package, data over USB only)

Test-only builds from this branch (`~/.cache/godnotic-work/updates2-tools/build.sh`: package
`.perf`, `AppUpdater` feed and downloads at `http://127.0.0.1:8765`, game data at a dead port;
`FAKE=1` adds the previous agent's fake AndySU host). Feed: `updates2-tools/feed.py` over
`adb reverse tcp:8765`, serving the test APK itself as `Godnotic-live-1.2.205-arm64.apk`
(size + sha256 digest like GitHub), throttled to 1-1.5 MB/s. Nothing went over the tablet's
network.

1. No host, automatic on: the start-up check found 1.2.205, downloaded it in the background,
   screen "Version 1.2.205 is downloaded · Install now" (`tab-03-nohost-ready.png`).
2. Automatic off (switch stays off; record line 8 `off`), feed moved to 1.2.206, **Check now**:
   "Version 1.2.206 is available · 34.5 MB · Automatic updates are off", the AndySU note,
   **Download and install** (`tab-05-nohost-available.png`).
3. **Download and install**: live bar "7.4 MB of 34.5 MB · 990 KB/s · 28 s left", Check now
   disabled (`tab-06-nohost-downloading.png`); at the end Android's "Install unknown apps" screen
   opened (`tab-07-after-download.png`). I did not grant it (a device security setting); back in
   the game the screen says Android needs the permission, with **Install now**
   (`tab-08-needs-permission.png`). The install itself was not run.
4. Fake host: AndySU screen, **Check now** with automatic off -> "Downloading version 1.2.999 ·
   7.0 MB of 30.0 MB · 2.3 MB/s · 11 s left" -> "Version 1.2.999 is ready · Apply now" -> Apply
   now -> up to date on 1.2.999 (`tab-10-host.png` ... `tab-13-host-applied.png`).

Screenshots: `~/.cache/godnotic-work/updates2-shots/` (tablet `tab-*.png`; QuakeC renderer
offscreen `nohost-available.png`, `nohost-downloading.png`, `nohost-uptodate.png`, `host.png`,
`linux.png`, `web.png`, `devbuild.png`, each with its `uib_dump` JSON).

## Tests

- `scripts/test-android-updates.sh` (now AndySUStateTest + new AppUpdateStateTest): OK (17 tests)
- `scripts/test-host-resources.sh`: host resources: ok
- `cargo test --offline -p godnotic-loader-core` (loader/): 12 + 11 passed (replays the new fixtures)
- `scripts/test-uibridge.sh --engine` (clone basedir `.suite-base`, engine built from 7a8d3ee):
  all passed, fixtures no drift
- `scripts/test-old-name.sh`: passed
- Fixtures regenerated with `scripts/uib-fixtures.py --write`: new `settings-updates-app`
  (Android, no host), `settings-updates-linux` (Linux, Flatpak); `settings-updates` (host)
  unchanged apart from the subtitle; the other settings fixtures, `mod-page` and `seq-setting`
  only the subtitle.
- CI on `ci/updates2` (c1e1a0b): Android, Tests, Loader, Web green. Head re-pushed with the
  subtitle, fixtures and docs.

## Not done / known

- `scripts/test-android-updates.sh` is not in any workflow (it was not before either).
- The real AndySU host was not run on a device (perf package only); detection is checked
  statically against the released APK.
- The final install step (Android's confirm dialog, the restart into the new version) was not
  run: it needs the "Install unknown apps" permission for the test package.
- Under the fake host the channel line shows only the ABI: a test build has no
  `assets/andysu/config.json`. Real hosts have it.
- A check that fails while a release is already downloaded shows "available" with "Ready to
  download" (an old `publishCheckFailed` behaviour); **Download and install** then installs from
  the cache without downloading.
- The tablet is left with the `.perf` build of this branch without the fake host (`CODE 9414`).
