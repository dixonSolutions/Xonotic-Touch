#!/bin/bash
# Checks when the engine HUD shows the ALT (secondary fire) button on servers
# that do not say: the stock-balance fallback, availability modes and fade in
# engine/darkplaces/touch_altfire.h. No engine build, no display. See
# docs/CONTROLS.md.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
CC="${CC:-cc}"
dir="$ROOT/tests/touch-altfire"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

"$CC" -std=c99 -Wall -Wextra -Werror -Wno-unused-function \
    -I"$ROOT/engine/darkplaces" -o "$out/altfire_test" "$dir/altfire_test.c" -lm
"$out/altfire_test"
