#!/bin/bash
# Reclaim disk after an accidental local compile or Clickable run.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"

rm -rf "$ROOT/build" "$ROOT/bin"

if [ -d "$ROOT/engine/darkplaces/build-obj" ]; then
    rm -rf "$ROOT/engine/darkplaces/build-obj"
    rm -f "$ROOT/engine/darkplaces/darkplaces-sdl" \
          "$ROOT/engine/darkplaces/darkplaces-sdl-release"
fi

if [ -f "$ROOT/engine/data/xonotic-data.pk3dir/qcsrc/Makefile" ]; then
    make -C "$ROOT/engine/data/xonotic-data.pk3dir" clean 2>/dev/null || true
fi

echo "Removed local build artifacts (source under engine/ kept)."
