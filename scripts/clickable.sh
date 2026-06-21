#!/bin/bash
# Build and install the Ubuntu Touch Click package via Clickable.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=lib/xonotic-shlib.sh
. "$ROOT/scripts/lib/xonotic-shlib.sh"

MODE=""
SETUP=0
BUILD=1
SKIP_BUILD=0
INSTALL_DEVICE=0
DESKTOP_SIM=0
CLEAN_CONTAINER=0
CLICKABLE_ARGS=()

usage() {
    cat <<EOF
Usage: $(basename "$0") (--desktop|--container) [options] [-- clickable args...]

Shortcut for Ubuntu Touch Click packaging with Clickable.

  --desktop       Build on host (Clickable --container-mode)
  --container     Build inside Clickable SDK container (arm64 default)

Options:
  --setup           Run install-clickable.sh for the selected mode first
  --skip-build      Do not compile (install or desktop sim only)
  --install         Install click package on connected device
  --desktop-sim     Run Clickable desktop simulator after build
  --clean-container Prune Podman/buildah artifacts after a container build
  --arch ARCH       Override arch (desktop: amd64, container: arm64)
  -h, --help

Examples:
  $(basename "$0") --container --setup --install
  $(basename "$0") --desktop --desktop-sim
  $(basename "$0") --container --skip-build --install
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --desktop|--container)
            if [ -n "$MODE" ]; then
                xonotic_usage 'Choose only one of --desktop or --container.' 1
            fi
            MODE="${1#--}"
            shift
            ;;
        --setup)
            SETUP=1
            shift
            ;;
        --skip-build)
            SKIP_BUILD=1
            BUILD=0
            shift
            ;;
        --install)
            INSTALL_DEVICE=1
            shift
            ;;
        --desktop-sim)
            DESKTOP_SIM=1
            shift
            ;;
        --clean-container)
            CLEAN_CONTAINER=1
            shift
            ;;
        --arch)
            XONOTIC_CLICKABLE_ARCH="${2:?--arch requires a value}"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            CLICKABLE_ARGS+=("$@")
            break
            ;;
        *)
            CLICKABLE_ARGS+=("$1")
            shift
            ;;
    esac
done

if [ -z "$MODE" ]; then
    usage >&2
    exit 1
fi

if [ "$SETUP" -eq 1 ]; then
    bash "$ROOT/scripts/install-clickable.sh" "--$MODE"
fi

xonotic_ensure_clickable_ready "$MODE"
xonotic_ensure_game_code

mapfile -t MODE_ARGS < <(xonotic_clickable_build_args "$MODE")

if [ "$SKIP_BUILD" -eq 0 ] && [ "$BUILD" -eq 1 ]; then
    export XONOTIC_PACKAGE_BUILD=1
    clickable build "${MODE_ARGS[@]}" "${CLICKABLE_ARGS[@]}"
    if [ "$MODE" = container ] && [ "$CLEAN_CONTAINER" -eq 1 ]; then
        xonotic_clean_container_artifacts
    fi
fi

if [ "$INSTALL_DEVICE" -eq 1 ]; then
    clickable install "${MODE_ARGS[@]}" "${CLICKABLE_ARGS[@]}"
    exit 0
fi

if [ "$DESKTOP_SIM" -eq 1 ]; then
    if [ "$SKIP_BUILD" -eq 1 ]; then
        clickable desktop "${MODE_ARGS[@]}" --skip-build "${CLICKABLE_ARGS[@]}"
    else
        clickable desktop "${MODE_ARGS[@]}" "${CLICKABLE_ARGS[@]}"
    fi
fi
