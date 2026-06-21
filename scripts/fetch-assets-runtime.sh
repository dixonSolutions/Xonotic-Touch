#!/bin/bash
# Download large game assets on first launch (or when cache is incomplete).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$SCRIPT_DIR/asset-fetch.sh" ]; then
    # shellcheck source=asset-fetch.sh
    . "$SCRIPT_DIR/asset-fetch.sh"
else
    ROOT="${ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
    # shellcheck source=lib/asset-fetch.sh
    . "$ROOT/scripts/lib/asset-fetch.sh"
fi

DATA_DIR="${1:-${XONOTIC_TOUCH_DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/xonotic-touch/data}}"

if [ "${XONOTIC_SKIP_ASSET_FETCH:-0}" = "1" ]; then
    exit 0
fi

UTIL_BIN="${XONOTIC_TOUCH_APP_ROOT:-}/bin"
if [ -x "$UTIL_BIN/curl" ]; then
    export PATH="$UTIL_BIN:${PATH}"
fi
if [ -x "$UTIL_BIN/unzip" ]; then
    export PATH="$UTIL_BIN:${PATH}"
fi

xonotic_fetch_game_assets "$DATA_DIR"
