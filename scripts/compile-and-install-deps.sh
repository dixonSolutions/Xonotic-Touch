#!/bin/bash
# Install native build dependencies and compile engine + QuakeC.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=lib/xonotic-shlib.sh
. "$ROOT/scripts/lib/xonotic-shlib.sh"

DEPS_ONLY=0
SKIP_DEPS=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Install Debian/Ubuntu build dependencies and compile to build/bin/xonotic.

Options:
  --deps-only     Install packages only; do not compile
  --skip-deps     Compile only; do not install packages
  -h, --help      Show this help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --deps-only)
            DEPS_ONLY=1
            shift
            ;;
        --skip-deps)
            SKIP_DEPS=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            xonotic_usage "Unknown option: $1 (try --help)" 1
            ;;
    esac
done

if [ "$SKIP_DEPS" -eq 0 ]; then
    xonotic_install_native_deps
fi

if [ "$DEPS_ONLY" -eq 1 ]; then
    printf 'Dependencies ready.\n'
    exit 0
fi

xonotic_compile
