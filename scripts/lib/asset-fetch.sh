#!/bin/bash
# Shared game asset download helpers (build-time and first-run).
set -euo pipefail

XONOTIC_DATA_PK3DIR_ASSET_DIRS=(textures models gfx sound particles demos cubemaps maps)

XONOTIC_AUTOBUILD_URL="${XONOTIC_AUTOBUILD_URL:-https://beta.xonotic.org/autobuild}"
XONOTIC_AUTOBUILD_USER="${XONOTIC_AUTOBUILD_USER:-xonotic}"
XONOTIC_AUTOBUILD_PASS="${XONOTIC_AUTOBUILD_PASS:-g-23}"

xonotic_asset_dirs_missing() {
    local data_dir="$1"
    local pk3dir="$data_dir/xonotic-data.pk3dir"
    local dir

    if compgen -G "$data_dir/xonotic-*-data.pk3" >/dev/null; then
        return 1
    fi

    for dir in "${XONOTIC_DATA_PK3DIR_ASSET_DIRS[@]}"; do
        if [ ! -d "$pk3dir/$dir" ] || [ -z "$(ls -A "$pk3dir/$dir" 2>/dev/null)" ]; then
            return 0
        fi
    done
    return 1
}

xonotic_maps_assets_missing() {
    local data_dir="$1"

    if compgen -G "$data_dir/xonotic-*-maps.pk3" >/dev/null; then
        return 1
    fi
    if [ -d "$data_dir/xonotic-maps.pk3dir/maps" ] \
        && [ -n "$(ls -A "$data_dir/xonotic-maps.pk3dir/maps" 2>/dev/null)" ]; then
        return 1
    fi
    return 0
}

xonotic_music_assets_missing() {
    local data_dir="$1"

    if compgen -G "$data_dir/xonotic-*-music.pk3" >/dev/null; then
        return 1
    fi
    if [ -d "$data_dir/xonotic-music.pk3dir/music" ] \
        && [ -n "$(ls -A "$data_dir/xonotic-music.pk3dir/music" 2>/dev/null)" ]; then
        return 1
    fi
    return 0
}

xonotic_nexcompat_assets_missing() {
    local data_dir="$1"

    if compgen -G "$data_dir/xonotic-*-nexcompat.pk3" >/dev/null; then
        return 1
    fi
    if [ -d "$data_dir/xonotic-nexcompat.pk3dir/textures" ] \
        && [ -n "$(ls -A "$data_dir/xonotic-nexcompat.pk3dir/textures" 2>/dev/null)" ]; then
        return 1
    fi
    return 0
}

xonotic_assets_need_fetch() {
    local data_dir="$1"

    xonotic_asset_dirs_missing "$data_dir" \
        || xonotic_maps_assets_missing "$data_dir" \
        || xonotic_music_assets_missing "$data_dir" \
        || xonotic_nexcompat_assets_missing "$data_dir"
}

xonotic_fetch_pk3dir_sparse() {
    local data_dir="$1"
    local data_url="${DATA_URL:-https://gitlab.com/xonotic/xonotic-data.pk3dir.git}"
    local pk3dir="$data_dir/xonotic-data.pk3dir"
    local tmp
    local dir

    if ! command -v git >/dev/null 2>&1; then
        return 1
    fi

    tmp="$(mktemp -d)"
    git clone --depth 1 --filter=blob:none --sparse "$data_url" "$tmp/data"
    (
        cd "$tmp/data"
        git sparse-checkout set "${XONOTIC_DATA_PK3DIR_ASSET_DIRS[@]}"
    )
    mkdir -p "$pk3dir"
    for dir in "${XONOTIC_DATA_PK3DIR_ASSET_DIRS[@]}"; do
        if [ -d "$tmp/data/$dir" ]; then
            mkdir -p "$pk3dir/$dir"
            rsync -a "$tmp/data/$dir/" "$pk3dir/$dir/"
        fi
    done
    rm -rf "$tmp"
}

xonotic_clone_pk3dir_repo() {
    local data_dir="$1"
    local dest_name="$2"
    local url="$3"
    local marker="$4"

    if ! command -v git >/dev/null 2>&1; then
        return 1
    fi

    local dest="$data_dir/$dest_name"
    if [ -e "$dest/$marker" ] || [ -d "$dest/$marker" ]; then
        return 0
    fi
    rm -rf "$dest"
    git clone --depth 1 "$url" "$dest"
}

xonotic_fetch_git_assets() {
    local data_dir="$1"

    if ! command -v git >/dev/null 2>&1; then
        return 1
    fi

    xonotic_fetch_pk3dir_sparse "$data_dir" || return 1

    if xonotic_maps_assets_missing "$data_dir"; then
        xonotic_clone_pk3dir_repo "$data_dir" xonotic-maps.pk3dir \
            "${MAPS_URL:-https://gitlab.com/xonotic/xonotic-maps.pk3dir.git}" maps || return 1
    fi
    if xonotic_music_assets_missing "$data_dir"; then
        xonotic_clone_pk3dir_repo "$data_dir" xonotic-music.pk3dir \
            "${MUSIC_URL:-https://gitlab.com/xonotic/xonotic-music.pk3dir.git}" music || return 1
    fi
    if xonotic_nexcompat_assets_missing "$data_dir"; then
        xonotic_clone_pk3dir_repo "$data_dir" xonotic-nexcompat.pk3dir \
            "${NEXCOMPAT_URL:-https://gitlab.com/xonotic/xonotic-nexcompat.pk3dir.git}" textures || return 1
    fi
}

xonotic_download_autobuild_zip() {
    local zip_path="$1"
    local zip_name="$2"

    if ! command -v curl >/dev/null 2>&1; then
        echo "xonotic: curl required to download game assets" >&2
        return 1
    fi

    curl -fL --user "${XONOTIC_AUTOBUILD_USER}:${XONOTIC_AUTOBUILD_PASS}" \
        -o "$zip_path" "${XONOTIC_AUTOBUILD_URL}/${zip_name}"
}

xonotic_extract_autobuild_pk3() {
    local zip_path="$1"
    local data_dir="$2"
    local extract_dir="$3"

    if ! command -v unzip >/dev/null 2>&1; then
        echo "xonotic: unzip required to extract game assets" >&2
        return 1
    fi

    mkdir -p "$extract_dir" "$data_dir"
    unzip -q "$zip_path" "Xonotic/data/*.pk3" -d "$extract_dir"
    mv "$extract_dir/Xonotic/data/"*.pk3 "$data_dir/"
    rm -rf "$extract_dir/Xonotic"
}

xonotic_fetch_autobuild_assets() {
    local data_dir="$1"
    local tmp
    local zip_path
    local extract_dir

    tmp="$data_dir/.fetch-tmp"
    mkdir -p "$tmp"
    trap 'rm -rf "$tmp"' RETURN

    zip_path="$tmp/xonotic.zip"
    extract_dir="$tmp/extract"

    if xonotic_asset_dirs_missing "$data_dir"; then
        xonotic_download_autobuild_zip "$zip_path" "Xonotic-latest.zip"
        xonotic_extract_autobuild_pk3 "$zip_path" "$data_dir" "$extract_dir"
        rm -f "$zip_path"
    fi

    if xonotic_maps_assets_missing "$data_dir"; then
        xonotic_download_autobuild_zip "$zip_path" "Xonotic-latest-mappingsupport.zip"
        xonotic_extract_autobuild_pk3 "$zip_path" "$data_dir" "$extract_dir"
        rm -f "$zip_path"
    fi

    if xonotic_music_assets_missing "$data_dir"; then
        xonotic_download_autobuild_zip "$zip_path" "Xonotic-latest-high.zip"
        xonotic_extract_autobuild_pk3 "$zip_path" "$data_dir" "$extract_dir"
        rm -f "$zip_path"
    fi

    if xonotic_nexcompat_assets_missing "$data_dir"; then
        xonotic_download_autobuild_zip "$zip_path" "Xonotic-latest.zip"
        xonotic_extract_autobuild_pk3 "$zip_path" "$data_dir" "$extract_dir"
        rm -f "$zip_path"
    fi
}

xonotic_fetch_game_assets() {
    local data_dir="$1"

    mkdir -p "$data_dir"

    if ! xonotic_assets_need_fetch "$data_dir"; then
        return 0
    fi

    echo "xonotic-touch: downloading game assets (first launch may take several minutes)..."
    if xonotic_fetch_git_assets "$data_dir"; then
        return 0
    fi

    xonotic_fetch_autobuild_assets "$data_dir"
}
