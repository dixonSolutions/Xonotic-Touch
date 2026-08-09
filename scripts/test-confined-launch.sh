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

stage_fake_click() {
    rm -rf "$WORK"
    mkdir -p "$APP_ROOT/bin" "$APP_ROOT/lib" "$APP_ROOT/share/xonotic" \
        "$APP_ROOT/data/xonotic-data.pk3dir/gfx" "$WORK/home"

    # Stub engine: records argv instead of opening a window.
    cat > "$APP_ROOT/bin/xonotic" <<EOF
#!/bin/sh
printf '%s\n' "\$@" > "$ENGINE_LOG"
exit 0
EOF
    chmod 755 "$APP_ROOT/bin/xonotic"

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
    output="$(
        rm -f "$ENGINE_LOG"
        ( cd "$APP_ROOT" && env -i \
            HOME="$WORK/home" \
            PATH=/nonexistent \
            XONOTIC_SKIP_ASSET_FETCH=1 \
            "$@" \
            /bin/sh -c 'exec bin/start.sh' ) 2>&1
    )" || status=$?

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

stage_fake_click
expect_launch 'launches confined (bash available)' "$USER_BASE" \
    XONOTIC_TOUCH_USER_BASE="$USER_BASE"
expect_launch 'launches confined (POSIX sh only)' "$USER_BASE" \
    XONOTIC_TOUCH_USER_BASE="$USER_BASE" XONOTIC_TOUCH_NO_BASH=1

# issue #19: APP_ID selects the AppArmor-writable APP_PKGNAME data dir.
rm -rf "$CLICK_USER_BASE" "$USER_BASE"
expect_launch 'launches confined (APP_ID writable path)' "$CLICK_USER_BASE" \
    APP_ID=xonotictouch.dixonsolutions_xonotic_1.2.42 \
    XDG_DATA_HOME="$WORK/home/.local/share" \
    XONOTIC_TOUCH_NO_BASH=1 \
    UBUNTU_APPLICATION_ISOLATION=1

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

if [ "$FAILURES" -ne 0 ]; then
    printf '%d confined-launch check(s) failed\n' "$FAILURES" >&2
    exit 1
fi

printf 'All confined-launch checks passed.\n'
