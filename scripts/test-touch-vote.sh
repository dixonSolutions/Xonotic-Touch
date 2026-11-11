#!/bin/bash
# Checks how the engine reads a server's vote prints back into vote events
# (engine/darkplaces/touch_vote_parse.h), the source of the touch vote chip on
# servers whose CSQC vote state cannot be read. No engine build, no display.
# See docs/CONTROLS.md ("Votes").
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
CC="${CC:-cc}"
dir="$ROOT/tests/touch-vote"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

"$CC" -std=c99 -Wall -Wextra -Werror -Wno-unused-function \
    -I"$ROOT/engine/darkplaces" -o "$out/vote_parse_test" "$dir/vote_parse_test.c"
"$out/vote_parse_test"
