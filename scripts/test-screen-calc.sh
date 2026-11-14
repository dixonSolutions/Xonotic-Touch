#!/bin/bash
# Dry-run screen calculation (no compile, no engine). For maintainers.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
OUT="${1:-/tmp/xonotic-screen.layout.cfg}"

# shellcheck source=/dev/null
. "$ROOT/touch/screen-calc.sh"

xonotic_screen_calc "$OUT"

echo "vid: ${XONOTIC_VID_WIDTH}x${XONOTIC_VID_HEIGHT}"
echo "dpi: ${GODNOTIC_XDPI}x${GODNOTIC_YDPI} density ${GODNOTIC_DENSITY}"
echo "orientation: ${XONOTIC_ORIENTATION:-unknown}"
echo "layout: $OUT"
cat "$OUT"
