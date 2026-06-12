#!/bin/bash
# Install Clickable and prerequisites for container or desktop workflows.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=lib/xonotic-shlib.sh
. "$ROOT/scripts/lib/xonotic-shlib.sh"

SETUP_CONTAINER=0
SETUP_DESKTOP=0
CLEAN_CONTAINER=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Install the Clickable CLI and workflow prerequisites.

  --container         Podman/Docker + Clickable SDK image setup
  --desktop           Host toolchain for Clickable --container-mode builds
  --clean-container   Prune Podman/buildah layers left by Clickable builds

With no mode flags, both --container and --desktop setups are installed.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --container)
            SETUP_CONTAINER=1
            shift
            ;;
        --desktop)
            SETUP_DESKTOP=1
            shift
            ;;
        --clean-container)
            CLEAN_CONTAINER=1
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

if [ "$CLEAN_CONTAINER" -eq 1 ]; then
    xonotic_clean_container_artifacts
    exit 0
fi

if [ "$SETUP_CONTAINER" -eq 0 ] && [ "$SETUP_DESKTOP" -eq 0 ]; then
    SETUP_CONTAINER=1
    SETUP_DESKTOP=1
fi

if [ "$SETUP_DESKTOP" -eq 1 ]; then
    xonotic_clickable_setup_desktop
fi

if [ "$SETUP_CONTAINER" -eq 1 ]; then
    xonotic_clickable_setup_container
fi

printf 'Clickable setup complete.\n'
if xonotic_has_clickable; then
    clickable --version 2>/dev/null || true
fi
