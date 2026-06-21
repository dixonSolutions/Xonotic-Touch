#!/bin/bash
# Stage slim game data (logic + configs, no large binary assets).
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

rsync -a \
    --exclude 'xonotic-maps.pk3dir' \
    --exclude 'xonotic-music.pk3dir' \
    --exclude 'xonotic-nexcompat.pk3dir' \
  "$SRC/" "$DEST/"

for dir in "${PK3DIR_ASSET_DIRS[@]}"; do
    rm -rf "$DEST/xonotic-data.pk3dir/$dir"
done
rm -rf "$DEST/xonotic-data.pk3dir/qcsrc" \
    "$DEST/xonotic-data.pk3dir/.tmp" \
    "$DEST/xonotic-data.pk3dir/.git"

if [ -f "$ROOT/touch/xonotic.cfg" ]; then
    install -m 644 "$ROOT/touch/xonotic.cfg" "$DEST/xonotic.cfg"
fi

if [ -d "$ROOT/touch/profiles" ]; then
    mkdir -p "$DEST/touch/profiles"
    cp -a "$ROOT/touch/profiles/." "$DEST/touch/profiles/"
fi

echo "Staged slim data to $DEST"
