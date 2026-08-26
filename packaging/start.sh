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
    # Not "downloads are off": prepare_assets falls back to the POSIX
    # downloader (fetch-assets-posix.sh, busybox wget/unzip). Only the bash-only
    # discovery libraries are out of reach. Saying otherwise sent the diagnosis
    # of issue #19 in the wrong direction.
    xonotic_log "bash unavailable — using the POSIX asset downloader"
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
#   Click   → $XDG_DATA_HOME/<APP_PKGNAME>  (only path AppArmor allows creating
#             files under; ~/.local/share/xonotic-touch is denied — issue #19)
#   Else    → ~/.local/share/xonotic-touch
# Override with XONOTIC_TOUCH_USER_BASE. Legacy host path is migrated once.
LEGACY_USER_BASE="${HOME}/.local/share/xonotic-touch"
# APP_ID is <pkgname>_<appname>_<version> (e.g. xonotictouch.dixonsolutions_xonotic_1.2.42).
CLICK_PKGNAME=""
if [ -n "${APP_ID:-}" ]; then
    CLICK_PKGNAME="${APP_ID%%_*}"
fi
if [ -n "${XONOTIC_TOUCH_USER_BASE:-}" ]; then
    USER_BASE="$XONOTIC_TOUCH_USER_BASE"
elif [ -n "${FLATPAK_ID:-}" ] && [ -n "${XDG_DATA_HOME:-}" ]; then
    USER_BASE="${XDG_DATA_HOME}/xonotic-touch"
elif [ -n "$CLICK_PKGNAME" ]; then
    USER_BASE="${XDG_DATA_HOME:-${HOME}/.local/share}/${CLICK_PKGNAME}"
elif [ -n "${UBUNTU_APPLICATION_ISOLATION:-}" ]; then
    USER_BASE="${XDG_DATA_HOME:-${HOME}/.local/share}/xonotictouch.dixonsolutions"
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

# One UI launcher at a time. The background fetchd does not hold this lock, so
# reopening the app while a download runs joins the existing job instead of
# stacking curls.
#
# Click confinement often leaves host flock(1) on PATH but denies executing it.
# Never treat a failed flock exec (or a non-writable lock path) as "already
# running" — that aborts launch before the engine starts (issue #19).
INSTANCE_LOCK="$USER_BASE/instance.lock"
engine_is_running() {
    pgrep -f "${BIN}" >/dev/null 2>&1
}

xonotic_notify_existing_session() {
    if engine_is_running; then
        xonotic_log "already running — ignoring duplicate launch"
    else
        xonotic_log "session already active — ignoring duplicate launch"
    fi
}

if command -v flock >/dev/null 2>&1; then
    # FD 7 stays open for the life of this process (survives bash re-exec above).
    if { exec 7>"$INSTANCE_LOCK"; } 2>/dev/null; then
        if flock -n 7 2>/dev/null; then
            : # we hold the single-instance lock
        else
            _xonotic_flock_status=$?
            # flock -n returns 1 when another process holds the lock. Any other
            # status (126/127 = AppArmor denied exec, etc.) must not abort launch.
            if [ "$_xonotic_flock_status" -eq 1 ]; then
                xonotic_notify_existing_session
                exit 0
            fi
            xonotic_log "flock unavailable (status ${_xonotic_flock_status}) — skipping single-instance guard"
            exec 7>&- 2>/dev/null || true
        fi
    else
        xonotic_log "cannot create $INSTANCE_LOCK — continuing without single-instance guard"
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

# Old device-test overrides under ~/.xonotic/data (zzz*-touch-fix.pk3dir and a
# copied xonotic-data.pk3dir) ship a menu.dat that still opens
# `.asset-fetch-progress` (rejected by FS_CheckNastyPath) and shows the stuck
# "Preparing download..." copy. Those packs sort above the Flatpak menu and
# freeze the wizard even while the shell writes live progress. Quarantine them.
quarantine_stale_touch_menu_overrides() {
    _q_home_data="${HOME}/.xonotic/data"
    [ -d "$_q_home_data" ] || return 0
    for _q_dir in \
        "$_q_home_data/zzz-touch-fix.pk3dir" \
        "$_q_home_data/zzzz-touch-fix.pk3dir"
    do
        if [ -d "$_q_dir" ]; then
            xonotic_log "removing stale test override $_q_dir (shadowed setup wizard)"
            rm -rf "$_q_dir" 2>/dev/null || true
        fi
    done
    _q_menu="$_q_home_data/xonotic-data.pk3dir/menu.dat"
    if [ -f "$_q_menu" ] && grep -Fq "asset-fetch-progress" "$_q_menu" 2>/dev/null; then
        xonotic_log "quarantining stale $_q_menu (old asset-fetch progress path)"
        mv -f "$_q_menu" "${_q_menu}.stale-pre-touch-progress" 2>/dev/null || true
    fi
    # Stale home CSQC sorts above Flatpak touch/data packs and causes
    # CL_ParseServerMessage: Illegible server message when SVQC/CSQC diverge.
    _q_csqc="$_q_home_data/xonotic-data.pk3dir/csprogs.dat"
    if [ -f "$_q_csqc" ]; then
        xonotic_log "quarantining stale $_q_csqc (home override shadows bundled Touch CSQC)"
        mv -f "$_q_csqc" "${_q_csqc}.stale-home-override" 2>/dev/null || true
    fi
}

sync_bundle_data
quarantine_stale_touch_menu_overrides

# Stock multiplayer servers push their csprogs into dlcache and wipe Touch HUD /
# Console. Drop those caches each launch; cl_csqc_download 0 (engine) prevents
# re-download once the Flatpak ships that cvar.
if [ -d "${HOME}/.xonotic/data/dlcache" ]; then
    rm -f "${HOME}/.xonotic/data/dlcache"/csprogs.dat.* 2>/dev/null || true
fi



ASSET_FETCH_ACTIVE=0
TOUCH_ASSETS_READY=0
# Files the engine has to read or write live under touch/ with plain names:
# DarkPlaces' FS_CheckNastyPath refuses every path with a leading dot, so QC
# cannot see a `.asset-fetch-progress` at all (verified against fs.c). The
# shell-only `.assets-ready` marker keeps its name.
mkdir -p "$USER_DATA/touch" 2>/dev/null || xonotic_log "cannot create $USER_DATA/touch"
# Menu QC FILE_WRITE often lands in the engine userdir (~/.xonotic/data), not
# the Flatpak gamedir — keep both marker directories ready.
mkdir -p "${HOME}/.xonotic/data/touch" 2>/dev/null || true
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
BACKGROUND_MARKER="$USER_DATA/touch/background-fetch-request.txt"
BACKGROUND_MARKER_HOME="${HOME}/.xonotic/data/touch/background-fetch-request.txt"
FETCHD_PIDFILE="$USER_BASE/fetchd.pid"
HELPER_LIB_DIR="$USER_BASE/lib"
FLATPAK_APP_ID="${FLATPAK_ID:-io.github.dixonSolutions.XonoticTouch}"

restart_requested() {
    [ -f "$RESTART_MARKER" ] || [ -f "$RESTART_MARKER_HOME" ]
}

clear_restart_request() {
    rm -f "$RESTART_MARKER" "$RESTART_MARKER_HOME" 2>/dev/null || true
}

background_fetch_requested() {
    [ -f "$BACKGROUND_MARKER" ] || [ -f "$BACKGROUND_MARKER_HOME" ]
}

clear_background_fetch_request() {
    rm -f "$BACKGROUND_MARKER" "$BACKGROUND_MARKER_HOME" 2>/dev/null || true
}

# Copy download helpers into user data so a host-side fetchd can run them after
# the Flatpak sandbox exits ( /app is not visible on the host ).
sync_session_helpers() {
    mkdir -p "$HELPER_LIB_DIR" 2>/dev/null || return 0
    for _h in asset-fetch.sh asset-discover.sh sync-bundle-data.sh \
        xonotic-touch-fetchd.sh fetch-assets-posix.sh; do
        if [ -f "${APP_ROOT}/share/xonotic/$_h" ]; then
            cp -f "${APP_ROOT}/share/xonotic/$_h" "$HELPER_LIB_DIR/$_h" 2>/dev/null || true
            case "$_h" in
                *.sh) chmod +x "$HELPER_LIB_DIR/$_h" 2>/dev/null || true ;;
            esac
        fi
    done
}

# Run a command on the host when we are inside Flatpak (desktop notifications,
# and a download that must outlive `flatpak kill` of the UI instance).
host_run() {
    if [ -n "${FLATPAK_ID:-}" ] && command -v flatpak-spawn >/dev/null 2>&1; then
        flatpak-spawn --host "$@"
        return $?
    fi
    "$@"
}

# The host-side fetchd lives outside the Flatpak PID namespace. In-sandbox
# kill -0 on its PID always fails and looks like "the download died".
host_pid_alive() {
    _hpa_pid="$1"
    [ -n "$_hpa_pid" ] || return 1
    host_run kill -0 "$_hpa_pid" 2>/dev/null
}

fetchd_is_live() {
    [ -f "$FETCHD_PIDFILE" ] || return 1
    _fd_pid="$(cat "$FETCHD_PIDFILE" 2>/dev/null || true)"
    [ -n "$_fd_pid" ] || return 1
    host_pid_alive "$_fd_pid"
}

# Prefer a host fetchd so closing the Flatpak UI does not kill curl.
# flock inside fetchd is the real single-writer guard — a stale "running"
# progress line must not prevent a handoff after the sandbox fetch was killed.
ensure_fetchd() {
    [ "${XONOTIC_SKIP_ASSET_FETCH:-0}" = "1" ] && return 0
    fetchd_is_live && return 0
    if [ -f "$USER_DATA/.assets-ready" ]; then
        return 0
    fi
    # fetchd is a bash script, and a confined click cannot exec bash at all, so
    # on Ubuntu Touch it can never start. Refusing the job here rather than
    # trying and failing is the whole point: the placeholder progress file below
    # is written before the daemon is launched, and a fresh one reads as a live
    # download to fetch_progress_is_live -- which then suppresses the POSIX
    # fallback in prepare_assets, the only downloader such a device has. The
    # wizard sat at "Checking your game data..." forever and nothing fetched.
    if [ -z "${BASH_VERSION:-}" ]; then
        return 1
    fi
    sync_session_helpers
    [ -x "$HELPER_LIB_DIR/xonotic-touch-fetchd.sh" ] || return 1
    xonotic_log "starting background asset fetchd"
    _ef_wrote_progress=0
    if [ ! -f "$PROGRESS_FILE" ]; then
        {
            printf '%s\n' discover
            printf '%s\n' 5
            printf '%s\n' "Checking your game data..."
        } > "$PROGRESS_FILE"
        _ef_wrote_progress=1
    fi
    host_run env \
        "XONOTIC_TOUCH_USER_BASE=$USER_BASE" \
        "XONOTIC_TOUCH_LIB_DIR=$HELPER_LIB_DIR" \
        "XONOTIC_TOUCH_FLATPAK_ID=$FLATPAK_APP_ID" \
        "XONOTIC_TOUCH_BUNDLE_DATA=$BUNDLE_DATA" \
        "XONOTIC_ASSET_FETCH_PROGRESS=$PROGRESS_FILE" \
        "XONOTIC_TOUCH_AUTO_OPEN_ON_READY=1" \
        "XONOTIC_TOUCH_NOTIFY_ON_READY=1" \
        bash "$HELPER_LIB_DIR/xonotic-touch-fetchd.sh" \
        >/dev/null 2>&1 &
    # Wait for the pidfile instead of assuming. The old 0.2s sleep was also the
    # last command, so this function reported success for a daemon that had not
    # started -- and on a slow device, for one that simply had not got there yet.
    _ef_i=0
    while [ "$_ef_i" -lt 20 ]; do
        if fetchd_is_live; then
            return 0
        fi
        sleep 0.1 2>/dev/null || true
        _ef_i=$((_ef_i + 1))
    done
    # It never came up. Drop the placeholder, or the caller sees a download in
    # flight that does not exist and skips starting a real one.
    if [ "$_ef_wrote_progress" = "1" ]; then
        rm -f "$PROGRESS_FILE" 2>/dev/null || true
    fi
    return 1
}

stop_fetchd() {
    if fetchd_is_live; then
        _sfd_pid="$(cat "$FETCHD_PIDFILE" 2>/dev/null || true)"
        if [ -n "$_sfd_pid" ]; then
            host_run kill -TERM "$_sfd_pid" 2>/dev/null || kill -TERM "$_sfd_pid" 2>/dev/null || true
            sleep 0.3 2>/dev/null || true
            host_run kill -KILL "$_sfd_pid" 2>/dev/null || kill -KILL "$_sfd_pid" 2>/dev/null || true
        fi
    fi
    rm -f "$FETCHD_PIDFILE" 2>/dev/null || true
    kill_orphan_fetch_writers
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
    for _kow_pid in $(pgrep -x curl 2>/dev/null || true) \
        $(pgrep -x wget 2>/dev/null || true) \
        $(pgrep -x aria2c 2>/dev/null || true); do
        _kow_cmd="$(tr '\0' ' ' < "/proc/$_kow_pid/cmdline" 2>/dev/null || true)"
        case "$_kow_cmd" in
            *"$_kow_needle"*) kill "$_kow_pid" 2>/dev/null || true ;;
        esac
    done
}

# In-sandbox fetch PID (fallback when host fetchd cannot start). Prefer fetchd.
ASSET_FETCH_PID=""
# When set, quitting the engine leaves fetchd running instead of pausing.
SESSION_KEEP_BACKGROUND=0

write_fetch_paused() {
    _wfp_pct=0
    [ -f "$USER_DATA/.assets-ready" ] && return 0
    # Host fetchd still owns the download — do not mark paused.
    fetchd_is_live && return 0
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
    # Leave host fetchd alone when the session continues in the background.
    if [ "${SESSION_KEEP_BACKGROUND:-0}" = "1" ]; then
        ASSET_FETCH_PID=""
        return 0
    fi
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
    stop_fetchd
    kill_orphan_fetch_writers
    write_fetch_paused
}

launcher_cleanup() {
    # Drop traps first so a second signal during teardown cannot re-enter.
    trap - EXIT INT TERM HUP
    if [ "${SESSION_KEEP_BACKGROUND:-0}" = "1" ]; then
        # Detach in-sandbox child so EXIT does not reap a download we handed off.
        ASSET_FETCH_PID=""
        return 0
    fi
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
        # A live download must keep the setup wizard up even if minimum packs
        # already satisfy .assets-ready (progressive extract / extra packs).
        if xonotic_assets_are_ready "$USER_DATA" \
            && ! fetchd_is_live && ! fetch_progress_is_live; then
            TOUCH_ASSETS_READY=1
        else
            ASSET_FETCH_ACTIVE=1
            ASSET_FETCH_JOB_STARTED=1
            if fetchd_is_live; then
                xonotic_log "asset fetchd already running — joining existing job"
            else
                # Always try host fetchd. A stale progress file alone must not
                # skip starting work; fetchd's flock is the real single-writer.
                if ensure_fetchd && fetchd_is_live; then
                    xonotic_log "asset fetchd started (survives UI close)"
                elif fetch_progress_is_live; then
                    xonotic_log "asset fetch already in progress — joining existing job"
                else
                    {
                        printf '%s\n' discover
                        printf '%s\n' 5
                        printf '%s\n' "Checking your game data..."
                    } > "$PROGRESS_FILE"
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
            fi
        fi
    elif fetchd_is_live || fetch_progress_is_live; then
        ASSET_FETCH_ACTIVE=1
        ASSET_FETCH_JOB_STARTED=1
    elif [ -f "$USER_DATA/.assets-ready" ]; then
        TOUCH_ASSETS_READY=1
    elif [ "${XONOTIC_SKIP_ASSET_FETCH:-0}" != "1" ] && [ -x "$FETCH_ASSETS_POSIX" ]; then
        ASSET_FETCH_ACTIVE=1
        ASSET_FETCH_JOB_STARTED=1
        export XONOTIC_ASSET_FETCH_PROGRESS="$PROGRESS_FILE"
        if fetchd_is_live; then
            :
        elif ensure_fetchd && fetchd_is_live; then
            :
        elif fetch_progress_is_live; then
            :
        else
            {
                printf '%s\n' discover
                printf '%s\n' 5
                printf '%s\n' "Checking your game data..."
            } > "$PROGRESS_FILE"
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
clear_background_fetch_request

sync_session_helpers

ASSET_FETCH_JOB_STARTED=0
ENGINE_STATUS=0

while :; do
    prepare_assets
    run_engine || ENGINE_STATUS=$?

    if restart_requested; then
        clear_restart_request
        xonotic_log "relaunching engine so downloaded game data is loaded"
        ENGINE_STATUS=0
        continue
    fi

    # Wizard: "Download in background" / Close window — the UI exits and the
    # host-side fetchd carries the download on its own, notifying (and reopening
    # the app) once the packs land.
    if background_fetch_requested; then
        clear_background_fetch_request
        SESSION_KEEP_BACKGROUND=1
        # Hand off any in-sandbox curl to host fetchd (resume partials).
        if [ -n "${ASSET_FETCH_PID:-}" ]; then
            kill_process_tree "$ASSET_FETCH_PID" 2>/dev/null || true
            ASSET_FETCH_PID=""
            sleep 0.5 2>/dev/null || true
        fi
        ensure_fetchd || true
        xonotic_log "download continues in background (notification when done)"
        trap - EXIT INT TERM HUP
        exit 0
    fi

    # Window closed. Closing quits the session — the one thing allowed to outlive
    # it is a download already in flight, which fetchd finishes on the host.
    if fetchd_is_live || fetch_progress_is_live; then
        SESSION_KEEP_BACKGROUND=1
        ensure_fetchd || true
        xonotic_log "UI closed — download still running in background"
        trap - EXIT INT TERM HUP
        exit 0
    fi
    break
done

xonotic_log "stopping session"
SESSION_KEEP_BACKGROUND=0
stop_asset_fetch
trap - EXIT INT TERM HUP
exit "$ENGINE_STATUS"
