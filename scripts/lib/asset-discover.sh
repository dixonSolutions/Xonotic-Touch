#!/bin/bash
# Resolve game assets for Xonotic Touch into OUR user data directory only.
#
# Sync (before launch): only ask "is our data already ready?"
# Background (drives fullscreen wizard): Flatpak copy → else download.
#
# Order (stop at first success):
#   1. Assets already in our app data → use them (no import, no download)
#   2. Original Flatpak org.xonotic.Xonotic → full copy into our data
#   3. Otherwise download via the in-game wizard
# Keep options local to direct execution; see asset-fetch.sh.
if [ "${BASH_SOURCE[0]-}" = "${0-}" ]; then
    set -euo pipefail
fi

XONOTIC_DISCOVERY_PK3_PATTERNS=(
    'xonotic-*-data.pk3'
    'xonotic-*-maps.pk3'
    'xonotic-*-music.pk3'
    'xonotic-*-nexcompat.pk3'
)

XONOTIC_DISCOVERY_PK3DIR_TREES=(
    'xonotic-data.pk3dir'
    'xonotic-maps.pk3dir'
    'xonotic-music.pk3dir'
    'xonotic-nexcompat.pk3dir'
)

# Directory presence is enough — never glob/list huge texture trees (bash hangs).
XONOTIC_DISCOVERY_PK3DIR_MARKERS=(
    'xonotic-data.pk3dir/textures'
    'xonotic-maps.pk3dir/maps'
    'xonotic-music.pk3dir/music'
    'xonotic-nexcompat.pk3dir/textures'
)

# flatpak(1) often wedges inside another Flatpak; keep a tiny timeout and prefer paths.
XONOTIC_FLATPAK_INFO_TIMEOUT_SEC="${XONOTIC_FLATPAK_INFO_TIMEOUT_SEC:-1}"

xonotic_discovery_progress_write() {
    local status="$1"
    local percent="$2"
    local message="$3"
    local file="${XONOTIC_ASSET_FETCH_PROGRESS:-}"
    local tmp

    if [ -z "$file" ]; then
        return 0
    fi
    mkdir -p "$(dirname "$file")"
    tmp="${file}.tmp.$$"
    {
        printf '%s\n' "$status"
        printf '%s\n' "$percent"
        printf '%s\n' "$message"
    } > "$tmp"
    mv -f "$tmp" "$file"
}

xonotic_discovery_source_has_assets() {
    local source_dir="$1"
    local pattern marker

    if [ ! -d "$source_dir" ]; then
        return 1
    fi

    # Top-level pk3 names only (handful of entries — safe to glob).
    for pattern in "${XONOTIC_DISCOVERY_PK3_PATTERNS[@]}"; do
        if compgen -G "$source_dir/$pattern" >/dev/null; then
            return 0
        fi
    done

    # Existence only — do NOT expand textures/* (tens of thousands of files).
    for marker in "${XONOTIC_DISCOVERY_PK3DIR_MARKERS[@]}"; do
        if [ -d "$source_dir/$marker" ]; then
            return 0
        fi
    done

    return 1
}

# Fast filesystem probe. Prefer direct paths; optional short flatpak(1) probe last.
xonotic_discovery_find_flatpak_xonotic_data() {
    local home="${HOME:-}"
    local flatpak_root dir
    local -a candidates=()

    shopt -s nullglob
    candidates=(
        /var/lib/flatpak/app/org.xonotic.Xonotic/*/files/share/xonotic/data
        "${home}/.local/share/flatpak/app/org.xonotic.Xonotic"/*/files/share/xonotic/data
    )
    shopt -u nullglob

    if [ -n "$home" ]; then
        candidates+=("$home/.var/app/org.xonotic.Xonotic/.xonotic/data")
    fi

    for dir in "${candidates[@]}"; do
        if xonotic_discovery_source_has_assets "$dir"; then
            printf '%s\n' "$dir"
            return 0
        fi
    done

    # Last resort: flatpak info (can hang in sandbox — hard-capped).
    if command -v timeout >/dev/null 2>&1 && command -v flatpak >/dev/null 2>&1; then
        flatpak_root="$(
            timeout "$XONOTIC_FLATPAK_INFO_TIMEOUT_SEC" \
                flatpak info --show-location org.xonotic.Xonotic 2>/dev/null || true
        )"
        if [ -n "$flatpak_root" ] && xonotic_discovery_source_has_assets "$flatpak_root/files/share/xonotic/data"; then
            printf '%s\n' "$flatpak_root/files/share/xonotic/data"
            return 0
        fi
    fi

    return 1
}

# Duplicate packs into our app. Always copy — never hardlink/symlink/use source in place.
xonotic_discovery_copy_assets_from() {
    local source_dir="$1"
    local target_dir="$2"
    local pattern file base tree
    local trees_total=0 trees_done=0 percent
    local rc=0

    mkdir -p "$target_dir"

    # Use "running" so the wizard shows a real determinate bar (not the sweep).
    xonotic_discovery_progress_write running 42 "Copying packages from Flatpak Xonotic..."

    for pattern in "${XONOTIC_DISCOVERY_PK3_PATTERNS[@]}"; do
        for file in "$source_dir"/$pattern; do
            [ -f "$file" ] || continue
            base="$(basename "$file")"
            if [ -e "$target_dir/$base" ]; then
                continue
            fi
            xonotic_discovery_progress_write running 48 "Copying package ${base}..."
            if ! cp -a "$file" "$target_dir/$base"; then
                xonotic_discovery_progress_write error 0 "Failed to copy ${base}."
                return 1
            fi
        done
    done

    for tree in "${XONOTIC_DISCOVERY_PK3DIR_TREES[@]}"; do
        if [ -d "$source_dir/$tree" ]; then
            trees_total=$((trees_total + 1))
        fi
    done

    for tree in "${XONOTIC_DISCOVERY_PK3DIR_TREES[@]}"; do
        if [ ! -d "$source_dir/$tree" ]; then
            continue
        fi
        trees_done=$((trees_done + 1))
        if [ "$trees_total" -gt 0 ]; then
            percent=$((50 + trees_done * 40 / trees_total))
        else
            percent=90
        fi
        case "$tree" in
            *data*) xonotic_discovery_progress_write running "$percent" "Copying core game files from Flatpak..." ;;
            *maps*) xonotic_discovery_progress_write running "$percent" "Copying maps from Flatpak..." ;;
            *music*) xonotic_discovery_progress_write running "$percent" "Copying music from Flatpak..." ;;
            *nexcompat*) xonotic_discovery_progress_write running "$percent" "Copying compatibility pack from Flatpak..." ;;
            *) xonotic_discovery_progress_write running "$percent" "Copying ${tree} from Flatpak..." ;;
        esac
        mkdir -p "$target_dir/$tree"
        rc=0
        if command -v rsync >/dev/null 2>&1; then
            rsync -a --ignore-existing "$source_dir/$tree/" "$target_dir/$tree/" || rc=$?
        else
            cp -a "$source_dir/$tree/." "$target_dir/$tree/" || rc=$?
        fi
        if [ "$rc" -ne 0 ]; then
            xonotic_discovery_progress_write error 0 "Failed to copy ${tree}."
            return 1
        fi
    done
    return 0
}

# Fast path used before the engine starts. Never touches Flatpak or the network.
xonotic_try_discover_assets() {
    local target_dir="$1"

    mkdir -p "$target_dir"

    if declare -F xonotic_assets_are_ready >/dev/null 2>&1 && xonotic_assets_are_ready "$target_dir"; then
        return 0
    fi
    return 1
}

# Background resolver: updates the progress file that drives the fullscreen wizard.
xonotic_resolve_missing_assets() {
    local target_dir="$1"
    local source

    mkdir -p "$target_dir"
    export XONOTIC_ASSET_FETCH_PROGRESS="${XONOTIC_ASSET_FETCH_PROGRESS:-$target_dir/touch/asset-progress.txt}"
    mkdir -p "$(dirname "$XONOTIC_ASSET_FETCH_PROGRESS")"

    xonotic_discovery_progress_write discover 5 "Checking this app's game data folder..."

    if declare -F xonotic_assets_are_ready >/dev/null 2>&1 && xonotic_assets_are_ready "$target_dir"; then
        xonotic_discovery_progress_write "done" 100 "Game data already installed in Xonotic Touch."
        return 0
    fi

    xonotic_discovery_progress_write discover 12 "No complete game data in this app yet."
    xonotic_discovery_progress_write discover 18 "Looking for Flatpak org.xonotic.Xonotic..."

    if source="$(xonotic_discovery_find_flatpak_xonotic_data)"; then
        xonotic_discovery_progress_write running 35 "Found Flatpak Xonotic — copying into this app..."
        if ! xonotic_discovery_copy_assets_from "$source" "$target_dir"; then
            return 1
        fi

        if declare -F xonotic_assets_are_ready >/dev/null 2>&1 && xonotic_assets_are_ready "$target_dir"; then
            xonotic_discovery_progress_write "done" 100 "Imported game data from Flatpak Xonotic."
            return 0
        fi
        xonotic_discovery_progress_write running 92 "Flatpak copy incomplete — downloading missing packs..."
    else
        xonotic_discovery_progress_write discover 25 "Flatpak Xonotic not found on this device."
        xonotic_discovery_progress_write running 28 "Will download game data from the Xonotic servers..."
    fi

    # Menu theme (~7 MB) before multi-GB packs so the wizard can start BGM.
    if declare -F xonotic_ensure_menu_theme_track >/dev/null 2>&1; then
        xonotic_discovery_progress_write running 30 "Downloading menu theme music..."
        xonotic_ensure_menu_theme_track "$target_dir" || true
    fi

    if declare -F xonotic_fetch_game_assets >/dev/null 2>&1; then
        xonotic_fetch_game_assets "$target_dir"
        return $?
    fi

    xonotic_discovery_progress_write error 0 "Asset download helper is missing."
    return 1
}
