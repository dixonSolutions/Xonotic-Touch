#!/bin/bash
# Sync bundled slim data into the writable user data directory.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
BUNDLE_DIR="${1:?usage: sync-bundle-data.sh <bundle-data-dir> <user-data-dir>}"
USER_DIR="${2:?usage: sync-bundle-data.sh <bundle-data-dir> <user-data-dir>}"

PK3DIR_ASSET_DIRS=(textures models gfx sound particles demos cubemaps maps)

mkdir -p "$USER_DIR"

rsync -a \
    --exclude 'xonotic-maps.pk3dir' \
    --exclude 'xonotic-music.pk3dir' \
    --exclude 'xonotic-nexcompat.pk3dir' \
    "$BUNDLE_DIR/" "$USER_DIR/"

for dir in "${PK3DIR_ASSET_DIRS[@]}"; do
    if [ -d "$USER_DIR/xonotic-data.pk3dir/$dir" ]; then
        continue
    fi
    rm -rf "$USER_DIR/xonotic-data.pk3dir/$dir"
done

if [ -f "$ROOT/touch/xonotic.cfg" ]; then
    install -m 644 "$ROOT/touch/xonotic.cfg" "$USER_DIR/xonotic.cfg"
fi

if [ -d "$ROOT/touch/profiles" ]; then
    mkdir -p "$USER_DIR/touch/profiles"
    rsync -a "$ROOT/touch/profiles/." "$USER_DIR/touch/profiles/"
fi
