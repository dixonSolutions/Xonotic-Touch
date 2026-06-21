#!/bin/bash
# Install Flatpak tooling and build/install Xonotic Touch locally.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=lib/xonotic-shlib.sh
. "$ROOT/scripts/lib/xonotic-shlib.sh"

FLATPAK_APP_ID="${FLATPAK_APP_ID:-io.github.dixonSolutions.XonoticTouch}"
FLATPAK_REMOTE_NAME="${FLATPAK_REMOTE_NAME:-xonotic-touch}"
FLATPAK_REMOTE_URL="${FLATPAK_REMOTE_URL:-https://dixonSolutions.github.io/Xonotic-Ubuntu-Touch-App/flatpak}"
FLATPAK_MANIFEST="${FLATPAK_MANIFEST:-flatpak/io.github.dixonSolutions.XonoticTouch.yml}"
FLATPAK_BUILD_DIR="${FLATPAK_BUILD_DIR:-build-flatpak}"

DEPS_ONLY=0
SKIP_DEPS=0
SKIP_BUILD=0
FROM_REMOTE=0
ADD_REMOTE=0
RUN_APP=0
CLEAN=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Install Flatpak build tools, then build and install Xonotic Touch for the local user.

Options:
  --deps-only       Install flatpak + flatpak-builder only
  --skip-deps       Skip tool installation
  --skip-build      Install from remote or existing build dir only
  --from-remote     Install from the public GitHub Pages remote (no local build)
  --add-remote      Add the public remote before installing
  --run             Run the app after install
  --clean           Remove $FLATPAK_BUILD_DIR before building
  -h, --help        Show this help

Examples:
  $(basename "$0")                    # build and install locally
  $(basename "$0") --from-remote    # install latest CI build from remote
  $(basename "$0") --add-remote --from-remote --run
  $(basename "$0") --deps-only
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
        --skip-build)
            SKIP_BUILD=1
            shift
            ;;
        --from-remote)
            FROM_REMOTE=1
            SKIP_BUILD=1
            shift
            ;;
        --add-remote)
            ADD_REMOTE=1
            shift
            ;;
        --run)
            RUN_APP=1
            shift
            ;;
        --clean)
            CLEAN=1
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

xonotic_install_flatpak_tools() {
    if command -v flatpak >/dev/null 2>&1 && command -v flatpak-builder >/dev/null 2>&1; then
        printf 'Flatpak tools already present.\n'
        return 0
    fi

    if ! command -v apt-get >/dev/null 2>&1; then
        xonotic_usage 'Install flatpak and flatpak-builder manually on this system.' 1
    fi

    printf 'Installing flatpak and flatpak-builder...\n'
    xonotic_maybe_sudo env DEBIAN_FRONTEND=noninteractive apt-get update -qq
    xonotic_maybe_sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
        flatpak flatpak-builder

    command -v flatpak >/dev/null 2>&1 || xonotic_usage 'flatpak install failed.' 1
    command -v flatpak-builder >/dev/null 2>&1 || xonotic_usage 'flatpak-builder install failed.' 1
}

xonotic_add_flatpak_remote() {
    flatpak remote-add --user --if-not-exists "$FLATPAK_REMOTE_NAME" "$FLATPAK_REMOTE_URL"
    flatpak remote-modify --user "$FLATPAK_REMOTE_NAME" --no-enumerate
}

if [ "$SKIP_DEPS" -eq 0 ]; then
    xonotic_install_flatpak_tools
fi

if [ "$DEPS_ONLY" -eq 1 ]; then
    printf 'Flatpak tools ready.\n'
    exit 0
fi

if [ "$ADD_REMOTE" -eq 1 ]; then
    xonotic_add_flatpak_remote
fi

if [ "$FROM_REMOTE" -eq 1 ]; then
    if [ "$ADD_REMOTE" -eq 0 ]; then
        xonotic_add_flatpak_remote
    fi
    flatpak install --user -y "$FLATPAK_REMOTE_NAME" "$FLATPAK_APP_ID"
elif [ "$SKIP_BUILD" -eq 0 ]; then
    if [ "$CLEAN" -eq 1 ]; then
        rm -rf "$ROOT/$FLATPAK_BUILD_DIR"
    fi

    bash "$ROOT/scripts/fetch-sources.sh" code

    flatpak-builder --user --install-deps-only --force-clean \
        "$ROOT/$FLATPAK_BUILD_DIR" "$ROOT/$FLATPAK_MANIFEST"

    flatpak-builder --user --install --force-clean \
        "$ROOT/$FLATPAK_BUILD_DIR" "$ROOT/$FLATPAK_MANIFEST"
else
    xonotic_usage 'Nothing to do: pass --from-remote or drop --skip-build.' 1
fi

printf 'Installed %s for user.\n' "$FLATPAK_APP_ID"

if [ "$RUN_APP" -eq 1 ]; then
    exec flatpak run "$FLATPAK_APP_ID"
fi
