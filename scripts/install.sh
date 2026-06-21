#!/bin/bash
# Stage click package from build outputs (slim data; assets fetched on first launch).
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
DEST="${CLICKABLE_DEST:?CLICKABLE_DEST not set}"

ARCH="${ARCH:-arm64}"
CLICK_FRAMEWORK="${CLICK_FRAMEWORK:-ubuntu-sdk-20.04}"
APPARMOR_POLICY="${APPARMOR_POLICY:-16.04}"

BIN="$ROOT/build/bin/xonotic"
test -x "$BIN" || {
    echo "Missing build/bin/xonotic — run build first" >&2
    exit 1
}

mkdir -p "$DEST/bin" "$DEST/data" "$DEST/share/xonotic"

install -m 755 "$BIN" "$DEST/bin/xonotic"
install -m 755 "$ROOT/packaging/start.sh" "$DEST/bin/start.sh"
install -m 755 "$ROOT/touch/screen-calc.sh" "$DEST/share/xonotic/screen-calc.sh"
install -m 755 "$ROOT/scripts/fetch-assets-runtime.sh" "$DEST/share/xonotic/fetch-assets-runtime.sh"
install -m 755 "$ROOT/scripts/sync-bundle-data.sh" "$DEST/share/xonotic/sync-bundle-data.sh"
install -m 644 "$ROOT/scripts/lib/asset-fetch.sh" "$DEST/share/xonotic/asset-fetch.sh"

bash "$ROOT/scripts/stage-slim-data.sh" "$DEST/data"
bash "$ROOT/scripts/stage-click-utils.sh" "$DEST"

install -m 644 "$ROOT/xonotic.apparmor" "$DEST/xonotic.apparmor"

sed -e "s/\$ENV{ARCH}/${ARCH}/g" \
    -e "s/\$ENV{CLICK_FRAMEWORK}/${CLICK_FRAMEWORK}/g" \
    -e "s/\$ENV{APPARMOR_POLICY}/${APPARMOR_POLICY}/g" \
    "$ROOT/manifest.json.in" > "$DEST/manifest.json"

sed "s/@ARCH_TRIPLET@/${ARCH_TRIPLET:-aarch64-linux-gnu}/g" \
    "$ROOT/xonotic.desktop.in" > "$DEST/xonotic.desktop"

echo "Installed slim click package to $DEST"
