#!/bin/bash
# Build the slim data payload that ships inside the APK.
#
# Same tree the Click package installs (game logic, configs, and the boot assets
# the menu needs to draw itself), zipped so the app can unpack it in one pass on
# first launch. Maps, textures and music are not here — they download at runtime,
# exactly as on Ubuntu Touch.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
# shellcheck source=lib/xonotic-shlib.sh
. "$ROOT/scripts/lib/xonotic-shlib.sh"

OUT_ZIP="${1:-$ROOT/android/app/src/main/assets/xonotic-slim-data.zip}"
STAGE="${ANDROID_CACHE_DIR:-$ROOT/build/android}/slim"

export XONOTIC_PACKAGE_BUILD=1

printf 'Compiling QuakeC for the APK payload...\n'
xonotic_compile_qc_only

rm -rf "$STAGE"
mkdir -p "$STAGE/data"
bash "$ROOT/scripts/stage-slim-data.sh" "$STAGE/data"

mkdir -p "$(dirname "$OUT_ZIP")"
rm -f "$OUT_ZIP"
# Store paths relative to $STAGE so entries read as data/... and unpack straight
# into the engine's basedir.
( cd "$STAGE" && zip -q -r -X "$OUT_ZIP" data )

printf 'Staged %s (%s)\n' "$OUT_ZIP" "$(du -h "$OUT_ZIP" | cut -f1)"
