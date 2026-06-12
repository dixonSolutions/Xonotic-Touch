#!/bin/bash
# Compile (if needed) and run Xonotic natively on Linux — no Clickable container.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=lib/xonotic-shlib.sh
. "$ROOT/scripts/lib/xonotic-shlib.sh"

BUILD=0
PASS_ARGS=()

usage() {
    cat <<EOF
Usage: $(basename "$0") [--build] [xonotic args...]

Run the game on native Linux. Mouse emulates touch when vid_touchscreen=1.

  --build   Install deps and compile before launch (also when binary is missing)
  -h, --help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --build)
            BUILD=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            PASS_ARGS+=("$@")
            break
            ;;
        *)
            PASS_ARGS+=("$1")
            shift
            ;;
    esac
done

if [ "$BUILD" -eq 1 ] || [ ! -x "$(xonotic_bin)" ]; then
    xonotic_install_native_deps
    xonotic_compile
elif ! xonotic_has_native_build_deps; then
    printf 'Build dependencies missing — installing...\n'
    xonotic_install_native_deps
fi

xonotic_run_native "${PASS_ARGS[@]}"
