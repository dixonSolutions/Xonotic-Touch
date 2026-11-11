#!/bin/bash
# Checks the decisions behind touch auto dodge: the damage an incoming
# projectile does if the player carries on, and the escape chosen (never into
# a wall or off a ledge), in engine/darkplaces/touch_dodge.h.
# No engine build, no display. See docs/CONTROLS.md.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
CC="${CC:-cc}"
dir="$ROOT/tests/touch-dodge"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

"$CC" -std=c99 -Wall -Wextra -Werror -Wno-unused-function \
    -I"$ROOT/engine/darkplaces" -o "$out/dodge_test" "$dir/dodge_test.c" -lm
"$out/dodge_test"

# Again under clang's MemorySanitizer and UBSan where clang is there: a field
# of td_move_t left unset read stack garbage, which changed the modelled dash
# from one compiler to the next with no warning.
if command -v clang >/dev/null 2>&1; then
    clang -std=c99 -O1 -g -fsanitize=memory,undefined -fno-sanitize-recover=all -Wno-unused-function \
        -I"$ROOT/engine/darkplaces" -o "$out/dodge_test_msan" "$dir/dodge_test.c" -lm
    "$out/dodge_test_msan" > /dev/null
    echo "touch dodge: clean under MemorySanitizer and UBSan"
fi
