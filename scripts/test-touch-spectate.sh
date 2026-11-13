#!/bin/bash
# Checks what the touch controls do while spectating
# (engine/darkplaces/touch_spectate.h): the spectator bar, that nothing a
# spectator touches sends +jump, and the join refusal lock. No engine build, no display.
# See docs/CONTROLS.md (spectating).
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
CC="${CC:-cc}"
dir="$ROOT/tests/touch-spectate"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

"$CC" -std=c99 -Wall -Wextra -Werror -Wno-unused-function \
    -I"$ROOT/engine/darkplaces" -o "$out/spectate_test" "$dir/spectate_test.c"
"$out/spectate_test"
