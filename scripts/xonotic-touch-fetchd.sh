#!/bin/bash
# Host-side asset fetch daemon. Survives Flatpak UI exit so "download in
# background" is meaningful. Holds touch/fetch.lock; resumes
# partial zips; notifies and optionally re-opens the app when packs are ready.
set -euo pipefail

USER_BASE="${XONOTIC_TOUCH_USER_BASE:?XONOTIC_TOUCH_USER_BASE required}"
USER_DATA="${USER_BASE}/data"
LIB_DIR="${XONOTIC_TOUCH_LIB_DIR:-${USER_BASE}/lib}"
PROGRESS_FILE="${XONOTIC_ASSET_FETCH_PROGRESS:-${USER_DATA}/touch/asset-progress.txt}"
PIDFILE="${USER_BASE}/fetchd.pid"
FLATPAK_ID="${XONOTIC_TOUCH_FLATPAK_ID:-io.github.dixonSolutions.XonoticTouch}"
AUTO_OPEN="${XONOTIC_TOUCH_AUTO_OPEN_ON_READY:-1}"
NOTIFY="${XONOTIC_TOUCH_NOTIFY_ON_READY:-1}"

mkdir -p "$USER_DATA/touch" "$USER_BASE"

# Exclusive download. A second fetchd exits quietly — the live job owns progress.
# Write the pidfile only after the lock is held so a losing instance cannot
# clobber / remove the winner's pid on its EXIT trap.
exec 9>"${USER_DATA}/touch/fetch.lock"
if command -v flock >/dev/null 2>&1; then
    flock -n 9 || {
        echo "xonotic-touch-fetchd: fetch already running" >&2
        exit 0
    }
fi

echo $$ > "$PIDFILE"
cleanup_pid() {
    if [ -f "$PIDFILE" ] && [ "$(cat "$PIDFILE" 2>/dev/null || true)" = "$$" ]; then
        rm -f "$PIDFILE" 2>/dev/null || true
    fi
}
trap cleanup_pid EXIT

export XONOTIC_ASSET_FETCH_PROGRESS="$PROGRESS_FILE"

if [ ! -f "${LIB_DIR}/asset-fetch.sh" ]; then
    echo "xonotic-touch-fetchd: missing ${LIB_DIR}/asset-fetch.sh" >&2
    exit 1
fi
# shellcheck source=/dev/null
. "${LIB_DIR}/asset-fetch.sh"
if [ -f "${LIB_DIR}/asset-discover.sh" ]; then
    # shellcheck source=/dev/null
    . "${LIB_DIR}/asset-discover.sh"
fi

if declare -F xonotic_assets_are_ready >/dev/null 2>&1 \
    && xonotic_assets_are_ready "$USER_DATA"; then
    echo "xonotic-touch-fetchd: assets already ready" >&2
    exit 0
fi

if declare -F xonotic_resolve_missing_assets >/dev/null 2>&1; then
    xonotic_resolve_missing_assets "$USER_DATA"
else
    xonotic_fetch_game_assets "$USER_DATA"
fi

if [ -x "${LIB_DIR}/sync-bundle-data.sh" ] && [ -d "${XONOTIC_TOUCH_BUNDLE_DATA:-}" ]; then
    "${LIB_DIR}/sync-bundle-data.sh" "$XONOTIC_TOUCH_BUNDLE_DATA" "$USER_DATA" || true
fi

notify_ready() {
    [ "$NOTIFY" = "1" ] || return 0
    _title="Xonotic Touch"
    _body="Game data finished downloading. Opening Xonotic Touch…"
    if command -v notify-send >/dev/null 2>&1; then
        notify-send -a "Xonotic Touch" -i "$FLATPAK_ID" "$_title" "$_body" || true
    elif command -v gdbus >/dev/null 2>&1; then
        gdbus call --session \
            --dest org.freedesktop.Notifications \
            --object-path /org/freedesktop/Notifications \
            --method org.freedesktop.Notifications.Notify \
            "Xonotic Touch" 0 "$FLATPAK_ID" "$_title" "$_body" \
            '[]' '{}' 8000 >/dev/null 2>&1 || true
    fi
}

engine_running() {
    pgrep -f '/bin/xonotic( |$)' >/dev/null 2>&1 \
        || pgrep -f 'xonotic-touch.*/bin/xonotic' >/dev/null 2>&1
}

open_app() {
    [ "$AUTO_OPEN" = "1" ] || return 0
    engine_running && return 0
    # Prefer Flatpak; fall back to a direct start.sh if packaged that way.
    if command -v flatpak >/dev/null 2>&1 && [ -n "$FLATPAK_ID" ]; then
        nohup flatpak run "$FLATPAK_ID" >/dev/null 2>&1 &
        return 0
    fi
    if [ -x "${XONOTIC_TOUCH_APP_ROOT:-}/bin/start.sh" ]; then
        nohup "${XONOTIC_TOUCH_APP_ROOT}/bin/start.sh" >/dev/null 2>&1 &
    fi
}

if [ -f "${USER_DATA}/.assets-ready" ] \
    || { declare -F xonotic_assets_are_ready >/dev/null 2>&1 \
        && xonotic_assets_are_ready "$USER_DATA"; }; then
    notify_ready
    open_app
fi
