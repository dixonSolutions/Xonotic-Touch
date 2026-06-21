#!/bin/bash
# Compile engine + QuakeC for Flatpak and local native runs.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
# shellcheck source=lib/xonotic-shlib.sh
. "$ROOT/scripts/lib/xonotic-shlib.sh"

export XONOTIC_PACKAGE_BUILD="${XONOTIC_PACKAGE_BUILD:-1}"

xonotic_compile
