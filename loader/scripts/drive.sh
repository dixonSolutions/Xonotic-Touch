#!/bin/bash
# Runs a UI script (see src/script.rs) against the fixtures on a private Xvfb.
#   scripts/drive.sh <script> <outdir> [WxH] [dark|light]
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT="$1"; OUT="$2"; SIZE="${3:-1280x800}"; SCHEME="${4:-dark}"
mkdir -p "$OUT"
TMP="$OUT/.script"
sed "s#OUT/#$OUT/#g" "$SCRIPT" > "$TMP"
BIN="${GODNOTIC_LOADER_BIN:-$HERE/target/release/godnotic-loader}"
# map previews: GODNOTIC_DATA_DIRS, else the app's own data folders and an installed Xonotic (data-dirs.sh)
. "$HERE/scripts/data-dirs.sh"
exec "$HERE/scripts/run-headless.sh" --size 2000x1300 -- flatpak run --filesystem=home \
    "${GN_DATA_FS[@]}" --env=GODNOTIC_DATA_DIRS="$GN_DATA_DIRS" --command="$BIN" org.gnome.Platform//48 \
    --fixture="${FIXTURE:-home}" --fixtures-dir="$HERE/../tests/uibridge/fixtures" --size="$SIZE" "--$SCHEME" --script="$TMP"
