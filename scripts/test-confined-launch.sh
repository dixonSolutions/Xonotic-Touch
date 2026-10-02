#!/bin/bash
# Regression test for the Ubuntu Touch launch path (issues #18 and #19).
#
# Simulates click confinement: the launcher is started as a relative path with
# an unusable PATH, so every host binary (dirname, mkdir, tar, sed, ...) is out
# of reach and only the bundled busybox applets remain.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
WORK="${WORK:-$ROOT/build/test-confined}"
APP_ROOT="$WORK/app"
USER_BASE="$WORK/home/.local/share/xonotic-touch"
CLICK_USER_BASE="$WORK/home/.local/share/xonotictouch.dixonsolutions"
ENGINE_LOG="$WORK/engine-args.txt"

FAILURES=0

fail() {
    printf 'FAIL: %s\n' "$1" >&2
    FAILURES=$((FAILURES + 1))
}

pass() {
    printf 'ok: %s\n' "$1"
}

# Stub engine: records argv instead of opening a window, and says so on stderr
# the way the real one reports a fatal error.
stage_engine_stub() {
    cat > "$APP_ROOT/bin/xonotic" <<EOF
#!/bin/sh
printf '%s\n' "\$@" > "$ENGINE_LOG"
echo 'xonotic-stub: engine stderr' >&2
exit 0
EOF
    chmod 755 "$APP_ROOT/bin/xonotic"
}

stage_fake_click() {
    rm -rf "$WORK"
    mkdir -p "$APP_ROOT/bin" "$APP_ROOT/lib" "$APP_ROOT/share/xonotic" \
        "$APP_ROOT/data/xonotic-data.pk3dir/gfx" "$WORK/home"

    stage_engine_stub

    install -m 755 "$ROOT/packaging/start.sh" "$APP_ROOT/bin/start.sh"
    install -m 755 "$ROOT/touch/screen-calc.sh" "$APP_ROOT/share/xonotic/screen-calc.sh"
    install -m 755 "$ROOT/scripts/sync-bundle-data.sh" "$APP_ROOT/share/xonotic/sync-bundle-data.sh"
    install -m 755 "$ROOT/scripts/fetch-assets-runtime.sh" "$APP_ROOT/share/xonotic/fetch-assets-runtime.sh"
    install -m 755 "$ROOT/scripts/fetch-assets-posix.sh" "$APP_ROOT/share/xonotic/fetch-assets-posix.sh"
    install -m 644 "$ROOT/scripts/lib/asset-fetch.sh" "$APP_ROOT/share/xonotic/asset-fetch.sh"
    install -m 644 "$ROOT/scripts/lib/asset-discover.sh" "$APP_ROOT/share/xonotic/asset-discover.sh"
    printf 'test\n' > "$APP_ROOT/data/xonotic-data.pk3dir/gfx/bundled.txt"
    mkdir -p "$APP_ROOT/data/touch/profiles"
    printf '// standard\n' > "$APP_ROOT/data/touch/profiles/standard.cfg"
    printf '// thermal\n' > "$APP_ROOT/data/touch/profiles/thermal.cfg"

    CLICK_ARCH="" bash "$ROOT/scripts/stage-click-utils.sh" "$APP_ROOT" >/dev/null
    if [ ! -x "$APP_ROOT/bin/busybox" ]; then
        printf 'cannot run test: no busybox staged for this host\n' >&2
        exit 77
    fi
}

# Launch exactly like the click desktop hook: relative Exec, no usable PATH.
expect_launch() {
    local label="$1"
    local expect_base="$2"
    shift 2

    local output status=0
    LAST_OUTPUT=""
    output="$(
        rm -f "$ENGINE_LOG"
        ( cd "$APP_ROOT" && env -i \
            HOME="$WORK/home" \
            PATH=/nonexistent \
            XONOTIC_SKIP_ASSET_FETCH=1 \
            "$@" \
            /bin/sh -c 'exec bin/start.sh' ) 2>&1
    )" || status=$?
    LAST_OUTPUT="$output"

    if [ "$status" -ne 0 ]; then
        fail "$label: launcher exited with status $status"
        printf '%s\n' "$output" >&2
        return
    fi
    if [ ! -f "$ENGINE_LOG" ]; then
        fail "$label: engine was never started"
        printf '%s\n' "$output" >&2
        return
    fi
    if grep -q 'Permission denied' <<<"$output"; then
        fail "$label: launcher hit a denied host binary"
        printf '%s\n' "$output" >&2
        return
    fi
    if grep -qE '^$' "$ENGINE_LOG"; then
        fail "$label: engine received an empty argument"
        return
    fi
    if ! grep -A1 -x '+vid_width' "$ENGINE_LOG" | grep -qxE '[0-9]+'; then
        fail "$label: engine did not receive a numeric vid_width"
        return
    fi
    if [ ! -f "$expect_base/data/xonotic-data.pk3dir/gfx/bundled.txt" ]; then
        fail "$label: bundled data was not synced into $expect_base/data"
        return
    fi
    if ! grep -q 'exec touch/profiles/standard.cfg' "$expect_base/data/touch/startup.cfg"; then
        fail "$label: touch profile was not wired into startup.cfg"
        return
    fi
    pass "$label"
}

# Asset fetching, which expect_launch deliberately skips.
#
# With no bash the POSIX downloader is the only fetcher a confined click has, so
# "was it actually started" is the whole question -- and nothing here used to ask
# it. `stage_fetchd` decides whether the fetchd daemon is present: the click
# package ships none, but a Flatpak/desktop install does, and it was that layout
# where ensure_fetchd wrote a placeholder progress file, failed to exec its bash
# daemon, reported success anyway, and left the fresh placeholder reading as a
# live download -- which suppressed the fallback entirely.
expect_posix_fetch() {
    local label="$1"
    local stage_fetchd="$2"
    local path="${3:-/nonexistent}"
    local fetch_log="$WORK/posix-fetch-invoked.txt"

    rm -f "$fetch_log"
    rm -rf "$CLICK_USER_BASE"

    if [ "$stage_fetchd" = 'with-fetchd' ]; then
        install -m 755 "$ROOT/scripts/xonotic-touch-fetchd.sh" \
            "$APP_ROOT/share/xonotic/xonotic-touch-fetchd.sh"
    else
        rm -f "$APP_ROOT/share/xonotic/xonotic-touch-fetchd.sh"
    fi

    # Recorder in place of the real downloader: this test is about whether the
    # launcher reaches it, not about downloading 1.16 GB.
    cat > "$APP_ROOT/share/xonotic/fetch-assets-posix.sh" <<EOF
#!/bin/sh
printf 'invoked %s\n' "\$1" >> "$fetch_log"
exit 0
EOF
    chmod 755 "$APP_ROOT/share/xonotic/fetch-assets-posix.sh"

    ( cd "$APP_ROOT" && env -i \
        HOME="$WORK/home" \
        PATH="$path" \
        APP_ID=xonotictouch.dixonsolutions_xonotic_1.2.49 \
        XDG_DATA_HOME="$WORK/home/.local/share" \
        UBUNTU_APPLICATION_ISOLATION=1 \
        XONOTIC_TOUCH_NO_BASH=1 \
        /bin/sh -c 'exec bin/start.sh' ) >/dev/null 2>&1 || true

    # The fetcher runs in a background subshell; give it a moment to land.
    local i=0
    while [ "$i" -lt 30 ] && [ ! -f "$fetch_log" ]; do
        sleep 0.1
        i=$((i + 1))
    done

    if [ -f "$fetch_log" ]; then
        pass "$label"
    else
        fail "$label: fetch-assets-posix.sh was never invoked"
    fi

    install -m 755 "$ROOT/scripts/fetch-assets-posix.sh" \
        "$APP_ROOT/share/xonotic/fetch-assets-posix.sh"
    rm -f "$APP_ROOT/share/xonotic/xonotic-touch-fetchd.sh"
}

stage_fake_click
expect_launch 'launches confined (bash available)' "$USER_BASE" \
    XONOTIC_TOUCH_USER_BASE="$USER_BASE"
if grep -qx -- '-userdir' "$ENGINE_LOG" 2>/dev/null; then
    fail 'desktop launch passed -userdir (its engine userdir is ~/.xonotic)'
else
    pass 'desktop launch keeps the engine userdir'
fi
expect_launch 'launches confined (POSIX sh only)' "$USER_BASE" \
    XONOTIC_TOUCH_USER_BASE="$USER_BASE" XONOTIC_TOUCH_NO_BASH=1

# issue #19: APP_ID selects the AppArmor-writable APP_PKGNAME data dir.
# The desktop launches above made ~/.xonotic in the same HOME.
rm -rf "$CLICK_USER_BASE" "$USER_BASE" "$WORK/home/.xonotic"
expect_launch 'launches confined (APP_ID writable path)' "$CLICK_USER_BASE" \
    APP_ID=xonotictouch.dixonsolutions_xonotic_1.2.42 \
    XDG_DATA_HOME="$WORK/home/.local/share" \
    XONOTIC_TOUCH_NO_BASH=1 \
    UBUNTU_APPLICATION_ISOLATION=1

# AppArmor lets a click create files only under XDG_*/<APP_PKGNAME>. Left at
# ~/.xonotic, the engine could not take its session lock and quit right after
# reading its configs; its userdir has to live in the click's own data dir.
if [ "$(grep -A1 -x -- '-userdir' "$ENGINE_LOG" 2>/dev/null | tail -n 1)" = "$CLICK_USER_BASE/userdir" ]; then
    pass 'click launch keeps the engine out of ~/.xonotic'
else
    fail 'click launch did not pass -userdir under its data dir (engine would write ~/.xonotic)'
fi
if [ -e "$WORK/home/.xonotic" ]; then
    fail 'click launch created ~/.xonotic'
else
    pass 'click launch did not create ~/.xonotic'
fi

# The menu saves the touch layout with fopen, which lands in
# <userdir>/data/data/; only `exec data/touch.layout.cfg` finds it there.
rm -rf "$CLICK_USER_BASE"
mkdir -p "$CLICK_USER_BASE/userdir/data/data"
printf 'seta touch_setup_done "1"\n' > "$CLICK_USER_BASE/userdir/data/data/touch.layout.cfg"
expect_launch 'launches with a saved touch layout' "$CLICK_USER_BASE" \
    APP_ID=xonotictouch.dixonsolutions_xonotic_1.2.42 \
    XDG_DATA_HOME="$WORK/home/.local/share" \
    XONOTIC_TOUCH_NO_BASH=1 \
    UBUNTU_APPLICATION_ISOLATION=1
if grep -qx 'exec data/touch.layout.cfg' "$CLICK_USER_BASE/data/touch/startup.cfg"; then
    pass 'saved touch layout is exec'"'"'d from where the menu wrote it'
else
    fail 'saved touch layout is not exec'"'"'d as data/touch.layout.cfg (setup reopens every launch)'
fi

if [ -d "$USER_BASE" ]; then
    fail 'APP_ID launch wrote to legacy ~/.local/share/xonotic-touch'
else
    pass 'APP_ID launch did not use legacy xonotic-touch path'
fi

# issue #19: flock on PATH but unusable must not abort or pretend "already running".
rm -rf "$CLICK_USER_BASE"
mkdir -p "$WORK/fake-bin"
cat > "$WORK/fake-bin/flock" <<'EOF'
#!/bin/sh
exit 126
EOF
chmod 755 "$WORK/fake-bin/flock"
expect_launch 'launches when flock exec is denied' "$CLICK_USER_BASE" \
    APP_ID=xonotictouch.dixonsolutions_xonotic_1.2.42 \
    XDG_DATA_HOME="$WORK/home/.local/share" \
    PATH="$WORK/fake-bin" \
    XONOTIC_TOUCH_NO_BASH=1

# Closing the lock fd as `exec 7>&- 2>/dev/null` sent every later line of
# stderr to /dev/null -- a bare exec keeps its redirections -- so the engine's
# fatal error never reached the phone's journal.
if grep -q 'xonotic-stub: engine stderr' <<<"$LAST_OUTPUT"; then
    pass 'engine stderr survives the flock fallback'
else
    fail 'engine stderr was discarded after the flock fallback'
fi

# No bash: the POSIX downloader must actually start, in both packaging layouts.
expect_posix_fetch 'POSIX downloader runs when no fetchd is packaged (click)' 'no-fetchd'
expect_posix_fetch 'POSIX downloader runs even when fetchd cannot start (bash absent)' 'with-fetchd'
# The download subshell took any flock failure for "another download holds the
# lock" and quit; under confinement flock exits 126, so none ever started.
expect_posix_fetch 'POSIX downloader runs when flock exec is denied' 'no-fetchd' "$WORK/fake-bin"

# The POSIX downloader itself, end to end, the way a click runs it: /bin/sh with
# the package's busybox applets, wget for the network (a fake serving a small
# zip laid out like the autobuild one), busybox unzip for the packs. Not
# `busybox sh`: that runs its own wget whatever PATH says.
#
# Its progress writer once reused the name of the download dir, so every
# download after the first went to "asset-progress.txt.tmp.<pid>/" and the fetch
# died with the wizard stuck on "Downloading...". It also fetched the maps and
# music zips up front, although the core zip already carries both.
expect_posix_fetch_completes() {
    local label="$1"
    local preload="$2"
    local data="$WORK/fetch-data"
    local fixture="$WORK/fixture/Xonotic-latest.zip"
    local wget_log="$WORK/fetch-wget.log"
    local progress="$data/touch/asset-progress.txt"

    rm -rf "$data" "$WORK/fetch-bin" "$wget_log"
    mkdir -p "$data/touch" "$WORK/fetch-bin" "$WORK/fixture"
    # The package's tools minus curl and wget: a cross-built click has no curl
    # (only a native one bundles the host's), and wget is faked below.
    local tool
    for tool in "$APP_ROOT"/bin/*; do
        case "${tool##*/}" in
            curl|wget) ;;
            *) ln -s "$tool" "$WORK/fetch-bin/${tool##*/}" ;;
        esac
    done
    if [ ! -f "$fixture" ]; then
        python3 - "$fixture" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w") as z:
    for pack in ("data", "maps", "music", "nexcompat"):
        z.writestr(f"Xonotic/data/xonotic-20260101-{pack}.pk3", pack * 64)
    z.writestr("Xonotic/Docs/readme.txt", "not a pack")
PY
    fi
    if [ "$preload" = 'complete-core-zip' ]; then
        mkdir -p "$data/.fetch-tmp"
        cp "$fixture" "$data/.fetch-tmp/xonotic.zip"
    fi

    # Fake wget: answers --spider with the fixture's length, serves the core zip,
    # 404s everything else, and logs each real download.
    cat > "$WORK/fetch-bin/wget" <<WGET
#!/bin/sh
out=""; url=""; spider=0
while [ \$# -gt 0 ]; do
    case "\$1" in
        -O) out="\$2"; shift ;;
        --spider) spider=1 ;;
        -*) ;;
        *) url="\$1" ;;
    esac
    shift
done
case "\$url" in
    */Xonotic-latest.zip) ;;
    *) echo "wget: server returned error: HTTP/1.1 404 Not Found" >&2; exit 1 ;;
esac
if [ "\$spider" = 1 ]; then
    echo "  Content-Length: \$(wc -c < "$fixture")" >&2
    exit 0
fi
echo "\${url##*/}" >> "$wget_log"
cat "$fixture" > "\$out"
WGET
    chmod 755 "$WORK/fetch-bin/wget"

    local status=0
    ( env -i HOME="$WORK/home" PATH="$WORK/fetch-bin" \
        XONOTIC_ASSET_FETCH_PROGRESS="$progress" \
        /bin/sh "$ROOT/scripts/fetch-assets-posix.sh" "$data" ) \
        >"$WORK/fetch.out" 2>&1 || status=$?

    if [ "$status" -ne 0 ]; then
        fail "$label: fetch-assets-posix.sh exited $status"
        cat "$WORK/fetch.out" >&2
        return
    fi
    if [ "$(head -n 1 "$progress" 2>/dev/null)" != 'done' ]; then
        fail "$label: progress did not reach done ($(tr '\n' '|' < "$progress" 2>/dev/null))"
        return
    fi
    local pack
    for pack in data maps music nexcompat; do
        if [ ! -f "$data/xonotic-20260101-$pack.pk3" ]; then
            fail "$label: $pack pack was not installed"
            return
        fi
    done
    if [ -e "$data/.fetch-tmp" ]; then
        fail "$label: download dir was left behind"
        return
    fi
    if [ "$preload" = 'complete-core-zip' ]; then
        if [ -s "$wget_log" ]; then
            fail "$label: fetched again a zip that was already complete ($(tr '\n' ' ' < "$wget_log"))"
            return
        fi
    elif [ "$(cat "$wget_log" 2>/dev/null)" != 'Xonotic-latest.zip' ]; then
        fail "$label: expected one download of Xonotic-latest.zip, got: $(tr '\n' ' ' < "$wget_log" 2>/dev/null)"
        return
    fi
    pass "$label"
}

expect_posix_fetch_completes 'POSIX downloader installs the packs from the core zip alone' 'none'
expect_posix_fetch_completes 'POSIX downloader does not fetch a complete zip again' 'complete-core-zip'

# After the first download the wizard writes touch/relaunch-request.txt and
# quits, and the launcher has to start the engine again to load the new packs.
# QC's FILE_WRITE puts that file under data/ in the engine's write directory,
# <userdir>/data/data/touch/, which the launcher never looked at: the session
# just ended. This stub writes the marker exactly there on its first run.
expect_relaunch_after_wizard() {
    local label="$1"
    shift
    local runs="$WORK/engine-runs.txt"

    rm -f "$runs"
    cat > "$APP_ROOT/bin/xonotic" <<EOF
#!/bin/sh
echo run >> "$runs"
userdir="\$HOME/.xonotic"
while [ \$# -gt 0 ]; do
    if [ "\$1" = -userdir ]; then userdir="\$2"; fi
    shift
done
if [ "\$(wc -l < "$runs")" -eq 1 ]; then
    mkdir -p "\$userdir/data/data/touch"
    : > "\$userdir/data/data/touch/relaunch-request.txt"
fi
exit 0
EOF
    chmod 755 "$APP_ROOT/bin/xonotic"

    ( cd "$APP_ROOT" && env -i \
        HOME="$WORK/home" \
        PATH=/nonexistent \
        XONOTIC_SKIP_ASSET_FETCH=1 \
        "$@" \
        /bin/sh -c 'exec bin/start.sh' ) >/dev/null 2>&1 || true

    local count=0
    [ -f "$runs" ] && count="$(wc -l < "$runs")"
    if [ "$count" -eq 2 ]; then
        pass "$label"
    else
        fail "$label: engine ran $count time(s), expected 2"
    fi
    stage_engine_stub
}

rm -rf "$CLICK_USER_BASE" "$WORK/home/.xonotic"
expect_relaunch_after_wizard 'click relaunches the engine when the wizard asks' \
    APP_ID=xonotictouch.dixonsolutions_xonotic_1.2.42 \
    XDG_DATA_HOME="$WORK/home/.local/share" \
    XONOTIC_TOUCH_NO_BASH=1
rm -rf "$WORK/home/.xonotic"
expect_relaunch_after_wizard 'desktop relaunches the engine when the wizard asks' \
    XONOTIC_TOUCH_USER_BASE="$USER_BASE"

# Handing an in-flight download to fetchd stops the in-sandbox job first, on the
# promise that the daemon picks it up. When it does not, the progress file must
# not be left on a fresh discover/running line: the next launch reads that as a
# job already running and joins a phantom instead of resuming the partial.
expect_paused_after_failed_handoff() {
    local label='failed fetchd handoff pauses instead of faking a live download'
    local progress="$CLICK_USER_BASE/data/touch/asset-progress.txt"

    rm -rf "$CLICK_USER_BASE"

    # A "download" that holds touch/fetch.lock and keeps reporting progress, the
    # way a real one does. Reporting once would let a job that survived the
    # handoff pass this check: `paused` would still be the last line written.
    cat > "$APP_ROOT/share/xonotic/asset-fetch.sh" <<'EOF'
xonotic_assets_are_ready() { return 1; }
xonotic_fetch_game_assets() {
  _i=0
  while [ "$_i" -lt 150 ]; do
    printf 'running\n42\nDownloading...\n' > "$XONOTIC_ASSET_FETCH_PROGRESS"
    sleep 0.2 || true
    _i=$((_i + 1))
  done
}
EOF
    # A fetchd that refuses to come up: exits without writing a pidfile.
    cat > "$APP_ROOT/share/xonotic/xonotic-touch-fetchd.sh" <<'EOF'
#!/bin/bash
exit 1
EOF
    chmod 755 "$APP_ROOT/share/xonotic/xonotic-touch-fetchd.sh"

    # An engine that lingers, so the window closes with the fetch job settled
    # into its loop. Stopping a process tree is only hard once the root has a
    # live child; an engine that returns instantly usually closes the window
    # before the job has one, and the handoff looks fine either way.
    cat > "$APP_ROOT/bin/xonotic" <<'EOF'
#!/bin/sh
sleep 1
exit 0
EOF
    chmod 755 "$APP_ROOT/bin/xonotic"

    # Real PATH here: this case needs bash, or the handoff is declined outright.
    ( cd "$APP_ROOT" && env -i \
        HOME="$WORK/home" \
        PATH=/usr/bin:/bin \
        APP_ID=xonotictouch.dixonsolutions_xonotic_1.2.49 \
        XDG_DATA_HOME="$WORK/home/.local/share" \
        UBUNTU_APPLICATION_ISOLATION=1 \
        /bin/sh -c 'exec bin/start.sh' ) >/dev/null 2>&1 || true

    # Long enough for a job that outlived the handoff to overwrite `paused`.
    sleep 0.5

    local status
    status="$(head -n 1 "$progress" 2>/dev/null || echo MISSING)"
    if [ "$status" = 'paused' ]; then
        pass "$label"
    else
        fail "$label: progress left as '$status'"
    fi

    install -m 644 "$ROOT/scripts/lib/asset-fetch.sh" "$APP_ROOT/share/xonotic/asset-fetch.sh"
    rm -f "$APP_ROOT/share/xonotic/xonotic-touch-fetchd.sh"
    rm -rf "$CLICK_USER_BASE"
    stage_engine_stub
}

expect_paused_after_failed_handoff

if [ "$FAILURES" -ne 0 ]; then
    printf '%d confined-launch check(s) failed\n' "$FAILURES" >&2
    exit 1
fi

printf 'All confined-launch checks passed.\n'
