#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -I"$root/engine/darkplaces" "$root/tests/join-world/world_test.c" -o "$work/world-test"
"$work/world-test"
