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
4. **Bash is optional.** The asset helpers need bash (arrays, `compgen`, process
   substitution), so the launcher probes bash and re-execs into it. If bash is
   not exec'able it keeps running under `/bin/sh` and uses
   `fetch-assets-posix.sh` (busybox wget/unzip) for first-launch downloads.
5. **Host tools are probed, not trusted.** `mkdir`/`grep`/`sed`/`awk`/`tar` are
   exercised once. If they work (desktop, Flatpak) the bundled `bin/` is appended
   to `PATH` so GNU behaviour is preserved; if they are denied it is prepended so
   the busybox applets take over. `flock` is only used when it actually acquires
   a lock (exit 1 = already running); denied exec must not abort launch.
6. **Only the engine exec is fatal.** Bundle sync, screen probing, config
   writes, and the instance lock log and continue, and empty screen values fall
   back to defaults, so a partially confined device still reaches the menu.

Environment overrides: `XONOTIC_TOUCH_APP_ROOT`, `XONOTIC_TOUCH_USER_BASE`,
`XONOTIC_TOUCH_NO_BASH=1` (stay on POSIX sh), `XONOTIC_SKIP_ASSET_FETCH=1`.

## 3. Bundled utilities (`scripts/stage-click-utils.sh`)

- Stages a **target-arch** `bin/busybox` plus applet symlinks (`awk`, `basename`,
  `cat`, `cp`, `dirname`, `grep`, `install`, `ln`, `mkdir`, `mv`, `rm`, `sed`,
  `sort`, `ssl_client`, `tar`, `tr`, `unzip`, `wget`, ...).
- Source order: host busybox when its ELF arch matches the target (native
  builds), otherwise `apt-get download busybox-static:<arch>` + `dpkg-deb -x`.
- `curl`/`unzip` are only bundled when the host binary matches the target arch.
  Cross builds previously shipped **amd64** helpers inside arm64/armhf clicks;
  those were unusable on device, so we now fall back to busybox `wget`
  (credentials move into the URL userinfo) and busybox `unzip`.
- Staging failure is a build failure. Override with
  `XONOTIC_ALLOW_MISSING_BUSYBOX=1` only to produce a knowingly broken package.

## 4. Busybox differences to watch for

busybox applets are not GNU coreutils. Known constraints already handled:

| Tool | Constraint | Workaround in tree |
|------|-----------|--------------------|
| `tar` | no `--exclude` | `scripts/sync-bundle-data.sh` copies per top-level entry |
| `wget` | no `--user` | credentials in the URL (`scripts/lib/asset-fetch.sh`) |
| `cp` | `cp -a src/. dst/` merges (verified) | used for bundle sync |

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

On-device verification after installing a `.click`:

```bash
# On the phone
journalctl -f | grep -i xonotic
ls /opt/click.ubuntu.com/xonotictouch.dixonsolutions/current/bin
```

Expect `using bundled busybox utilities (host binaries are confined)` in the log
and no `Permission denied` lines.
