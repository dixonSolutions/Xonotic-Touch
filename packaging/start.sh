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
# Always use the host xdg-data path. Flatpak sets XDG_DATA_HOME to
# ~/.var/app/<app>/data, which would fork assets/config away from the
# documented ~/.local/share/xonotic-touch tree (and the
# --filesystem=xdg-data/xonotic-touch bind). Override with XONOTIC_TOUCH_USER_BASE.
USER_BASE="${XONOTIC_TOUCH_USER_BASE:-${HOME}/.local/share/xonotic-touch}"
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
PROGRESS_FILE="$USER_DATA/.asset-fetch-progress"
ASSET_FETCH_LIB="${FETCH_ASSETS%/*}/asset-fetch.sh"
ASSET_DISCOVER_LIB="${FETCH_ASSETS%/*}/asset-discover.sh"

# The asset libraries are bash-only; sourcing them from dash is a syntax error.
# Confined clicks usually cannot exec host bash, so fall back to the POSIX
# autobuild downloader (busybox wget/unzip).
if [ -n "${BASH_VERSION:-}" ] && [ "${XONOTIC_SKIP_ASSET_FETCH:-0}" != "1" ] && [ -f "$ASSET_FETCH_LIB" ]; then
	# shellcheck source=/dev/null
	. "$ASSET_FETCH_LIB"
	if [ -f "$ASSET_DISCOVER_LIB" ]; then
		# shellcheck source=/dev/null
		. "$ASSET_DISCOVER_LIB"
		export XONOTIC_ASSET_FETCH_PROGRESS="$PROGRESS_FILE"
		xonotic_try_discover_assets "$USER_DATA" || true
		sync_bundle_data
	fi
	if xonotic_assets_are_ready "$USER_DATA"; then
		TOUCH_ASSETS_READY=1
	else
		ASSET_FETCH_ACTIVE=1
		export XONOTIC_ASSET_FETCH_PROGRESS="$PROGRESS_FILE"
		rm -f "$PROGRESS_FILE"
		(
			xonotic_fetch_game_assets "$USER_DATA"
			sync_bundle_data
		) &
	fi
elif [ -f "$USER_DATA/.assets-ready" ]; then
	TOUCH_ASSETS_READY=1
elif [ "${XONOTIC_SKIP_ASSET_FETCH:-0}" != "1" ] && [ -x "$FETCH_ASSETS_POSIX" ]; then
	ASSET_FETCH_ACTIVE=1
	export XONOTIC_ASSET_FETCH_PROGRESS="$PROGRESS_FILE"
	rm -f "$PROGRESS_FILE"
	(
		"$FETCH_ASSETS_POSIX" "$USER_DATA"
		sync_bundle_data
	) &
fi

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

XONOTIC_LOCK="${HOME}/.xonotic/lock"
if [ -f "$XONOTIC_LOCK" ]; then
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
# shellcheck disable=SC2086
exec "$BIN" -xonotic \
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
