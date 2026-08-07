#!/bin/sh
# Sync bundled slim data into the writable user data directory.
#
# POSIX sh on purpose: packaging/start.sh runs this inside Ubuntu Touch click
# confinement, where bash may not be exec'able (docs/UBUNTU_TOUCH_LAUNCH.md).
set -eu

ROOT="${XONOTIC_TOUCH_APP_ROOT:-${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}}"
BUNDLE_DIR="${1:?usage: sync-bundle-data.sh <bundle-data-dir> <user-data-dir>}"
USER_DIR="${2:?usage: sync-bundle-data.sh <bundle-data-dir> <user-data-dir>}"

PK3DIR_ASSET_DIRS='textures models gfx sound particles demos cubemaps maps'
# Downloaded packs live in the user dir only; never seed them from the bundle.
SKIP_BUNDLE_ENTRIES='xonotic-maps.pk3dir xonotic-music.pk3dir xonotic-nexcompat.pk3dir'

mkdir -p "$USER_DIR"

if [ ! -d "$BUNDLE_DIR" ]; then
    echo "sync-bundle-data: bundle data missing at $BUNDLE_DIR" >&2
    exit 1
fi

is_skipped_entry() {
    for skip in $SKIP_BUNDLE_ENTRIES; do
        if [ "$1" = "$skip" ]; then
            return 0
        fi
    done
    return 1
}

# Per-entry copy instead of `tar --exclude`: busybox tar (shipped in the click)
# has no --exclude, and `cp -a src/.` merges into existing user data.
for entry in "$BUNDLE_DIR"/* "$BUNDLE_DIR"/.[!.]*; do
    [ -e "$entry" ] || continue
    name="${entry##*/}"
    if is_skipped_entry "$name"; then
        continue
    fi
    if [ -d "$entry" ]; then
        mkdir -p "$USER_DIR/$name"
        cp -a "$entry/." "$USER_DIR/$name/"
    else
        cp -a "$entry" "$USER_DIR/$name"
    fi
done

for dir in $PK3DIR_ASSET_DIRS; do
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
    cp -a "$ROOT/touch/profiles/." "$USER_DIR/touch/profiles/"
fi

if [ -f "$ROOT/touch/console_palette.txt" ]; then
    mkdir -p "$USER_DIR/touch"
    install -m 644 "$ROOT/touch/console_palette.txt" "$USER_DIR/touch/console_palette.txt"
fi

if [ -d "$ROOT/touch/gfx" ]; then
    mkdir -p "$USER_DIR/xonotic-data.pk3dir/gfx"
    cp -a "$ROOT/touch/gfx/." "$USER_DIR/xonotic-data.pk3dir/gfx/"
fi
