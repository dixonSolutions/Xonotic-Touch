#!/bin/bash
# Checks the pure parts of the in-match touch controls: the top bar's layout
# (no two members intersect at 1920x1200, 2340x1080 and 1280x720, either
# hand, with and without insets), the look (density-independent, nothing lost
# on lift, acceleration) and the floating stick; then that the port's CSQC
# mirrors the constants and that both HUD copies have the same defaults.
# No engine build, no display. See docs/TOUCH_CONTROLS_REVIEW.md.
# The same controls with scripted fingers in a running game:
# scripts/test-touch-controls-live.sh.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
CC="${CC:-cc}"
dir="$ROOT/tests/touch-controls"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

"$CC" -std=c99 -Wall -Wextra -Werror -Wno-unused-function \
    -I"$ROOT/engine/darkplaces" -o "$out/controls_test" "$dir/controls_test.c" -lm
"$out/controls_test"

python3 "$dir/mirror_check.py" "$ROOT/engine/darkplaces/touch_feel.h" "$ROOT/engine/darkplaces/touch_hud.c" \
    "$ROOT/engine/data/xonotic-data.pk3dir/qcsrc/client/touch_feel.qh" \
    "$ROOT/engine/data/xonotic-data.pk3dir/qcsrc/client/touch_init.qc"
