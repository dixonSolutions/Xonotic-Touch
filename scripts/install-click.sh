#!/bin/bash
# Build and/or install the Ubuntu Touch .click package.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
# shellcheck source=lib/xonotic-shlib.sh
. "$ROOT/scripts/lib/xonotic-shlib.sh"

CLICK_ARCH="${CLICK_ARCH:-arm64}"
CLICK_VERSION="${CLICK_VERSION:-1.1.1}"
DO_BUILD=1
DO_INSTALL=1
USE_CLICKABLE=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --arch ARCH         arm64 (default), armhf, or amd64
  --version VER       Package version
  --skip-build        Install an already-built .click from build/click-out
  --build-only        Build without installing
  --clickable         Prefer Clickable (device deploy via adb/ssh)
  -h, --help          Show this help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --arch) CLICK_ARCH="$2"; shift 2 ;;
        --version) CLICK_VERSION="$2"; shift 2 ;;
        --skip-build) DO_BUILD=0; shift ;;
        --build-only) DO_INSTALL=0; shift ;;
        --clickable) USE_CLICKABLE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) xonotic_usage "Unknown option: $1" 1 ;;
    esac
done

export CLICK_ARCH CLICK_VERSION

if [ "$USE_CLICKABLE" = "1" ] && command -v clickable >/dev/null 2>&1; then
    if [ "$DO_BUILD" = "1" ]; then
        clickable build --arch "$CLICK_ARCH" --skip-review
    fi
    if [ "$DO_INSTALL" = "1" ]; then
        clickable install --arch "$CLICK_ARCH"
    fi
    exit 0
fi

if [ "$DO_BUILD" = "1" ]; then
    bash "$ROOT/scripts/build-click.sh" --arch "$CLICK_ARCH" --version "$CLICK_VERSION"
fi

CLICK_FILE="$(find "$ROOT/build/click-out" -name "*_${CLICK_ARCH}.click" -type f | head -1 || true)"
if [ -z "$CLICK_FILE" ]; then
    xonotic_usage "No .click found in build/click-out for arch ${CLICK_ARCH}" 1
fi

if [ "$DO_INSTALL" != "1" ]; then
    printf '%s\n' "$CLICK_FILE"
    exit 0
fi

if command -v pkcon >/dev/null 2>&1; then
    printf 'Installing %s via pkcon...\n' "$CLICK_FILE"
    xonotic_maybe_sudo pkcon install-local --allow-untrusted "$CLICK_FILE"
elif command -v click >/dev/null 2>&1; then
    printf 'Installing %s via click...\n' "$CLICK_FILE"
    click install --force-missing-framework "$CLICK_FILE" || click install "$CLICK_FILE"
else
    printf 'Built package: %s\n' "$CLICK_FILE"
    printf 'Install on an Ubuntu Touch device with:\n' >&2
    printf '  pkcon install-local --allow-untrusted %s\n' "$CLICK_FILE" >&2
    printf 'Or: clickable install\n' >&2
    exit 1
fi
