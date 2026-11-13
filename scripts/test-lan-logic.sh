#!/usr/bin/env bash
# The LAN code's pure parts (touch_lan_logic.h); no engine, no game data.
# The end-to-end LAN test is scripts/test-lan.sh (needs game data).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -I"$root/engine/darkplaces" "$root/tests/lan/lan_test.c" -o "$work/lan-test"
"$work/lan-test"
