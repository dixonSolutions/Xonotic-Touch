#!/bin/bash
# Invoked from clickable.yaml. Keep this file free of nested shell quoting —
# Clickable runs the YAML build: block through shlex.split.
set -eux

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

export XONOTIC_PACKAGE_BUILD=1

if [ -f click/.ci-name ]; then
    CLICK_NAME="$(tr -d '\n' < click/.ci-name)"
    export CLICK_NAME
fi
if [ -f click/.ci-version ]; then
    CLICK_VERSION="$(tr -d '\n' < click/.ci-version)"
    export CLICK_VERSION
fi
if [ -f click/.ci-framework ]; then
    CLICK_FRAMEWORK="$(tr -d '\n' < click/.ci-framework)"
    export CLICK_FRAMEWORK
fi

export CLICK_NAME="${CLICK_NAME:-xonotictouch.dixonsolutions}"
export CLICK_VERSION="${CLICK_VERSION:-1.1.1}"
export CLICK_FRAMEWORK="${CLICK_FRAMEWORK:-ubuntu-touch-24.04-1.x}"

# Clickable sets ARCH / ARCH_TRIPLET / INSTALL_DIR in the container.
CLICKABLE_ARCH="${ARCH:-}"
export CLICK_ARCH="${CLICKABLE_ARCH}"
export DEST="${INSTALL_DIR:?INSTALL_DIR must be set by Clickable}"

case "${CLICKABLE_ARCH}" in
    arm64)
        export ARCH=arm64
        export ARCH_TRIPLET="${ARCH_TRIPLET:-aarch64-linux-gnu}"
        ;;
    armhf)
        export ARCH=armhf
        export ARCH_TRIPLET="${ARCH_TRIPLET:-arm-linux-gnueabihf}"
        ;;
    amd64)
        # Native host build — clear cross-compile hints for scripts/build.sh.
        unset ARCH ARCH_TRIPLET || true
        ;;
    *)
        echo "Unsupported Clickable ARCH: ${CLICKABLE_ARCH}" >&2
        exit 1
        ;;
esac

bash scripts/fetch-sources.sh code
bash scripts/build.sh
bash scripts/stage-click.sh
export ARCH="${CLICKABLE_ARCH}"
