#!/bin/bash
# Checks the decisions behind the touch ledge guard: a run off a platform is
# stopped, slowed or turned along the edge, and bridges, stairs, walls and
# lower floors are left alone, in engine/darkplaces/touch_ledge.h.
# No engine build, no display. See docs/CONTROLS.md.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
CC="${CC:-cc}"
dir="$ROOT/tests/touch-ledge"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

"$CC" -std=c99 -Wall -Wextra -Werror -Wno-unused-function \
    -I"$ROOT/engine/darkplaces" -o "$out/ledge_test" "$dir/ledge_test.c" -lm
"$out/ledge_test"

if command -v clang >/dev/null 2>&1; then
    clang -std=c99 -O1 -g -fsanitize=memory,undefined -fno-sanitize-recover=all -Wno-unused-function \
        -I"$ROOT/engine/darkplaces" -o "$out/ledge_test_msan" "$dir/ledge_test.c" -lm
    "$out/ledge_test_msan" > /dev/null
    echo "touch ledge: clean under MemorySanitizer and UBSan"
fi
