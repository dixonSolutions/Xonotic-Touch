#!/bin/bash
# Clone the full Xonotic project into engine/ (fork-style). Touch port changes live in-tree.
# Set XONOTIC_URL to your GitLab/GitHub fork to push engine changes upstream later.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
XONOTIC_URL="${XONOTIC_URL:-https://gitlab.com/xonotic/xonotic.git}"
FRESH=0
PREPARE=0

usage() {
    echo "Usage: $0 [--fresh] [--prepare-git]" >&2
    echo "  Clones xonotic superproject + ./all update (maps, music, all repos)." >&2
    echo "  --fresh        remove engine/ first (clean re-clone)" >&2
    echo "  --prepare-git  run prepare-engine-for-git.sh after update (for committing in this repo)" >&2
    echo "  XONOTIC_URL=…  point at your fork (default: upstream GitLab)" >&2
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --fresh) FRESH=1 ;;
        --prepare-git) PREPARE=1 ;;
        -h|--help) usage ;;
        *) echo "Unknown option: $1" >&2; usage ;;
    esac
    shift
done

ensure_superproject_git() {
    if [ -d "$ROOT/engine/.git" ] && [ -f "$ROOT/engine/all" ]; then
        echo "engine/ superproject git already present"
        return 0
    fi

    if [ "$FRESH" -eq 1 ] && [ -d "$ROOT/engine" ]; then
        echo "Removing existing engine/ (--fresh)..."
        rm -rf "$ROOT/engine"
    fi

    if [ ! -f "$ROOT/engine/all" ]; then
        echo "Cloning Xonotic superproject -> engine/"
        echo "  $XONOTIC_URL"
        git clone --depth 1 "$XONOTIC_URL" "$ROOT/engine"
        return 0
    fi

    echo "engine/ exists without .git — attaching upstream superproject metadata..."
    (
        cd "$ROOT/engine"
        git init -q
        git remote add origin "$XONOTIC_URL" 2>/dev/null || git remote set-url origin "$XONOTIC_URL"
        git fetch --depth 1 origin
        git checkout -q FETCH_HEAD
    )
}

echo "=== Xonotic fork init (full project) ==="
ensure_superproject_git

cd "$ROOT/engine"
echo "Running ./all update -l best (maps, music, mediasource — large download)..."
./all update -l best

cd "$ROOT"
bash "$ROOT/scripts/fetch-sources.sh" code

if [ "$PREPARE" -eq 1 ]; then
    bash "$ROOT/scripts/prepare-engine-for-git.sh" --yes
fi

du -sh "$ROOT/engine"
echo
echo "Full Xonotic tree ready under engine/ (touch port changes integrated in-tree)."
echo "  Edit engine/ directly — touch CSQC, menus, and darkplaces patches are committed here."
echo "  Pull upstream: ./scripts/sync-upstream-fork.sh --init-git  (once), then ./scripts/sync-upstream-fork.sh"
echo "  Monorepo commit: ./scripts/prepare-engine-for-git.sh --yes && git add engine/"
