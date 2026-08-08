#!/bin/bash
# Stage slim game data: logic + configs + the boot assets the menu needs.
#
# Large media (maps, textures, music) downloads on first launch, but the menu
# skin cannot: without it the download wizard would be drawn on top of a wall of
# missing-texture placeholders. scripts/fetch-boot-assets.sh stages that subset.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
DEST="${1:?usage: stage-slim-data.sh <dest-data-dir>}"

SRC="$ROOT/engine/data"
PK3DIR_ASSET_DIRS=(textures models gfx sound particles demos cubemaps maps)

if [ ! -d "$SRC/xonotic-data.pk3dir" ]; then
    echo "Missing $SRC/xonotic-data.pk3dir — run scripts/fetch-sources.sh code first" >&2
    exit 1
fi

mkdir -p "$DEST"

(
    cd "$SRC"
    tar cf - \
        --exclude='xonotic-maps.pk3dir' \
        --exclude='xonotic-music.pk3dir' \
        --exclude='xonotic-nexcompat.pk3dir' \
        .
) | (cd "$DEST" && tar xf -)

for dir in "${PK3DIR_ASSET_DIRS[@]}"; do
    rm -rf "$DEST/xonotic-data.pk3dir/$dir"
done
rm -rf "$DEST/xonotic-data.pk3dir/qcsrc" \
    "$DEST/xonotic-data.pk3dir/.tmp" \
    "$DEST/xonotic-data.pk3dir/.git"

# Single choke point for both packages: stage-flatpak.sh and stage-click.sh get
# boot assets by calling this script. Offline builds can opt out.
if [ "${XONOTIC_SKIP_BOOT_ASSETS:-0}" != "1" ]; then
    bash "$ROOT/scripts/fetch-boot-assets.sh" --dest "$DEST"
fi

if [ -f "$ROOT/touch/xonotic.cfg" ]; then
    install -m 644 "$ROOT/touch/xonotic.cfg" "$DEST/xonotic.cfg"
fi

if [ -d "$ROOT/touch/profiles" ]; then
    mkdir -p "$DEST/touch/profiles"
    cp -a "$ROOT/touch/profiles/." "$DEST/touch/profiles/"
fi

if [ -f "$ROOT/touch/console_palette.txt" ]; then
    mkdir -p "$DEST/touch"
    install -m 644 "$ROOT/touch/console_palette.txt" "$DEST/touch/console_palette.txt"
fi

if [ -d "$ROOT/touch/gfx" ]; then
    mkdir -p "$DEST/xonotic-data.pk3dir/gfx"
    cp -a "$ROOT/touch/gfx/." "$DEST/xonotic-data.pk3dir/gfx/"
fi

echo "Staged slim data to $DEST"
