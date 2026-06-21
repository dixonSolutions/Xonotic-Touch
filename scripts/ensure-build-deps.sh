#!/bin/bash
# Install native build headers inside Clickable SDK / local build environments.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
# shellcheck source=lib/xonotic-shlib.sh
. "$ROOT/scripts/lib/xonotic-shlib.sh"

xonotic_ensure_gmp_headers
