#!/bin/sh
# Launch wrapper for Xonotic Touch (Flatpak, Click, local packages).
#
# On Ubuntu Touch this script runs inside AppArmor click confinement, which
# denies exec of host binaries outside the click tree (see
# docs/UBUNTU_TOUCH_LAUNCH.md). Everything above the "tool bootstrap" section
# below must therefore use shell builtins only — no dirname, no coreutils.
set -e

xonotic_log() {
    echo "xonotic-touch: $*" >&2
}

# `cd` + $PWD instead of `pwd`/`realpath`: builtins always work when confined.
xonotic_resolve_dir() {
    CDPATH='' cd -P -- "$1" 2>/dev/null && echo "$PWD"
}

xonotic_parent_dir() {
    case "$1" in
        */*) echo "${1%/*}" ;;
        *) echo "." ;;
    esac
}

# The desktop hook launches us as a relative path (Exec=bin/start.sh), so $0
# alone is not enough; APP_DIR is exported by lomiri-app-launch on the device.
APP_ROOT=""
for xonotic_root_candidate in \
    "${XONOTIC_TOUCH_APP_ROOT:-}" \
    "$(xonotic_parent_dir "$0")/.." \
    "${APP_DIR:-}"; do
    [ -n "$xonotic_root_candidate" ] || continue
    xonotic_root_resolved="$(xonotic_resolve_dir "$xonotic_root_candidate")" || continue
    if [ -n "$xonotic_root_resolved" ] && [ -x "$xonotic_root_resolved/bin/xonotic" ]; then
        APP_ROOT="$xonotic_root_resolved"
        break
    fi
done

if [ -z "$APP_ROOT" ]; then
    xonotic_log "engine binary not found (script=$0 APP_DIR=${APP_DIR:-unset})"
    exit 1
fi

export XONOTIC_TOUCH_APP_ROOT="$APP_ROOT"

# The asset helpers need bash (arrays, process substitution). Prefer it when the
# confinement allows exec'ing it, but keep running under /bin/sh otherwise.
if [ -z "${BASH_VERSION:-}" ] && [ "${XONOTIC_TOUCH_NO_BASH:-0}" != "1" ]; then
    for xonotic_bash in "$APP_ROOT/bin/bash" /bin/bash /usr/bin/bash; do
        [ -x "$xonotic_bash" ] || continue
        # Probe first: a failing `exec` would kill this shell.
        if "$xonotic_bash" -c ':' 2>/dev/null; then
            exec "$xonotic_bash" "$0" "$@"
        fi
    done
    xonotic_log "bash unavailable — asset discovery and download are disabled"
fi

BUNDLE_DATA="${APP_ROOT}/data"
BIN="${APP_ROOT}/bin/xonotic"
SCREEN_CALC="${APP_ROOT}/share/xonotic/screen-calc.sh"
FETCH_ASSETS="${APP_ROOT}/share/xonotic/fetch-assets-runtime.sh"
FETCH_ASSETS_POSIX="${APP_ROOT}/share/xonotic/fetch-assets-posix.sh"
SYNC_BUNDLE="${APP_ROOT}/share/xonotic/sync-bundle-data.sh"
# Data location:
#   Flatpak → $XDG_DATA_HOME/xonotic-touch  (~/.var/app/<id>/data/...) so
#             "Delete app data" / uninstall --delete-data actually wipes packs.
#   Else    → ~/.local/share/xonotic-touch
# Override with XONOTIC_TOUCH_USER_BASE. Legacy host path is migrated once.
LEGACY_USER_BASE="${HOME}/.local/share/xonotic-touch"
if [ -n "${XONOTIC_TOUCH_USER_BASE:-}" ]; then
    USER_BASE="$XONOTIC_TOUCH_USER_BASE"
elif [ -n "${FLATPAK_ID:-}" ] && [ -n "${XDG_DATA_HOME:-}" ]; then
    USER_BASE="${XDG_DATA_HOME}/xonotic-touch"
else
    USER_BASE="$LEGACY_USER_BASE"
fi
USER_DATA="${USER_BASE}/data"
LAYOUT_CFG="${USER_DATA}/screen.layout.cfg"
TOUCH_PROFILE="${XONOTIC_TOUCH_PROFILE:-standard}"
# Thermal is the default on tablets (Surface / fanless Iris); override with balanced/quality.
TOUCH_PERF_PROFILE="${XONOTIC_TOUCH_PERF_PROFILE:-thermal}"
TOUCH_PROFILES_DIR="${USER_DATA}/touch/profiles"
# CSQC writes layout under the gamedir (data/); keep legacy home path as fallback only.
USER_TOUCH_LAYOUT="${XONOTIC_TOUCH_LAYOUT:-${HOME}/.xonotic/data/touch.layout.cfg}"
USER_TOUCH_LAYOUT_LEGACY="${HOME}/.xonotic/touch.layout.cfg"

if [ ! -x "$BIN" ]; then
    xonotic_log "engine binary not found at $BIN"
    exit 1
fi

# --- tool bootstrap -----------------------------------------------------------
# Confined click apps may not exec host coreutils, so the package ships busybox
# applets in bin/. Host tools stay first on PATH where they actually run, so
# desktop and Flatpak installs keep using GNU behaviour.
xonotic_host_tools_usable() {
    mkdir -p "$USER_DATA" 2>/dev/null || return 1
    echo x | grep -q x 2>/dev/null || return 1
    sed '' /dev/null >/dev/null 2>&1 || return 1
    awk 'BEGIN { exit 0 }' >/dev/null 2>&1 || return 1
    cp /dev/null "$USER_DATA/.tool-probe" 2>/dev/null || return 1
    rm -f "$USER_DATA/.tool-probe" 2>/dev/null || return 1
    return 0
}

if xonotic_host_tools_usable; then
    export PATH="${PATH}:${APP_ROOT}/bin"
else
    export PATH="${APP_ROOT}/bin:${PATH}"
    if xonotic_host_tools_usable; then
        xonotic_log "using bundled busybox utilities (host binaries are confined)"
    else
        xonotic_log "no usable shell utilities — data sync and downloads will be skipped"
    fi
fi

mkdir -p "$USER_DATA" 2>/dev/null || xonotic_log "cannot create $USER_DATA"

# One launcher process only. Flatpak/desktop icons + wizard relaunches used to
# stack several start.sh copies, each spawning its own curl into the same zip.
INSTANCE_LOCK="$USER_BASE/instance.lock"
if command -v flock >/dev/null 2>&1; then
    # FD 7 stays open for the life of this process (survives bash re-exec above).
    exec 7>"$INSTANCE_LOCK"
    if ! flock -n 7; then
        xonotic_log "already running — refusing second instance"
        exit 0
    fi
fi

# Move the old host path into Flatpak app data, then delete the host copy.
# Leaving the host tree around would make "Delete app data" look like a no-op
# (packs would migrate back on the next launch). Also run when the new base
# already exists but is empty while legacy still holds packs or a partial zip —
# otherwise curl and the wizard progress file split across two directories.
if [ -n "${FLATPAK_ID:-}" ] \
    && [ "$USER_BASE" != "$LEGACY_USER_BASE" ] \
    && [ -d "$LEGACY_USER_BASE/data" ]; then
    _migrate_needed=0
    if [ ! -e "$USER_BASE" ]; then
        _migrate_needed=1
    elif [ ! -f "$USER_BASE/data/.assets-ready" ] \
        && [ ! -f "$USER_DATA/.assets-ready" ]; then
        if [ -f "$LEGACY_USER_BASE/data/.assets-ready" ] \
            || [ -d "$LEGACY_USER_BASE/data/.fetch-tmp" ] \
            || ls "$LEGACY_USER_BASE"/data/xonotic-*-data.pk3 >/dev/null 2>&1; then
            _migrate_needed=1
        fi
    fi
    if [ "$_migrate_needed" = "1" ]; then
        xonotic_log "migrating game data from $LEGACY_USER_BASE → $USER_BASE"
        if mkdir -p "$USER_BASE" \
            && cp -a "$LEGACY_USER_BASE/." "$USER_BASE/" 2>/dev/null; then
            rm -rf "$LEGACY_USER_BASE" 2>/dev/null \
                || xonotic_log "could not remove $LEGACY_USER_BASE — delete it manually for clean wipes"
            xonotic_log "legacy host data migrated"
        else
            xonotic_log "legacy migration copy failed — leaving $LEGACY_USER_BASE in place"
        fi
    fi
fi

sync_bundle_data() {
    if [ ! -x "$SYNC_BUNDLE" ]; then
        return 0
    fi
    "$SYNC_BUNDLE" "$BUNDLE_DATA" "$USER_DATA" \
        || xonotic_log "bundle sync failed"
}

sync_bundle_data

ASSET_FETCH_ACTIVE=0
TOUCH_ASSETS_READY=0
# Files the engine has to read or write live under touch/ with plain names:
# DarkPlaces' FS_CheckNastyPath refuses every path with a leading dot, so QC
# cannot see a `.asset-fetch-progress` at all (verified against fs.c). The
# shell-only `.assets-ready` marker keeps its name.
mkdir -p "$USER_DATA/touch" 2>/dev/null || xonotic_log "cannot create $USER_DATA/touch"
PROGRESS_FILE="$USER_DATA/touch/asset-progress.txt"
ASSET_FETCH_LIB="${FETCH_ASSETS%/*}/asset-fetch.sh"
ASSET_DISCOVER_LIB="${FETCH_ASSETS%/*}/asset-discover.sh"

# The download wizard asks for a relaunch by creating this file, either after a
# successful download (packs added to a running engine are not in its search
# path) or when the user taps "Try again" after a failure. The engine's write
# directory is either the gamedir or ~/.xonotic/data depending on how DarkPlaces
# resolves the user path, so both are checked (same split as touch.layout.cfg).
RESTART_MARKER="$USER_DATA/touch/relaunch-request.txt"
RESTART_MARKER_HOME="${HOME}/.xonotic/data/touch/relaunch-request.txt"

restart_requested() {
    [ -f "$RESTART_MARKER" ] || [ -f "$RESTART_MARKER_HOME" ]
}

clear_restart_request() {
    rm -f "$RESTART_MARKER" "$RESTART_MARKER_HOME" 2>/dev/null || true
}

# True when a background job is already updating the wizard progress file.
# (No `local` — this file must stay dash-safe when bash cannot re-exec.)
fetch_progress_is_live() {
    _fp_status=""
    _fp_age=0
    [ -f "$PROGRESS_FILE" ] || return 1
    _fp_status="$(head -n 1 "$PROGRESS_FILE" 2>/dev/null || true)"
    case "$_fp_status" in
        discover|running) ;;
        *) return 1 ;;
    esac
    # Fresh tick within 90s ⇒ another job owns the download; do not start another.
    _fp_age="$(($(date +%s) - $(stat -c %Y "$PROGRESS_FILE" 2>/dev/null || echo 0)))"
    [ "$_fp_age" -ge 0 ] 2>/dev/null && [ "$_fp_age" -lt 90 ]
}

# After we hold fetch.lock exclusively, drop leftover writers from killed
# launches so only this job writes into .fetch-tmp/.
kill_orphan_fetch_writers() {
    _kow_needle="$USER_DATA/.fetch-tmp/"
    for _kow_pid in $(pgrep -x curl 2>/dev/null || true) $(pgrep -x wget 2>/dev/null || true); do
        _kow_cmd="$(tr '\0' ' ' < "/proc/$_kow_pid/cmdline" 2>/dev/null || true)"
        case "$_kow_cmd" in
            *"$_kow_needle"*) kill "$_kow_pid" 2>/dev/null || true ;;
        esac
    done
}

# Download must not outlive the app (wizard says keep it open; curl -C - resumes).
ASSET_FETCH_PID=""

write_fetch_paused() {
    _wfp_pct=0
    [ -f "$USER_DATA/.assets-ready" ] && return 0
    if [ -f "$PROGRESS_FILE" ]; then
        _wfp_pct="$(sed -n '2p' "$PROGRESS_FILE" 2>/dev/null || echo 0)"
    fi
    case "$_wfp_pct" in
        ''|*[!0-9]*) _wfp_pct=0 ;;
    esac
    {
        printf '%s\n' paused
        printf '%s\n' "$_wfp_pct"
        printf '%s\n' "Download paused. Reopen Xonotic Touch to continue."
    } > "${PROGRESS_FILE}.tmp.$$" 2>/dev/null \
        && mv -f "${PROGRESS_FILE}.tmp.$$" "$PROGRESS_FILE" 2>/dev/null \
        || true
}

# Kill a PID and every descendant (fetch shell → curl). Dash-safe, no locals.
kill_process_tree() {
    _kpt_root="$1"
    [ -n "$_kpt_root" ] || return 0
    for _kpt_child in $(pgrep -P "$_kpt_root" 2>/dev/null || true); do
        kill_process_tree "$_kpt_child"
    done
    kill -TERM "$_kpt_root" 2>/dev/null || true
}

stop_asset_fetch() {
    _saf_pid="${ASSET_FETCH_PID:-}"
    ASSET_FETCH_PID=""
    if [ -n "$_saf_pid" ]; then
        kill_process_tree "$_saf_pid"
        # Brief grace for curl to flush; then hard-kill anything left.
        sleep 0.3 2>/dev/null || sleep 1
        kill_process_tree "$_saf_pid"
        kill -KILL "$_saf_pid" 2>/dev/null || true
        for _saf_child in $(pgrep -P "$_saf_pid" 2>/dev/null || true); do
            kill -KILL "$_saf_child" 2>/dev/null || true
        done
        wait "$_saf_pid" 2>/dev/null || true
    fi
    kill_orphan_fetch_writers
    write_fetch_paused
}

launcher_cleanup() {
    # Drop traps first so a second signal during teardown cannot re-enter.
    trap - EXIT INT TERM HUP
    stop_asset_fetch
}

trap 'launcher_cleanup; exit 130' INT
trap 'launcher_cleanup; exit 143' TERM
trap 'launcher_cleanup; exit 129' HUP
trap launcher_cleanup EXIT

# Asset resolution:
#   Sync:  our USER_DATA already ready? → no wizard
#   Else:  show fullscreen wizard; background does Flatpak copy → else download
# Never block launch on Flatpak/network — that was freezing "detection".
# Called at most once per launcher process (relaunch loop must not stack curls).
prepare_assets() {
    ASSET_FETCH_ACTIVE=0
    TOUCH_ASSETS_READY=0

    if [ "${ASSET_FETCH_JOB_STARTED:-0}" = "1" ]; then
        if [ -f "$USER_DATA/.assets-ready" ]; then
            TOUCH_ASSETS_READY=1
        else
            ASSET_FETCH_ACTIVE=1
        fi
        return 0
    fi

    # The asset libraries are bash-only; sourcing them from dash is a syntax
    # error. Confined clicks usually cannot exec host bash, so fall back to the
    # POSIX autobuild downloader (busybox wget/unzip).
    if [ -n "${BASH_VERSION:-}" ] && [ "${XONOTIC_SKIP_ASSET_FETCH:-0}" != "1" ] && [ -f "$ASSET_FETCH_LIB" ]; then
        # shellcheck source=/dev/null
        . "$ASSET_FETCH_LIB"
        if [ -f "$ASSET_DISCOVER_LIB" ]; then
            # shellcheck source=/dev/null
            . "$ASSET_DISCOVER_LIB"
        fi
        export XONOTIC_ASSET_FETCH_PROGRESS="$PROGRESS_FILE"
        if xonotic_assets_are_ready "$USER_DATA"; then
            TOUCH_ASSETS_READY=1
        else
            ASSET_FETCH_ACTIVE=1
            ASSET_FETCH_JOB_STARTED=1
            if fetch_progress_is_live; then
                xonotic_log "asset fetch already in progress — joining existing job"
            else
                # Seed progress before the engine opens so the wizard is never blank.
                {
                    printf '%s\n' discover
                    printf '%s\n' 5
                    printf '%s\n' "Checking your game data..."
                } > "$PROGRESS_FILE"
            fi
            # flock: relaunch / double-start must not run two curls into one zip.
            (
                trap 'kill_orphan_fetch_writers; exit 143' TERM INT
                if command -v flock >/dev/null 2>&1; then
                    flock -n 9 || exit 0
                fi
                kill_orphan_fetch_writers
                if declare -F xonotic_resolve_missing_assets >/dev/null 2>&1; then
                    xonotic_resolve_missing_assets "$USER_DATA"
                else
                    xonotic_fetch_game_assets "$USER_DATA"
                fi
                sync_bundle_data
            ) 9>"$USER_DATA/touch/fetch.lock" &
            ASSET_FETCH_PID=$!
        fi
    elif [ -f "$USER_DATA/.assets-ready" ]; then
        TOUCH_ASSETS_READY=1
    elif [ "${XONOTIC_SKIP_ASSET_FETCH:-0}" != "1" ] && [ -x "$FETCH_ASSETS_POSIX" ]; then
        ASSET_FETCH_ACTIVE=1
        ASSET_FETCH_JOB_STARTED=1
        # Same file the wizard polls; without this the POSIX downloader reports
        # nowhere and setup sits on its opening message for the whole download.
        export XONOTIC_ASSET_FETCH_PROGRESS="$PROGRESS_FILE"
        if ! fetch_progress_is_live; then
            {
                printf '%s\n' discover
                printf '%s\n' 5
                printf '%s\n' "Checking your game data..."
            } > "$PROGRESS_FILE"
        fi
        (
            trap 'kill_orphan_fetch_writers; exit 143' TERM INT
            if command -v flock >/dev/null 2>&1; then
                flock -n 9 || exit 0
            fi
            kill_orphan_fetch_writers
            "$FETCH_ASSETS_POSIX" "$USER_DATA"
            sync_bundle_data
        ) 9>"$USER_DATA/touch/fetch.lock" &
        ASSET_FETCH_PID=$!
    fi
}

# Gameplay stays blocked in-menu until assets are ready (asset-fetch dialog).

DATA_DIR="$USER_DATA"

# Desktop dev window is opt-in; real touch devices should use the full touch code path.
if [ "${XONOTIC_TOUCH_DESKTOP_DEV:-0}" = "1" ]; then
    IS_DESKTOP=1
else
    IS_DESKTOP=0
fi

if [ "$IS_DESKTOP" = "1" ]; then
    FULLSCREEN=0
    XONOTIC_VID_WIDTH="${XONOTIC_DESKTOP_WIDTH:-412}"
    XONOTIC_VID_HEIGHT="${XONOTIC_DESKTOP_HEIGHT:-915}"
    XONOTIC_TOUCH_XDPI="${XONOTIC_TOUCH_XDPI:-429}"
    XONOTIC_TOUCH_YDPI="${XONOTIC_TOUCH_YDPI:-429}"
    XONOTIC_TOUCH_DENSITY="${XONOTIC_TOUCH_DENSITY:-2.625}"
    cat > "$LAYOUT_CFG" <<EOF || xonotic_log "cannot write $LAYOUT_CFG"
// Generated by packaging/start.sh (desktop portrait test window)
vid_width ${XONOTIC_VID_WIDTH}
vid_height ${XONOTIC_VID_HEIGHT}
vid_conwidthauto 0
vid_conwidth ${XONOTIC_VID_WIDTH}
vid_conheight ${XONOTIC_VID_HEIGHT}
vid_touchscreen_xdpi ${XONOTIC_TOUCH_XDPI}
vid_touchscreen_ydpi ${XONOTIC_TOUCH_YDPI}
vid_touchscreen_density ${XONOTIC_TOUCH_DENSITY}
EOF
elif [ -f "$SCREEN_CALC" ]; then
    # Prefer landscape for two-thumb controls (GNOME auto-rotate fights this).
    if [ "${XONOTIC_TOUCH_LOCK_LANDSCAPE:-1}" = "1" ] && command -v gsettings >/dev/null 2>&1; then
        gsettings set org.gnome.settings-daemon.plugins.orientation active false 2>/dev/null || true
    fi
    # Ask the session for cooler power before we spin the GPU.
    if command -v gsettings >/dev/null 2>&1; then
        gsettings set org.gnome.settings-daemon.plugins.power power-saver-profile-on-low-battery true 2>/dev/null || true
        gsettings set org.gnome.shell power-saver false 2>/dev/null || true
    fi
    if command -v powerprofilesctl >/dev/null 2>&1; then
        powerprofilesctl set power-saver 2>/dev/null || true
    fi
    # shellcheck source=/dev/null
    . "$SCREEN_CALC"
    # Fanless default unless explicitly overridden.
    XONOTIC_RENDER_MAX_EDGE="${XONOTIC_RENDER_MAX_EDGE:-960}"
    xonotic_screen_calc "$LAYOUT_CFG" || xonotic_log "screen calc failed — using defaults"
else
    xonotic_log "screen-calc missing at $SCREEN_CALC"
    XONOTIC_VID_WIDTH="${XONOTIC_DEFAULT_WIDTH:-1920}"
    XONOTIC_VID_HEIGHT="${XONOTIC_DEFAULT_HEIGHT:-1080}"
    XONOTIC_TOUCH_XDPI="${XONOTIC_TOUCH_XDPI:-320}"
    XONOTIC_TOUCH_YDPI="${XONOTIC_TOUCH_YDPI:-320}"
    XONOTIC_TOUCH_DENSITY="${XONOTIC_TOUCH_DENSITY:-2.0}"
    cat > "$LAYOUT_CFG" <<EOF || xonotic_log "cannot write $LAYOUT_CFG"
// Fallback layout (screen-calc.sh not installed)
vid_width ${XONOTIC_VID_WIDTH}
vid_height ${XONOTIC_VID_HEIGHT}
vid_touchscreen_xdpi ${XONOTIC_TOUCH_XDPI}
vid_touchscreen_ydpi ${XONOTIC_TOUCH_YDPI}
vid_touchscreen_density ${XONOTIC_TOUCH_DENSITY}
EOF
fi

# A partial screen probe must never hand empty cvar values to the engine.
XONOTIC_VID_WIDTH="${XONOTIC_VID_WIDTH:-${XONOTIC_DEFAULT_WIDTH:-1920}}"
XONOTIC_VID_HEIGHT="${XONOTIC_VID_HEIGHT:-${XONOTIC_DEFAULT_HEIGHT:-1080}}"
XONOTIC_TOUCH_XDPI="${XONOTIC_TOUCH_XDPI:-320}"
XONOTIC_TOUCH_YDPI="${XONOTIC_TOUCH_YDPI:-320}"
XONOTIC_TOUCH_DENSITY="${XONOTIC_TOUCH_DENSITY:-2.0}"

if [ -n "${LD_LIBRARY_PATH:-}" ]; then
    export LD_LIBRARY_PATH="${APP_ROOT}/lib:${LD_LIBRARY_PATH}"
else
    export LD_LIBRARY_PATH="${APP_ROOT}/lib"
fi

# DarkPlaces session lock (when locksession>0). Only clear a stale file — never
# delete a lock held by a live engine (that allowed stacked instances).
XONOTIC_LOCK="${HOME}/.xonotic/lock"
if [ -f "$XONOTIC_LOCK" ] && ! pgrep -f "${BIN}" >/dev/null 2>&1; then
    rm -f "$XONOTIC_LOCK" 2>/dev/null || true
fi

if [ "$IS_DESKTOP" != "1" ]; then
    FULLSCREEN=1
fi

STARTUP_CFG="${DATA_DIR}/touch/startup.cfg"
mkdir -p "${DATA_DIR}/touch/profiles" 2>/dev/null || true
{
    echo "// Generated by packaging/start.sh"
    # Kill leftover QC statement tracing / developer spam (was causing huge lag).
    echo "developer 0"
    echo "prvm_traceqc 0"
    echo "prvm_statementprofiling 0"
    echo "prvm_timeprofiling 0"
    if [ -f "${TOUCH_PROFILES_DIR}/${TOUCH_PROFILE}.cfg" ]; then
        echo "exec touch/profiles/${TOUCH_PROFILE}.cfg"
    else
        echo "xonotic-touch: touch profile missing: ${TOUCH_PROFILES_DIR}/${TOUCH_PROFILE}.cfg" >&2
    fi
    if [ -f "${TOUCH_PROFILES_DIR}/${TOUCH_PERF_PROFILE}.cfg" ]; then
        echo "exec touch/profiles/${TOUCH_PERF_PROFILE}.cfg"
    fi
    # Prefer engine-resolved name so fopen/exec share one file (TOUCH_LAYOUT_SPEC D14).
    if [ -f "$USER_TOUCH_LAYOUT" ] || [ -f "${DATA_DIR}/touch.layout.cfg" ]; then
        echo "exec touch.layout.cfg"
    elif [ -f "$USER_TOUCH_LAYOUT_LEGACY" ]; then
        echo "exec ${USER_TOUCH_LAYOUT_LEGACY}"
    fi
} > "$STARTUP_CFG" || xonotic_log "cannot write $STARTUP_CFG"

# The engine resolves -xonotic gamedirs relative to the cwd.
cd "$USER_BASE" 2>/dev/null || xonotic_log "cannot enter $USER_BASE — engine may not find game data"

# Re-exec user config AFTER xonotic.cfg: the default chain (xonotic-client.cfg)
# sets `_cl_name ""` which would wipe the archived player name every launch and
# force the FirstRun wizard again. config.cfg / autoexec restore user prefs.
run_engine() {
    "$BIN" -xonotic \
        -customgamename "Xonotic Touch" \
        +exec xonotic.cfg \
        +exec screen.layout.cfg \
        +exec config.cfg \
        +exec autoexec.cfg \
        +exec touch/startup.cfg \
        +set _touch_asset_fetch_active "$ASSET_FETCH_ACTIVE" \
        +set _touch_assets_ready "$TOUCH_ASSETS_READY" \
        +vid_fullscreen "$FULLSCREEN" \
        +vid_touchscreen 1 \
        +vid_conwidthauto 1 \
        +vid_conheight "$XONOTIC_VID_HEIGHT" \
        +vid_width "$XONOTIC_VID_WIDTH" \
        +vid_height "$XONOTIC_VID_HEIGHT" \
        +vid_touchscreen_xdpi "$XONOTIC_TOUCH_XDPI" \
        +vid_touchscreen_ydpi "$XONOTIC_TOUCH_YDPI" \
        +vid_touchscreen_density "$XONOTIC_TOUCH_DENSITY" \
        +cl_movement 1 \
        +con_closeontoggle 1 \
        +scr_screenshot_jpeg 0 \
        +developer 0 \
        +prvm_traceqc 0 \
        +prvm_statementprofiling 0 \
        +prvm_timeprofiling 0
}

# A stale marker from a killed session must not relaunch this one.
clear_restart_request

ASSET_FETCH_JOB_STARTED=0
ENGINE_STATUS=0
while :; do
    prepare_assets
    run_engine || ENGINE_STATUS=$?
    # Intentional post-download relaunch: keep the (usually finished) fetch job
    # alone; do not treat engine exit as "user closed the app".
    if restart_requested; then
        clear_restart_request
        xonotic_log "relaunching engine so downloaded game data is loaded"
        ENGINE_STATUS=0
        continue
    fi
    break
done

# User closed the app (or the engine exited without asking for relaunch).
# Stop download explicitly; EXIT trap is a backstop for signals.
xonotic_log "stopping asset fetch (app closing)"
stop_asset_fetch
trap - EXIT INT TERM HUP
exit "$ENGINE_STATUS"
