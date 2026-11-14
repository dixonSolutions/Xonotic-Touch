#!/bin/bash
# Checks the pure parts of the in-match touch controls: the top bar's layout
# (no two members intersect at 1920x1200, 2340x1080 and 1280x720, either
# hand, with and without insets). No engine build, no display. See
# docs/CONTROLS.md ("The top bar").
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
