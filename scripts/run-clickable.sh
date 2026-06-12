#!/bin/bash
# Build and run the click package via Clickable (desktop sim or SDK container).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=lib/xonotic-shlib.sh
. "$ROOT/scripts/lib/xonotic-shlib.sh"

MODE=""
BUILD=1
SKIP_BUILD=0
INSTALL_DEVICE=0
CLEAN_CONTAINER=0
CLICKABLE_ARGS=()

usage() {
    cat <<EOF
Usage: $(basename "$0") (--desktop|--container) [options] [-- clickable args...]

  --desktop       Build on host (Clickable --container-mode) and run desktop sim
  --container     Build inside Clickable SDK container and run desktop sim

Options:
  --skip-build        Run desktop sim without rebuilding
  --install           Build and install click package to connected device
  --clean-container   Prune Podman/buildah artifacts after a container build
  --arch ARCH         Override arch (desktop: amd64, container: arm64)
  -h, --help

Examples:
  $(basename "$0") --desktop
  $(basename "$0") --container --install
  $(basename "$0") --container --clean-container
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
        --build)
            BUILD=1
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

xonotic_ensure_clickable_ready "$MODE"
xonotic_ensure_game_code

mapfile -t MODE_ARGS < <(xonotic_clickable_build_args "$MODE")

if [ "$SKIP_BUILD" -eq 0 ] && [ "$BUILD" -eq 1 ]; then
    clickable build "${MODE_ARGS[@]}" "${CLICKABLE_ARGS[@]}"
    if [ "$MODE" = container ] && [ "$CLEAN_CONTAINER" -eq 1 ]; then
        xonotic_clean_container_artifacts
    fi
fi

if [ "$INSTALL_DEVICE" -eq 1 ]; then
    clickable install "${MODE_ARGS[@]}" "${CLICKABLE_ARGS[@]}"
    exit 0
fi

if [ "$SKIP_BUILD" -eq 1 ]; then
    clickable desktop "${MODE_ARGS[@]}" --skip-build "${CLICKABLE_ARGS[@]}"
else
    clickable desktop "${MODE_ARGS[@]}" "${CLICKABLE_ARGS[@]}"
fi
