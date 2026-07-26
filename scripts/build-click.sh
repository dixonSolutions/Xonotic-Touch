#!/bin/bash
# Build Ubuntu Touch .click package for arm64 / armhf / amd64.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
# shellcheck source=lib/xonotic-shlib.sh
. "$ROOT/scripts/lib/xonotic-shlib.sh"

CLICK_ARCH="${CLICK_ARCH:-arm64}"
CLICK_NAME="${CLICK_NAME:-xonotictouch.dixonsolutions}"
CLICK_VERSION="${CLICK_VERSION:-1.1.1}"
CLICK_FRAMEWORK="${CLICK_FRAMEWORK:-ubuntu-touch-24.04-1.x}"
STAGE_ONLY=0
SKIP_BUILD=0
INSTALL_DEPS=0
OUT_DIR="${OUT_DIR:-$ROOT/build/click-out}"

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Build a slim Ubuntu Touch .click package (assets download on first launch).

Options:
  --arch ARCH       arm64 (default), armhf, or amd64
  --version VER     Click package version (default: ${CLICK_VERSION})
  --framework FW    Click framework (default: ${CLICK_FRAMEWORK})
  --skip-build      Reuse existing build/bin/xonotic
  --stage-only      Stage tree only (for Clickable INSTALL_DIR)
  --install-deps    Install native/cross build deps via apt
  --out DIR         Output directory for .click (default: build/click-out)
  -h, --help        Show this help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --arch) CLICK_ARCH="$2"; shift 2 ;;
        --version) CLICK_VERSION="$2"; shift 2 ;;
        --framework) CLICK_FRAMEWORK="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --stage-only) STAGE_ONLY=1; shift ;;
        --install-deps) INSTALL_DEPS=1; shift ;;
        --out) OUT_DIR="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) xonotic_usage "Unknown option: $1" 1 ;;
    esac
done

normalize_arch() {
    case "$1" in
        arm64|aarch64) printf 'arm64' ;;
        armhf|armv7l|armv7) printf 'armhf' ;;
        amd64|x86_64) printf 'amd64' ;;
        *) xonotic_usage "Unsupported click arch: $1 (use arm64, armhf, amd64)" 1 ;;
    esac
}

CLICK_ARCH="$(normalize_arch "$CLICK_ARCH")"
export CLICK_ARCH CLICK_NAME CLICK_VERSION CLICK_FRAMEWORK

case "$CLICK_ARCH" in
    arm64)
        export ARCH=arm64
        export ARCH_TRIPLET="${ARCH_TRIPLET:-aarch64-linux-gnu}"
        ;;
    armhf)
        export ARCH=armhf
        export ARCH_TRIPLET="${ARCH_TRIPLET:-arm-linux-gnueabihf}"
        ;;
    amd64)
        unset ARCH || true
        unset ARCH_TRIPLET || true
        ;;
esac

if [ "$INSTALL_DEPS" = "1" ]; then
    xonotic_install_native_deps
    if [ "$CLICK_ARCH" = "arm64" ] || [ "$CLICK_ARCH" = "armhf" ]; then
        if [ "$(uname -m)" = "x86_64" ] || [ "$(uname -m)" = "amd64" ]; then
            printf 'Installing cross-compile packages for %s...\n' "$ARCH_TRIPLET"
            xonotic_maybe_sudo dpkg --add-architecture "$CLICK_ARCH" || true
            xonotic_maybe_sudo env DEBIAN_FRONTEND=noninteractive apt-get update -qq
            xonotic_maybe_sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
                "crossbuild-essential-${CLICK_ARCH}" \
                "libsdl2-dev:${CLICK_ARCH}" \
                "libjpeg-dev:${CLICK_ARCH}" \
                "zlib1g-dev:${CLICK_ARCH}" \
                "libxmp-dev:${CLICK_ARCH}" \
                "libgmp-dev:${CLICK_ARCH}" \
                click \
                || printf 'Warning: some cross packages may be missing; build may still work in Clickable containers.\n' >&2
        fi
    elif ! command -v click >/dev/null 2>&1; then
        xonotic_maybe_sudo env DEBIAN_FRONTEND=noninteractive apt-get update -qq
        xonotic_maybe_sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq click || true
    fi
fi

export XONOTIC_PACKAGE_BUILD="${XONOTIC_PACKAGE_BUILD:-1}"

if [ "$SKIP_BUILD" != "1" ]; then
    bash "$ROOT/scripts/fetch-sources.sh" code
    bash "$ROOT/scripts/build.sh"
fi

DEST="${DEST:-${DESTDIR:-$ROOT/build/click}}"
export DEST
bash "$ROOT/scripts/stage-click.sh"

if [ "$STAGE_ONLY" = "1" ]; then
    exit 0
fi

if ! command -v click >/dev/null 2>&1; then
    xonotic_usage 'Missing click tool. Install with: sudo apt install click  (or use clickable)' 1
fi

mkdir -p "$OUT_DIR"
# click build writes <name>_<version>_<arch>.click next to the source tree parent.
click_tmp="$ROOT/build/click-build-tmp"
rm -rf "$click_tmp"
mkdir -p "$click_tmp"
cp -a "$DEST/." "$click_tmp/"
(
    cd "$ROOT/build"
    click build "$click_tmp"
)
shopt -s nullglob
built=( "$ROOT/build/${CLICK_NAME}_${CLICK_VERSION}_${CLICK_ARCH}.click" )
if [ ! -f "${built[0]}" ]; then
    built=( "$ROOT/build"/*.click )
fi
if [ "${#built[@]}" -eq 0 ] || [ ! -f "${built[0]}" ]; then
    xonotic_usage 'click build finished but no .click artifact was found' 1
fi

for f in "${built[@]}"; do
    install -m 644 "$f" "$OUT_DIR/"
    printf 'Built %s\n' "$OUT_DIR/$(basename "$f")"
done
