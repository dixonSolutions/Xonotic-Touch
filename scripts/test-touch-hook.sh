#!/bin/bash
# Checks the decisions behind the touch fall rescue: where a fall comes down
# (ground, void or a death zone), which anchor the hook is sent to, and when
# the rescue fires, holds and lets go, in engine/darkplaces/touch_rescue.h.
# No engine build, no display. See docs/CONTROLS.md.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
CC="${CC:-cc}"
dir="$ROOT/tests/touch-hook"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

"$CC" -std=c99 -Wall -Wextra -Werror -Wno-unused-function \
    -I"$ROOT/engine/darkplaces" -o "$out/hook_test" "$dir/hook_test.c" -lm
"$out/hook_test"
