#!/bin/bash
# Checks what the engine reads out of /proc/bus/input/devices (touchscreen,
# keyboard, mouse or touchpad, tablet-mode switch, screen pen) against
# recorded listings. No engine build, no display. See docs/TOUCH_DETECTION.md.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
CC="${CC:-cc}"
dir="$ROOT/tests/input-presence"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

"$CC" -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Wno-unused-function \
    -I"$ROOT/engine/darkplaces" -o "$out/inputscan_test" "$dir/inputscan_test.c"
"$out/inputscan_test" "$dir/fixtures"
