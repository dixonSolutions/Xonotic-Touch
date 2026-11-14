#!/bin/bash
# Renders one fixture to a PNG on a private Xvfb, in the GNOME runtime the Flatpak uses.
#   scripts/shot.sh <fixture> <WxH> <dark|light> <out.png> [extra args]
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
FX="$1"; SIZE="$2"; SCHEME="$3"; OUT="$4"; shift 4
BIN="${GODNOTIC_LOADER_BIN:-$HERE/target/release/godnotic-loader}"
# map previews: GODNOTIC_DATA_DIRS, else the app's own data folders and an installed Xonotic (data-dirs.sh)
. "$HERE/scripts/data-dirs.sh"
exec "$HERE/scripts/run-headless.sh" --size 2000x1300 -- flatpak run --filesystem=home --filesystem=host \
    "${GN_DATA_FS[@]}" --env=GODNOTIC_DATA_DIRS="$GN_DATA_DIRS" --command="$BIN" org.gnome.Platform//48 \
    --fixture="$FX" --fixtures-dir="$HERE/../tests/uibridge/fixtures" --size="$SIZE" "--$SCHEME" \
    --snapshot="$OUT" --snapshot-delay="${DELAY:-1500}" "$@"
