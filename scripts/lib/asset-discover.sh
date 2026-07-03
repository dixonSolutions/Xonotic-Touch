#!/bin/bash
# Discover Xonotic game assets from existing local installs (Flatpak, Debian, user data).
set -euo pipefail

XONOTIC_DISCOVERY_PK3_PATTERNS=(
    'xonotic-*-data.pk3'
    'xonotic-*-maps.pk3'
    'xonotic-*-music.pk3'
    'xonotic-*-nexcompat.pk3'
)

XONOTIC_DISCOVERY_PK3DIR_MARKERS=(
    'xonotic-data.pk3dir/textures'
    'xonotic-maps.pk3dir/maps'
    'xonotic-music.pk3dir/music'
    'xonotic-nexcompat.pk3dir/textures'
)

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

xonotic_discovery_source_has_assets() {
    local source_dir="$1"
    local pattern

    if [ ! -d "$source_dir" ]; then
        return 1
    fi

    for pattern in "${XONOTIC_DISCOVERY_PK3_PATTERNS[@]}"; do
        if compgen -G "$source_dir/$pattern" >/dev/null; then
            return 0
        fi
    done

    for marker in "${XONOTIC_DISCOVERY_PK3DIR_MARKERS[@]}"; do
        if [ -d "$source_dir/$marker" ] && [ -n "$(ls -A "$source_dir/$marker" 2>/dev/null)" ]; then
            return 0
        fi
    done

    return 1
}

xonotic_discovery_collect_candidate_dirs() {
    local -a candidates=()
    local home="${HOME:-}"
    local path flatpak_root dir

    if [ -n "$home" ]; then
        candidates+=(
            "$home/.xonotic/data"
            "$home/.var/app/org.xonotic.Xonotic/.xonotic/data"
        )
    fi

    if command -v flatpak >/dev/null 2>&1; then
        flatpak_root="$(flatpak info --show-location org.xonotic.Xonotic 2>/dev/null || true)"
        if [ -n "$flatpak_root" ] && [ -d "$flatpak_root/files/share/xonotic/data" ]; then
            candidates+=("$flatpak_root/files/share/xonotic/data")
        fi
    fi

    for path in \
        /var/lib/flatpak/app/org.xonotic.Xonotic/*/files/share/xonotic/data \
        "$home/.local/share/flatpak/app/org.xonotic.Xonotic"/*/files/share/xonotic/data \
        /usr/share/games/xonotic/data \
        /usr/share/xonotic/data \
        /usr/local/share/games/xonotic/data \
        /usr/local/share/xonotic/data; do
        for dir in $path; do
            [ -d "$dir" ] && candidates+=("$dir")
        done
    done

    if command -v dpkg >/dev/null 2>&1 && dpkg -s xonotic >/dev/null 2>&1; then
        while IFS= read -r dir; do
            [ -n "$dir" ] && [ -d "$dir" ] && candidates+=("$dir")
        done <<EOF
$(dpkg -L xonotic 2>/dev/null | while IFS= read -r file; do
    case "$file" in
        *.pk3) dirname "$file" ;;
        */data) printf '%s\n' "$file" ;;
    esac
done | sort -u)
EOF
    fi

    printf '%s\n' "${candidates[@]}" | awk '!seen[$0]++'
}

xonotic_discovery_import_pk3() {
    local source_dir="$1"
    local target_dir="$2"
    local pattern file base

    for pattern in "${XONOTIC_DISCOVERY_PK3_PATTERNS[@]}"; do
        for file in "$source_dir"/$pattern; do
            [ -f "$file" ] || continue
            base="$(basename "$file")"
            if [ -f "$target_dir/$base" ]; then
                continue
            fi
            if ln "$file" "$target_dir/$base" 2>/dev/null; then
                :
            else
                cp -a "$file" "$target_dir/$base"
            fi
        done
    done
}

xonotic_discovery_import_pk3dir_tree() {
    local source_dir="$1"
    local target_dir="$2"
    local tree marker

    for tree in xonotic-data.pk3dir xonotic-maps.pk3dir xonotic-music.pk3dir xonotic-nexcompat.pk3dir; do
        if [ ! -d "$source_dir/$tree" ]; then
            continue
        fi
        mkdir -p "$target_dir/$tree"
        rsync -a --ignore-existing "$source_dir/$tree/" "$target_dir/$tree/"
    done
}

xonotic_try_discover_assets() {
    local target_dir="$1"
    local source label
    local -a sources=()
    local total=0
    local index=0
    local percent

    mkdir -p "$target_dir"

    xonotic_discovery_progress_write discover 0 "Searching for an existing Xonotic install..."

    while IFS= read -r source; do
        [ -n "$source" ] || continue
        if xonotic_discovery_source_has_assets "$source"; then
            sources+=("$source")
        fi
    done < <(xonotic_discovery_collect_candidate_dirs)

    total="${#sources[@]}"
    if [ "$total" -eq 0 ]; then
        xonotic_discovery_progress_write discover 100 "No local Xonotic install found — download required."
        return 1
    fi

    for source in "${sources[@]}"; do
        index=$((index + 1))
        percent=$((index * 100 / total))
        case "$source" in
            *org.xonotic.Xonotic*)
                label="Flatpak Xonotic"
                ;;
            */usr/share/*|*/usr/games/*)
                label="System package (Debian/native)"
                ;;
            */.xonotic/data)
                label="User Xonotic data (~/.xonotic)"
                ;;
            *)
                label="Local Xonotic data"
                ;;
        esac
        xonotic_discovery_progress_write discover "$percent" "Found $label — importing game assets..."
        xonotic_discovery_import_pk3 "$source" "$target_dir"
        xonotic_discovery_import_pk3dir_tree "$source" "$target_dir"
    done

    if declare -F xonotic_assets_are_ready >/dev/null 2>&1 && xonotic_assets_are_ready "$target_dir"; then
        xonotic_discovery_progress_write discover 100 "Reused assets from an existing Xonotic install."
        return 0
    fi

    xonotic_discovery_progress_write discover 100 "Existing installs found but some packs are still missing."
    return 1
}
