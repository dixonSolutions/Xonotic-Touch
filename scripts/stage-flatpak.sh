#!/bin/bash
# Stage Flatpak install tree (used by flatpak-builder; slim data, assets on launch).
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
DEST="${DESTDIR:-/app}"

BIN="$ROOT/build/bin/xonotic"
test -x "$BIN" || {
    echo "Missing build/bin/xonotic — run build first" >&2
    exit 1
}

mkdir -p "$DEST/bin" "$DEST/data" "$DEST/share/xonotic" \
    "$DEST/share/applications" \
    "$DEST/share/metainfo" \
    "$DEST/share/icons/hicolor/128x128/apps" \
    "$DEST/share/icons/hicolor/256x256/apps" \
    "$DEST/share/icons/hicolor/512x512/apps"

install -m 755 "$BIN" "$DEST/bin/xonotic"
install -m 755 "$ROOT/packaging/start.sh" "$DEST/bin/start.sh"
install -m 755 "$ROOT/touch/screen-calc.sh" "$DEST/share/xonotic/screen-calc.sh"
install -m 755 "$ROOT/scripts/fetch-assets-runtime.sh" "$DEST/share/xonotic/fetch-assets-runtime.sh"
install -m 755 "$ROOT/scripts/sync-bundle-data.sh" "$DEST/share/xonotic/sync-bundle-data.sh"
install -m 644 "$ROOT/scripts/lib/asset-fetch.sh" "$DEST/share/xonotic/asset-fetch.sh"

bash "$ROOT/scripts/stage-slim-data.sh" "$DEST/data"

install -m 644 "$ROOT/flatpak/io.github.dixonSolutions.XonoticTouch.desktop" \
    "$DEST/share/applications/io.github.dixonSolutions.XonoticTouch.desktop"
install -m 644 "$ROOT/flatpak/io.github.dixonSolutions.XonoticTouch.metainfo.xml" \
    "$DEST/share/metainfo/io.github.dixonSolutions.XonoticTouch.metainfo.xml"

ICON_SRC="$ROOT/engine/misc/logos/icons_png"
install -m 644 "$ICON_SRC/xonotic_128.png" \
    "$DEST/share/icons/hicolor/128x128/apps/io.github.dixonSolutions.XonoticTouch.png"
install -m 644 "$ICON_SRC/xonotic_256.png" \
    "$DEST/share/icons/hicolor/256x256/apps/io.github.dixonSolutions.XonoticTouch.png"
install -m 644 "$ICON_SRC/xonotic_512.png" \
    "$DEST/share/icons/hicolor/512x512/apps/io.github.dixonSolutions.XonoticTouch.png"

echo "Staged Flatpak tree to $DEST"
