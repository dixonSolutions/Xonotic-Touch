# Ubuntu Touch launch contract

Ubuntu Touch runs click apps inside AppArmor confinement (`aa-exec`), which is far
stricter than Flatpak or a desktop session. This document is the contract every
script in the launch path must honour.

Reported by [issue #18](https://github.com/dixonSolutions/Xonotic-Touch/issues/18)
(Xiaomi Redmi Note 9 Pro, UT 24.04-1.4) and [issue #19](https://github.com/dixonSolutions/Xonotic-Touch/issues/19)
(immediate exit after load when the instance lock path is not AppArmor-writable):

```
aa-exec[19008]: bin/start.sh: 5: dirname: Permission denied
aa-exec[19006]: xonotic-touch: engine binary not found at //bin/xonotic
```

`dirname` was denied, `$(dirname "$0")` returned an empty string, so the app root
collapsed to `/` and the launcher aborted before the engine ever started.

## 1. Confinement rules

| Rule | Consequence for us |
|------|--------------------|
| Host binaries outside the click tree are not exec'able (`dirname`, `mkdir`, `tar`, `curl`, `flock`, ...) | Every runtime tool must ship inside the package; optional host tools must be probed before use |
| Files inside the click tree are exec'able | `bin/busybox` and its applet symlinks work |
| The click manifest only accepts `policy_groups`, not custom AppArmor rules | We cannot whitelist host binaries; bundling is the only fix |
| Writable paths are only under `XDG_*/<APP_PKGNAME>` | Game data lives in `$XDG_DATA_HOME/<APP_PKGNAME>` (e.g. `~/.local/share/xonotictouch.dixonsolutions/`). Writing to `~/.local/share/xonotic-touch/` is denied (`mknod` / Permission denied) and aborts launch if the instance lock is treated as fatal — see [issue #19](https://github.com/dixonSolutions/Xonotic-Touch/issues/19) |
| `/bin/sh` (dash) runs the desktop hook's `Exec=bin/start.sh` | The launcher must be POSIX until it re-execs into bash |
| `$0` is relative; `APP_DIR` points at the install root | Resolve the app root from both, never from `pwd`/`dirname` |
| `APP_ID` is `<pkgname>_<appname>_<version>` | Derive `APP_PKGNAME` as `${APP_ID%%_*}` for the writable data dir |
| The engine's default userdir `~/.xonotic` is not writable | Click launches pass `-userdir $USER_BASE/userdir` (section 2) |
| Most phones (Halium) drive the GPU through libhybris: EGL with GLES, no desktop GL | The click engine is built with `USE_GLES2` (`XONOTIC_DP_GLES2=1` in `scripts/clickable-build.sh`). Its texture table takes DXT for GPUs with S3TC (Mesa ones): the full data's DDS textures otherwise stopped it at init |

## 2. Launcher contract (`packaging/start.sh`)

1. **Builtins only, until the tool bootstrap.** App-root resolution uses `cd` +
   `$PWD` and `${var%/*}` — no `dirname`, `readlink`, or `pwd`.
2. **App root is validated, not assumed.** Candidates are
   `$XONOTIC_TOUCH_APP_ROOT`, `${0%/*}/..`, then `$APP_DIR`; the first one that
   actually contains `bin/xonotic` wins. Otherwise we log both inputs and exit.
3. **User data uses the AppArmor-writable path.** With `APP_ID` set (click),
   `USER_BASE` is `$XDG_DATA_HOME/${APP_ID%%_*}` — not
   `~/.local/share/xonotic-touch`. Flatpak still uses
   `$XDG_DATA_HOME/xonotic-touch`. Override with `XONOTIC_TOUCH_USER_BASE`.
   The engine follows it: a click launch passes `-userdir $USER_BASE/userdir`,
   laid out like `~/.xonotic`. Left at `~/.xonotic`, the session lock failed and
   the engine quit straight after reading its configs — the "does not start at
   all" of every review. `-nohome` is no substitute: without a userdir
   DarkPlaces writes QC files one `data/` level deeper, and the wizard's
   relaunch marker never reached the launcher. The launcher's own references to
   the engine userdir go through `ENGINE_HOME`.
4. **Bash is optional.** The asset helpers need bash (arrays, `compgen`, process
   substitution), so the launcher probes bash and re-execs into it. If bash is
   not exec'able it keeps running under `/bin/sh` and uses
   `fetch-assets-posix.sh` (busybox wget/unzip) for first-launch downloads.
   Anything that *needs* bash must therefore decline the job rather than try and
   fail. `ensure_fetchd` is the one that got this wrong: it wrote a placeholder
   `discover` progress file, launched its bash daemon into a denied exec, and
   still returned success. `fetch_progress_is_live` then read that fresh
   placeholder as a download already in flight and skipped starting the POSIX
   fallback, so nothing downloaded at all. It now returns non-zero when there is
   no bash, waits for the daemon's pidfile instead of assuming a 0.2 s sleep was
   enough, and removes the placeholder if the daemon never came up. The click
   package ships no fetchd, so this never reached a phone — but a Flatpak or
   desktop install, which does ship one, could hit it on a slow start.
5. **Host tools are probed, not trusted.** `mkdir`/`grep`/`sed`/`awk`/`tar` are
   exercised once. If they work (desktop, Flatpak) the bundled `bin/` is appended
   to `PATH` so GNU behaviour is preserved; if they are denied it is prepended so
   the busybox applets take over. `flock` is only used when it actually acquires
   a lock (exit 1 = already running); denied exec must not abort launch. That
   holds for every `flock` in the file, the download lock included
   (`fd9_lock_is_held`): confined, flock exits 126, and reading that as "held"
   quit every download before it started.
6. **Only the engine exec is fatal.** Bundle sync, screen probing, config
   writes, and the instance lock log and continue, and empty screen values fall
   back to defaults, so a partially confined device still reaches the menu.
7. **Never redirect on a bare `exec`.** `exec 7>&- 2>/dev/null` closes fd 7 *and*
   sends stderr to /dev/null for the rest of the script. That line hid the
   engine's fatal error from the journal on every confined launch.

Environment overrides: `XONOTIC_TOUCH_APP_ROOT`, `XONOTIC_TOUCH_USER_BASE`,
`XONOTIC_TOUCH_NO_BASH=1` (stay on POSIX sh), `XONOTIC_SKIP_ASSET_FETCH=1`.

## 3. Bundled utilities (`scripts/stage-click-utils.sh`)

- Stages a **target-arch** `bin/busybox` plus applet symlinks (`awk`, `basename`,
  `cat`, `cp`, `dirname`, `grep`, `install`, `ln`, `mkdir`, `mv`, `rm`, `sed`,
  `sort`, `ssl_client`, `tar`, `tr`, `unzip`, `wget`, ...).
- Source order: host busybox when its ELF arch matches the target (native
  builds), otherwise `apt-get download busybox-static:<arch>` + `dpkg-deb -x`.
- Stages a target-arch `bin/openssl`. busybox `wget` hands each `https://`
  connection to `openssl s_client` and falls back to its own TLS only when that
  cannot run. Confined, `/usr/bin/openssl` cannot, and the asset servers (and
  GitHub) refuse busybox 1.36's TLS (`alert code 47`, `bad MAC`), so no download
  ever started. The bundled one runs from the click tree and links the phone's
  `libssl3`; no libraries are copied with it. Override a missing one with
  `XONOTIC_ALLOW_MISSING_OPENSSL=1`.
- Stages a target-arch `bin/curl` for the asset download, without libraries: it
  links the phone's `libcurl4t64`. From the autobuild server it is about 2.4 times
  faster than busybox `wget` through `openssl` (432 against 183 KB/s, measured
  back to back), and it checks certificates. `fetch-assets-posix.sh` uses it only
  if `curl --version` runs, so a phone without libcurl still downloads, through
  `wget`. No libraries are copied with any tool: a native build's curl copied with
  its libraries once brought libc into the amd64 test click. `stage-click.sh`
  likewise skips `/usr/local` libraries, the build image's own (an sdl2-compat
  without its SDL3).
- Branches other than `main` also build an **amd64** click (never published), so
  a click can be play-tested at native speed under its real profile on a PC.
- Staging failure is a build failure. Override with
  `XONOTIC_ALLOW_MISSING_BUSYBOX=1` only to produce a knowingly broken package.

## 4. Busybox differences to watch for

busybox applets are not GNU coreutils. Known constraints already handled:

| Tool | Constraint | Workaround in tree |
|------|-----------|--------------------|
| `tar` | no `--exclude` | `scripts/sync-bundle-data.sh` copies per top-level entry |
| `wget` | no `--user` | credentials in the URL (`scripts/lib/asset-fetch.sh`) |
| `cp` | `cp -a src/. dst/` merges (verified) | used for bundle sync |
| `sh` | `busybox sh` runs its own applets whatever `PATH` says | tests run the scripts under `/bin/sh`, as the phone does |

Every command the launch scripts run must be an applet in `BUSYBOX_APPLETS`:
the host's is denied. `wc` was missing, so `file_size` failed on a phone and
with it download resume and the MB progress. `file_size` now uses `stat -c %s`:
busybox `wc -c` reads the whole file, and the progress loop asks every second.

`fetch-assets-posix.sh` is plain POSIX sh, where a function has no local
variables. `progress_write` once named its temp file `tmp`, the download dir's
name too, and every download after the first went into
`asset-progress.txt.tmp.<pid>/`. Function variables there take a `_<fn>_`
prefix. The core zip (`Xonotic-latest.zip`) carries the data, maps, music and
compat packs, so it is fetched and unpacked first, and the maps and music zips
only for a pack it lacked: 1.2 GB on a phone, not 2.7 GB. busybox `wget` resumes
with `-c` and skips a zip that is already complete.

Build-time scripts (`stage-slim-data.sh`, `stage-click.sh`) run on the CI host
with GNU tools, so GNU-only flags are fine there — the restriction applies to
anything staged into the package.

## 5. Testing

```bash
./scripts/test-confined-launch.sh
```

Builds a fake click tree (stub engine, bundled busybox, slim data), then launches
it exactly like the desktop hook does: relative `Exec=bin/start.sh` with
`env -i` and `PATH=/nonexistent`, so no host binary is reachable. It runs twice —
with bash available and with `XONOTIC_TOUCH_NO_BASH=1` — and asserts that the
engine starts, receives numeric `vid_*` values, gets the bundled data synced into
the user data dir, and has the touch profile wired into `touch/startup.cfg`.

Those launch checks all pass `XONOTIC_SKIP_ASSET_FETCH=1`, so three further
checks cover the downloader itself. Two assert that with no bash,
`fetch-assets-posix.sh` is actually invoked — once with no fetchd packaged (the
click layout) and once with one present (the Flatpak/desktop layout), because
only the second reproduces the `ensure_fetchd` false-success above.

The third covers the handoff. Stopping the in-sandbox download so fetchd can
take `touch/fetch.lock` is a promise that something will pick it up, and when
the daemon does not come up the promise is broken twice over: the download is
gone, and the progress file still reads `discover`/`running`, which
`fetch_progress_is_live` takes for a job in flight — so the next launch inside
90 s joins a phantom rather than resuming the partial. `handoff_inflight_fetch`
now waits for the lock to actually come free instead of sleeping a fixed 0.5 s
(the outgoing job runs a TERM trap before its fd closes, and fetchd does not
retry), and `settle_after_handoff` writes `paused` when nothing took over.

Both halves of that check are load-bearing, and neither is obvious. Its stub
engine lingers a second and its stub download keeps rewriting progress, because
stopping a process tree only gets hard once the root has a live child and a
surviving job only gives itself away by overwriting `paused` afterwards. With an
instant engine and a download that reports once, the check passes over a handoff
that killed nothing at all — which is how `kill_process_tree` came to send its
last TERM to the deepest child (a global clobbered by its own recursion) and go
unnoticed.

`PATH=/nonexistent` stands in for AppArmor, and a stub stands in for the engine,
so neither the real profile nor GL is exercised. Four bugs got past it until the
release click was run under its real profile: the engine userdir, the silenced
stderr, the download lock, and TLS. It now checks for each of them.

To run a release (or CI artifact) click under its real profile without a phone:

1. Register arm64 emulation on the host: `docker run --privileged --rm
   tonistiigi/binfmt --install arm64` (lasts until reboot).
2. Make an arm64 Ubuntu 24.04 container with the UBports repo (key and list from
   `clickable/ci-ut24.04-1.x-amd64`), `click`, `click-apparmor`,
   `apparmor-easyprof-ubuntu`, `mir-demos`, `mir-platform-graphics-virtual`,
   `mir-platform-rendering-egl-generic`, `mir-platform-input-evdev10`,
   `dmz-cursor-theme`, `grim`, `libsdl2-2.0-0`, `libjpeg8`, Mesa (`libegl-mesa0`,
   `libgl1-mesa-dri`), and `/usr/share/click/frameworks/ubuntu-touch-24.04-1.x.framework`.
3. In a `--privileged` container with `/sys/kernel/security` mounted,
   `click install --user=phablet --allow-unauthenticated <click>`. The phone's
   AppArmor hook writes the profile and loads it into the (host) kernel.
4. Start `miral-shell --platform-display-libs mir:virtual --virtual-output
   2340x1080` as `phablet`, then launch from the click dir as UT does:
   `aa-exec-click -p <pkg>_<app>_<version> -- /bin/sh -c 'exec bin/start.sh'`
   with `WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR` and `APP_DIR` set. `grim` takes
   screenshots; `journalctl -k | grep DENIED` shows the denials.

Two things differ from a phone. An unprivileged `aa-exec` on a recent desktop
kernel is turned into a profile stack (`unconfined//&<profile>`), which denies
unix-socket traffic a phone allows; launch from a privileged container to get the
plain label when a socket denial looks suspect. And Mesa offers desktop GL as well
as GLES, so a container cannot show a desktop-GL engine failing on a Halium GPU.

On-device verification after installing a `.click`:

```bash
# On the phone
journalctl -f | grep -i xonotic
ls /opt/click.ubuntu.com/xonotictouch.dixonsolutions/current/bin
```

Expect `using bundled busybox utilities (host binaries are confined)` in the log
and no `Permission denied` lines.
