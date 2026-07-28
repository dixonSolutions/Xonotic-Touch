#!/bin/bash
# Merge upstream Xonotic sub-repos into engine/ while keeping Touch port changes.
#
# Preferred monorepo workflow (no nested .git required):
#   See docs/UPSTREAM_SYNC.md — overlay upstream tips, restore Touch-only files,
#   then re-port shared hotspots (vid_sdl.c, GameMenu, client/view.qc, …).
#
# Nested-git / fork workflow (optional):
#   ./scripts/sync-upstream-fork.sh --init-git
#   FORK_DARKPLACES=… FORK_DATA=… ./scripts/sync-upstream-fork.sh
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
UPSTREAM_DARKPLACES="${UPSTREAM_DARKPLACES:-https://gitlab.com/xonotic/darkplaces.git}"
UPSTREAM_DATA="${UPSTREAM_DATA:-https://gitlab.com/xonotic/xonotic-data.pk3dir.git}"
UPSTREAM_GMQCC="${UPSTREAM_GMQCC:-https://gitlab.com/xonotic/gmqcc.git}"
FORK_DARKPLACES="${FORK_DARKPLACES:-${DARKPLACES_URL:-}}"
FORK_DATA="${FORK_DATA:-${DATA_URL:-}}"
FORK_GMQCC="${FORK_GMQCC:-${GMQCC_URL:-}}"

usage() {
    echo "Usage: $0 [--init-git] [--allow-unrelated] [darkplaces|data|gmqcc|all]" >&2
    echo "  Merges upstream GitLab into nested git under engine/ sub-repos." >&2
    echo "  --init-git         create .git in sub-repos from current vendored tree (once)" >&2
    echo "  --allow-unrelated  allow first merge when histories do not share a base" >&2
    echo "  Fork push only when FORK_DARKPLACES / FORK_DATA / FORK_GMQCC are set." >&2
    echo "  For vendored monorepo sync without nested git, see docs/UPSTREAM_SYNC.md." >&2
    exit 1
}

INIT_GIT=0
ALLOW_UNRELATED=0
TARGET="all"

while [ $# -gt 0 ]; do
    case "$1" in
        --init-git) INIT_GIT=1 ;;
        --allow-unrelated) ALLOW_UNRELATED=1 ;;
        darkplaces|data|gmqcc|all) TARGET="$1" ;;
        -h|--help) usage ;;
        *) echo "Unknown option: $1" >&2; usage ;;
    esac
    shift
done

ensure_repo_git() {
    local dir="$1"
    local upstream="$2"
    local fork="${3:-}"

    if [ ! -d "$dir" ]; then
        echo "Missing $dir — run ./scripts/fetch-sources.sh code first" >&2
        exit 1
    fi

    cd "$dir"

    if [ ! -d .git ]; then
        if [ "$INIT_GIT" -ne 1 ]; then
            echo "$dir has no .git — re-run with --init-git once, or use the overlay" >&2
            echo "workflow in docs/UPSTREAM_SYNC.md (recommended for this monorepo)." >&2
            exit 1
        fi
        echo "Initializing git in $dir from current tree..."
        git init -q
        git add -A
        git commit -q -m "Xonotic Touch port baseline (integrated touch changes)"
        # First merge against GitLab will almost always be unrelated.
        ALLOW_UNRELATED=1
    fi

    git remote remove upstream 2>/dev/null || true
    git remote add upstream "$upstream"

    if [ -n "$fork" ]; then
        if git remote get-url origin >/dev/null 2>&1; then
            git remote set-url origin "$fork"
        else
            git remote add origin "$fork"
        fi
    fi

    echo "Fetching upstream for $dir ..."
    git fetch --depth 50 upstream

    local branch
    branch="$(git remote show upstream 2>/dev/null | awk '/HEAD branch/ {print $NF}')"
    branch="${branch:-master}"

    local merge_args=(--no-edit)
    if [ "$ALLOW_UNRELATED" -eq 1 ]; then
        merge_args+=(--allow-unrelated-histories)
    fi

    echo "Merging upstream/$branch into $(basename "$dir") ..."
    if ! git merge "${merge_args[@]}" "upstream/$branch"; then
        echo
        echo "Merge conflict in $dir — resolve Touch hotspots carefully, then:"
        echo "  cd $dir && git add -A && git commit"
        if [ -n "$fork" ]; then
            echo "  git push origin HEAD"
        fi
        echo "See docs/UPSTREAM_SYNC.md for Touch-only paths and shared hotspots."
        exit 1
    fi

    if [ -n "$fork" ]; then
        echo "Pushing merged $dir to fork ..."
        git push -u origin HEAD
    else
        echo "No FORK_* URL set — skipped push for $(basename "$dir")."
    fi
}

echo "=== Sync upstream into engine/ fork sub-repos ==="

case "$TARGET" in
    darkplaces)
        ensure_repo_git "$ROOT/engine/darkplaces" "$UPSTREAM_DARKPLACES" "$FORK_DARKPLACES"
        ;;
    data)
        ensure_repo_git "$ROOT/engine/data/xonotic-data.pk3dir" "$UPSTREAM_DATA" "$FORK_DATA"
        ;;
    gmqcc)
        ensure_repo_git "$ROOT/engine/gmqcc" "$UPSTREAM_GMQCC" "$FORK_GMQCC"
        ;;
    all)
        ensure_repo_git "$ROOT/engine/darkplaces" "$UPSTREAM_DARKPLACES" "$FORK_DARKPLACES"
        ensure_repo_git "$ROOT/engine/data/xonotic-data.pk3dir" "$UPSTREAM_DATA" "$FORK_DATA"
        ensure_repo_git "$ROOT/engine/gmqcc" "$UPSTREAM_GMQCC" "$FORK_GMQCC"
        ;;
    *)
        usage
        ;;
esac

echo "Done. Touch changes are integrated in engine/ — re-run ./scripts/prepare-engine-for-git.sh --yes before monorepo commit if needed."
