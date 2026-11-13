#!/bin/bash
# Publish this checkout's copy of a component to its own repository and pin it.
#
#   scripts/component-push.sh darkplaces "Console: open from the shell icon"
#   scripts/component-push.sh data       "Menus say Godnotic"
#   scripts/component-push.sh all        "..."      # both, same message
#
# The engine lives in dixonSolutions/Godnotic-darkplaces and the game code in
# dixonSolutions/Godnotic-data. Edit them here, where everything builds together,
# then run this before committing: it commits the component directory's tracked
# files to the component repository, pushes, and writes the new commit into
# components.json. Commit components.json together with the engine/data change;
# CI refuses a main whose copy differs from its pin.
#
# COMPONENT_PUSH_DRY=1 stops before the push and leaves the pin alone.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CACHE="${GODNOTIC_COMPONENT_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/godnotic-components}"

usage() { sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
[ $# -eq 2 ] || usage
WHICH="$1"; MESSAGE="$2"
[ -n "$MESSAGE" ] || usage

push_one() {
    local name="$1" repo dir head tree_before
    case "$name" in
        darkplaces) repo="dixonSolutions/Godnotic-darkplaces" ;;
        data) repo="dixonSolutions/Godnotic-data" ;;
        *) usage ;;
    esac
    dir="$CACHE/$name"
    mkdir -p "$CACHE"
    if [ -d "$dir/.git" ]; then
        git -C "$dir" fetch -q --depth 1 origin main
        git -C "$dir" checkout -q -B main FETCH_HEAD
        git -C "$dir" clean -qfdx
    else
        rm -rf "$dir"
        git clone -q --depth 1 --branch main "https://github.com/$repo.git" "$dir"
    fi
    python3 "$ROOT/scripts/export-components.py" --export "$name" "$dir"
    git -C "$dir" add -A
    if git -C "$dir" diff --cached --quiet; then
        echo "$name: already at $(git -C "$dir" rev-parse HEAD)"
    else
        git -C "$dir" -c user.name="$(git -C "$ROOT" config user.name)" \
            -c user.email="$(git -C "$ROOT" config user.email)" \
            commit -q -m "$MESSAGE" -m "From dixonSolutions/Godnotic $(git -C "$ROOT" rev-parse --short HEAD) plus working tree."
        git -C "$dir" --no-pager show --stat --format='%h %s' HEAD | tail -n 4
        if [ "${COMPONENT_PUSH_DRY:-0}" = 1 ]; then
            echo "$name: dry run, not pushed"
            return 0
        fi
        git -C "$dir" -c credential.helper='!gh auth git-credential' push -q origin HEAD:main
    fi
    head="$(git -C "$dir" rev-parse HEAD)"
    python3 "$ROOT/scripts/export-components.py" --pin "$name" "$head"
}

if [ "$WHICH" = all ]; then
    push_one darkplaces
    push_one data
else
    push_one "$WHICH"
fi
