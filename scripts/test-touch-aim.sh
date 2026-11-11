#!/bin/bash
# Checks the weapon decisions behind touch auto-shoot and aim assist: lead and
# lob maths, fire window, primary or secondary per weapon, weapon choice per
# distance, Devastator detonation and the weapon table, in
# engine/darkplaces/touch_weapons.h. No engine build, no display. See
# docs/CONTROLS.md.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
CC="${CC:-cc}"
dir="$ROOT/tests/touch-aim"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

"$CC" -std=c99 -Wall -Wextra -Werror -Wno-unused-function \
    -I"$ROOT/engine/darkplaces" -o "$out/aim_test" "$dir/aim_test.c" -lm
"$out/aim_test"
