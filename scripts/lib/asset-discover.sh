#!/bin/bash
# Resolve game assets for Xonotic Touch into OUR user data directory only.
#
# Sync (before launch): only ask "is our data already ready?"
# Background (decides wizard content): Flatpak copy → else download.
#
# Order (stop at first success):
#   1. Assets already in our app data → use them (no import, no download)
#   2. Original Flatpak org.xonotic.Xonotic → full copy into our data
#   3. Otherwise download via the in-game wizard
set -euo pipefail

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

XONOTIC_DISCOVERY_PK3DIR_MARKERS=(
    'xonotic-data.pk3dir/textures'
    'xonotic-maps.pk3dir/maps'
    'xonotic-music.pk3dir/music'
    'xonotic-nexcompat.pk3dir/textures'
)

# Never block the UI forever on a wedged flatpak helper.
XONOTIC_FLATPAK_INFO_TIMEOUT_SEC="${XONOTIC_FLATPAK_INFO_TIMEOUT_SEC:-4}"

xonotic_discovery_progress_write() {
    local status="$1"
    local percent="$2"
    local message="$3"
    local file="${XONOTIC_ASSET_FETCH_PROGRESS:-}"

    if [ -z "$file" ]; then
        return 0
    fi
    {
        printf '%s\n' "$status"
        printf '%s\n' "$percent"
        printf '%s\n' "$message"
    } > "$file"
}

# Cheap non-empty dir check — never `ls -A` huge texture trees (that hung detection).
xonotic_discovery_dir_nonempty() {
    local dir="$1"
    local entry

    [ -d "$dir" ] || return 1
    for entry in "$dir"/* "$dir"/.[!.]* "$dir"/..?*; do
        if [ -e "$entry" ]; then
            return 0
        fi
    done
    return 1
}

xonotic_discovery_source_has_assets() {
    local source_dir="$1"
    local pattern marker

    if [ ! -d "$source_dir" ]; then
        return 1
    fi

    for pattern in "${XONOTIC_DISCOVERY_PK3_PATTERNS[@]}"; do
        if compgen -G "$source_dir/$pattern" >/dev/null; then
            return 0
        fi
    done

    for marker in "${XONOTIC_DISCOVERY_PK3DIR_MARKERS[@]}"; do
        if xonotic_discovery_dir_nonempty "$source_dir/$marker"; then
            return 0
        fi
    done

    return 1
}

xonotic_discovery_find_flatpak_xonotic_data() {
    local home="${HOME:-}"
    local flatpak_root dir

    if command -v flatpak >/dev/null 2>&1; then
        flatpak_root="$(
            timeout "$XONOTIC_FLATPAK_INFO_TIMEOUT_SEC" \
                flatpak info --show-location org.xonotic.Xonotic 2>/dev/null || true
        )"
        if [ -n "$flatpak_root" ] && xonotic_discovery_source_has_assets "$flatpak_root/files/share/xonotic/data"; then
            printf '%s\n' "$flatpak_root/files/share/xonotic/data"
            return 0
        fi
    fi

    # Avoid unquoted globs exploding into a multi-minute walk; expand carefully.
    shopt -s nullglob
    for dir in \
        /var/lib/flatpak/app/org.xonotic.Xonotic/*/files/share/xonotic/data \
        "${home}/.local/share/flatpak/app/org.xonotic.Xonotic"/*/files/share/xonotic/data; do
        if xonotic_discovery_source_has_assets "$dir"; then
            shopt -u nullglob
            printf '%s\n' "$dir"
            return 0
        fi
    done
    shopt -u nullglob

    if [ -n "$home" ] && xonotic_discovery_source_has_assets "$home/.var/app/org.xonotic.Xonotic/.xonotic/data"; then
        printf '%s\n' "$home/.var/app/org.xonotic.Xonotic/.xonotic/data"
        return 0
    fi

    return 1
}

# Duplicate packs into our app. Always copy — never hardlink/symlink/use source in place.
xonotic_discovery_copy_assets_from() {
    local source_dir="$1"
    local target_dir="$2"
    local pattern file base tree
    local trees_total=0 trees_done=0 percent

    mkdir -p "$target_dir"

    xonotic_discovery_progress_write discover 45 "Copying packages from Flatpak Xonotic..."

    for pattern in "${XONOTIC_DISCOVERY_PK3_PATTERNS[@]}"; do
        for file in "$source_dir"/$pattern; do
            [ -f "$file" ] || continue
            base="$(basename "$file")"
            if [ -e "$target_dir/$base" ]; then
                continue
            fi
            xonotic_discovery_progress_write discover 50 "Copying ${base}..."
            cp -a "$file" "$target_dir/$base"
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
            percent=$((55 + trees_done * 35 / trees_total))
        else
            percent=90
        fi
        xonotic_discovery_progress_write discover "$percent" "Copying ${tree} into Xonotic Touch..."
        mkdir -p "$target_dir/$tree"
        if command -v rsync >/dev/null 2>&1; then
            rsync -a --ignore-existing "$source_dir/$tree/" "$target_dir/$tree/"
        else
            cp -a "$source_dir/$tree/." "$target_dir/$tree/" 2>/dev/null || true
        fi
    done
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
# Call only when our data is not ready yet.
xonotic_resolve_missing_assets() {
    local target_dir="$1"
    local source

    mkdir -p "$target_dir"
    # Plain name under touch/: the engine cannot open dot-prefixed files.
    export XONOTIC_ASSET_FETCH_PROGRESS="${XONOTIC_ASSET_FETCH_PROGRESS:-$target_dir/touch/asset-progress.txt}"
    mkdir -p "$(dirname "$XONOTIC_ASSET_FETCH_PROGRESS")"

    xonotic_discovery_progress_write discover 5 "Checking Xonotic Touch game data..."

    if declare -F xonotic_assets_are_ready >/dev/null 2>&1 && xonotic_assets_are_ready "$target_dir"; then
        xonotic_discovery_progress_write "done" 100 "Using game assets already in Xonotic Touch."
        return 0
    fi

    xonotic_discovery_progress_write discover 20 "Looking for original Xonotic Flatpak..."

    if source="$(xonotic_discovery_find_flatpak_xonotic_data)"; then
        xonotic_discovery_progress_write discover 40 "Found Flatpak Xonotic — copying into this app..."
        xonotic_discovery_copy_assets_from "$source" "$target_dir"

        if declare -F xonotic_assets_are_ready >/dev/null 2>&1 && xonotic_assets_are_ready "$target_dir"; then
            xonotic_discovery_progress_write "done" 100 "Copied game assets from Flatpak Xonotic."
            return 0
        fi
        xonotic_discovery_progress_write discover 92 "Flatpak copy incomplete — downloading the rest..."
    else
        xonotic_discovery_progress_write discover 30 "No Flatpak Xonotic — starting download..."
    fi

    if declare -F xonotic_fetch_game_assets >/dev/null 2>&1; then
        xonotic_fetch_game_assets "$target_dir"
        return $?
    fi

    xonotic_discovery_progress_write error 0 "Asset download helper is missing."
    return 1
}
