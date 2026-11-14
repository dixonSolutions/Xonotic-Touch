#!/bin/bash
# Checks the in-match HUD modes (engine/darkplaces/touch_hudmode.h): the mode
# derived from the client state, the controls each mode has (MENU only at the
# end of the match and on the next-map vote), the MENU sheet's action list,
# and the join dialog (when it opens, what its rows send). Then that the port's
# CSQC mirror (qcsrc/client/touch_mode.qh) uses the same numbers. No engine
# build, no display. See docs/CONTROLS.md ("HUD modes").
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
CC="${CC:-cc}"
dir="$ROOT/tests/touch-hudmode"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

"$CC" -std=c99 -Wall -Wextra -Werror -Wno-unused-function \
    -I"$ROOT/engine/darkplaces" -o "$out/hudmode_test" "$dir/hudmode_test.c"
"$out/hudmode_test"

python3 "$dir/mirror_check.py" "$ROOT/engine/darkplaces/touch_hudmode.h" \
    "$ROOT/engine/data/xonotic-data.pk3dir/qcsrc/client/touch_mode.qh"
