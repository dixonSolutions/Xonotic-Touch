#!/bin/bash
# Populate engine/ with upstream source (no compile — saves disk and CPU).
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$ROOT"

MODE="${1:-code}"
XONOTIC_URL="${XONOTIC_URL:-https://gitlab.com/xonotic/xonotic.git}"
DARKPLACES_URL="${DARKPLACES_URL:-https://gitlab.com/xonotic/darkplaces.git}"
GMQCC_URL="${GMQCC_URL:-https://gitlab.com/xonotic/gmqcc.git}"
DATA_URL="${DATA_URL:-https://gitlab.com/xonotic/xonotic-data.pk3dir.git}"
BLIND_ID_URL="${BLIND_ID_URL:-https://gitlab.com/xonotic/d0_blind_id.git}"

clone_repo() {
    local dest="$1"
    local url="$2"
    local marker="${3:-Makefile}"
    if [ -e "$dest/$marker" ] || [ -d "$dest/$marker" ]; then
        echo "  present: $dest"
        return 0
    fi
    echo "  cloning: $url -> $dest"
    rm -rf "$dest"
    git clone --depth 1 "$url" "$dest"
}

fetch_pk3dir_assets() {
    local pk3dir="$ROOT/engine/data/xonotic-data.pk3dir"
    local asset_dirs=(textures models gfx sound particles demos cubemaps maps)
    local missing=0

    for dir in "${asset_dirs[@]}"; do
        if [ ! -d "$pk3dir/$dir" ] || [ -z "$(ls -A "$pk3dir/$dir" 2>/dev/null)" ]; then
            missing=1
            break
        fi
    done

    if [ "$missing" -eq 0 ]; then
        echo "  game assets already present under $pk3dir"
        return 0
    fi

    if [ ! -f "$pk3dir/Makefile" ] && [ ! -d "$pk3dir/qcsrc" ]; then
        clone_repo "$pk3dir" "$DATA_URL" "qcsrc"
    fi

    echo "  fetching large pk3dir assets (textures, models, …) via sparse clone..."
    local tmp
    tmp="$(mktemp -d)"
    git clone --depth 1 --filter=blob:none --sparse "$DATA_URL" "$tmp/data"
    (
        cd "$tmp/data"
        git sparse-checkout set "${asset_dirs[@]}"
    )
    for dir in "${asset_dirs[@]}"; do
        if [ -d "$tmp/data/$dir" ]; then
            mkdir -p "$pk3dir/$dir"
            rsync -a "$tmp/data/$dir/" "$pk3dir/$dir/"
            echo "  synced: $pk3dir/$dir"
        fi
    done
    rm -rf "$tmp"
}

if [ -f "$ROOT/engine/all" ]; then
    echo "engine/ superproject present"
elif [ -d "$ROOT/engine/.git" ]; then
    echo "engine/ superproject already initialized"
elif [ -f "$ROOT/engine/darkplaces/makefile.inc" ]; then
    echo "engine/ source present (vendored in monorepo)"
else
    echo "Cloning Xonotic superproject into engine/..."
    rm -rf "$ROOT/engine"
    git clone --depth 1 "$XONOTIC_URL" "$ROOT/engine"
fi

cd "$ROOT/engine"

case "$MODE" in
    minimal)
        clone_repo darkplaces "$DARKPLACES_URL" "makefile.inc"
        echo "Minimal fetch: engine/darkplaces only (engine C work)"
        ;;
    code)
        clone_repo darkplaces "$DARKPLACES_URL" "makefile.inc"
        clone_repo gmqcc "$GMQCC_URL" "Makefile"
        clone_repo data/xonotic-data.pk3dir "$DATA_URL" "qcsrc"
        clone_repo d0_blind_id "$BLIND_ID_URL" "configure.ac"
        echo "Code fetch: darkplaces + gmqcc + xonotic-data.pk3dir + d0_blind_id"
        echo "  UI/controls: engine/data/xonotic-data.pk3dir/qcsrc/{menu,client,common}/"
        echo "  Binary assets gitignored — run: $0 assets (or full for entire game)"
        ;;
    assets)
        clone_repo darkplaces "$DARKPLACES_URL" "makefile.inc"
        clone_repo gmqcc "$GMQCC_URL" "Makefile"
        clone_repo data/xonotic-data.pk3dir "$DATA_URL" "qcsrc"
        clone_repo d0_blind_id "$BLIND_ID_URL" "configure.ac"
        fetch_pk3dir_assets
        echo "Assets fetch: textures/models/gfx/sound/… under xonotic-data.pk3dir"
        ;;
    full)
        # Fetch all game data packs individually (no superproject required).
        clone_repo darkplaces "$DARKPLACES_URL" "makefile.inc"
        clone_repo gmqcc "$GMQCC_URL" "Makefile"
        clone_repo data/xonotic-data.pk3dir "$DATA_URL" "qcsrc"
        clone_repo d0_blind_id "$BLIND_ID_URL" "configure.ac"
        fetch_pk3dir_assets
        # Additional data packs from the Xonotic release
        MAPS_URL="${MAPS_URL:-https://gitlab.com/xonotic/xonotic-maps.pk3dir.git}"
        MUSIC_URL="${MUSIC_URL:-https://gitlab.com/xonotic/xonotic-music.pk3dir.git}"
        NEXCOMPAT_URL="${NEXCOMPAT_URL:-https://gitlab.com/xonotic/xonotic-nexcompat.pk3dir.git}"
        clone_repo data/xonotic-maps.pk3dir "$MAPS_URL" "maps"
        clone_repo data/xonotic-music.pk3dir "$MUSIC_URL" "music"
        clone_repo data/xonotic-nexcompat.pk3dir "$NEXCOMPAT_URL" "textures"
        echo "Full fetch complete: all data packs present under engine/data/"
        ;;
    *)
        echo "Usage: $0 [minimal|code|assets|full]" >&2
        exit 1
        ;;
esac

if [ -f "$ROOT/touch/xonotic.cfg" ]; then
    mkdir -p "$ROOT/data"
    cp -f "$ROOT/touch/xonotic.cfg" "$ROOT/data/xonotic.cfg"
    mkdir -p "$ROOT/engine/data"
    cp -f "$ROOT/touch/xonotic.cfg" "$ROOT/engine/data/xonotic.cfg"
fi

echo "Source ready under engine/ (touch changes integrated in-tree; large assets download at launch in packages)."
