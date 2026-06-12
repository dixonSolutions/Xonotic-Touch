#!/bin/bash
# Clickable build hook (see clickable.json).
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
# shellcheck source=lib/xonotic-shlib.sh
. "$ROOT/scripts/lib/xonotic-shlib.sh"

xonotic_compile
xonotic_stage_click_build
