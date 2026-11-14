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
# The export replaces the component repository's files with this checkout's,
# so it refuses when that repository's main has moved past the commit this
# checkout pins (another agent pushed, or a commit was made there directly):
# exporting then would undo that work. It says how to merge it in first. A
# move that touches only the repository's own files (.github/, .godnotic/,
# GODNOTIC.md) is not a conflict. The push only fast-forwards from the main
# it checked, so a push that lands in between is rejected, not overwritten.
# With "all", both are checked before either is pushed.
#
# COMPONENT_PUSH_DRY=1 stops before the push and leaves the pin alone.
# COMPONENT_PUSH_OVER=<sha> pushes over that moved head anyway, on purpose.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CACHE="${GODNOTIC_COMPONENT_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/godnotic-components}"
# where the component repositories live (scripts/test-component-push.sh uses local ones)
REMOTE_BASE="${GODNOTIC_COMPONENT_REMOTE_BASE:-https://github.com}"
ZERO=0000000000000000000000000000000000000000

usage() { sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
[ $# -eq 2 ] || usage
WHICH="$1"; MESSAGE="$2"
[ -n "$MESSAGE" ] || usage

repo_of() {
    case "$1" in
        darkplaces) echo "dixonSolutions/Godnotic-darkplaces" ;;
        data) echo "dixonSolutions/Godnotic-data" ;;
        *) usage ;;
    esac
}

# Fetch the component's main into the cache clone and refuse if it moved past
# the pin in a way the export would undo.
check_one() {
    local name="$1" repo dir head pin path moved
    repo="$(repo_of "$name")"
    dir="$CACHE/$name"
    mkdir -p "$CACHE"
    if [ -d "$dir/.git" ]; then
        git -C "$dir" remote set-url origin "$REMOTE_BASE/$repo.git"
        git -C "$dir" fetch -q --depth 1 origin main
        git -C "$dir" checkout -q -B main FETCH_HEAD
        git -C "$dir" clean -qfdx
    else
        rm -rf "$dir"
        git clone -q --depth 1 --branch main "$REMOTE_BASE/$repo.git" "$dir"
    fi
    head="$(git -C "$dir" rev-parse HEAD)"
    pin="$(python3 "$ROOT/scripts/export-components.py" --show-pin "$name")"
    [ "$head" = "$pin" ] || [ "$pin" = "$ZERO" ] && return 0
    if git -C "$dir" fetch -q --depth 1 origin "$pin" 2>/dev/null; then
        moved="$(git -C "$dir" diff --name-only "$pin" "$head" -- . ':!.github' ':!.godnotic' ':!GODNOTIC.md')"
    else
        moved="(the pinned commit is not in $repo)"
    fi
    [ -n "$moved" ] || return 0
    [ "${COMPONENT_PUSH_OVER:-}" = "$head" ] && return 0
    path="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["components"][sys.argv[2]]["path"])' "$ROOT/components.json" "$name")"
    {
        echo "$name: refusing to push. $repo main is at ${head:0:12}, but this checkout"
        echo "pins ${pin:0:12}. Exporting would undo what changed there since:"
        printf '%s\n' "$moved" | head -n 12 | sed 's/^/    /'
        echo "Merge it in first, then run this again. If origin/main already pins"
        echo "${head:0:12}, that is just: git fetch origin && git merge origin/main"
        echo "Otherwise:"
        echo "    git switch -c import-$name origin/main"
        echo "    python3 scripts/export-components.py --import $name $head"
        echo "    git commit -am \"Import $name ${head:0:12}\""
        echo "    git switch -                # back to your branch"
        echo "    git merge import-$name      # conflicts under $path resolve as usual"
        echo "To replace that head on purpose: COMPONENT_PUSH_OVER=$head $0 $WHICH \"...\""
    } >&2
    return 3
}

push_one() {
    local name="$1" dir head
    dir="$CACHE/$name"
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
        # never forced: it fast-forwards from the head check_one saw, or fails
        git -C "$dir" -c credential.helper='!gh auth git-credential' push -q origin HEAD:main
    fi
    head="$(git -C "$dir" rev-parse HEAD)"
    python3 "$ROOT/scripts/export-components.py" --pin "$name" "$head"
}

if [ "$WHICH" = all ]; then
    names="darkplaces data"
else
    repo_of "$WHICH" >/dev/null
    names="$WHICH"
fi
for n in $names; do check_one "$n" || exit 3; done
for n in $names; do push_one "$n"; done
