#!/bin/bash
# What changed in each release, from git history: the commit subjects between
# one v* tag and the next.
#
#   release-changelog.sh markdown VERSION [REV]
#       "### Changes" for the release being built (REV, default HEAD), to go in
#       its GitHub release notes.
#   release-changelog.sh text VERSION [REV]
#       Plain text for the in-app Updates screen, newest release first, which
#       the Pages job publishes as changelog.txt:
#
#           1.2.90
#           - Show ALT and ZOOM only for a weapon that can use them
#           - ...
#
#           1.2.89
#           ...
#
#       The menu reads it with uri_get, which truncates a reply at 16 KiB
#       (MAX_INPUTLINE), so it stops before CHANGELOG_MAX_BYTES (default 15000)
#       at a release boundary rather than mid-list.
#
# Needs full history and tags (actions/checkout with fetch-depth: 0).
set -euo pipefail

mode="${1:?usage: release-changelog.sh markdown|text VERSION [REV]}"
version="${2:?usage: release-changelog.sh markdown|text VERSION [REV]}"
rev="${3:-HEAD}"
max_bytes="${CHANGELOG_MAX_BYTES:-15000}"
max_releases="${CHANGELOG_MAX_RELEASES:-15}"

# Release tags reachable from REV, newest first, without the one being built
# (a re-run finds its own tag already pushed).
release_tags() {
    git tag -l 'v[0-9]*' --sort=-v:refname | while read -r tag; do
        [ "$tag" = "v${version}" ] && continue
        git merge-base --is-ancestor "$tag" "$rev" 2>/dev/null && echo "$tag"
    done
}

# Subjects of the commits in a range, oldest first so a release reads in the
# order it was built. Merges and the bot's own commits say nothing.
subjects() {
    git log --no-merges --topo-order --reverse --format='%s' "$1" \
        | { grep -v -e '^Merge ' -e '^\[bot\]' || true; } \
        | sed -e 's/[[:space:]]*\[skip ci\]//' -e 's/^/- /'
}

mapfile -t tags < <(release_tags)

range_for_current() {
    if [ "${#tags[@]}" -gt 0 ]; then
        echo "${tags[0]}..${rev}"
    else
        echo "$rev"
    fi
}

case "$mode" in
    markdown)
        echo "### Changes"
        echo
        changes="$(subjects "$(range_for_current)")"
        if [ -n "$changes" ]; then
            echo "$changes"
        else
            echo "- No code changes since the previous release."
        fi
        ;;
    text)
        out="$(mktemp)"
        trap 'rm -f "$out"' EXIT
        {
            echo "$version"
            subjects "$(range_for_current)"
        } > "$out"
        count=1
        for ((i = 0; i < ${#tags[@]} && count < max_releases; i++)); do
            tag="${tags[$i]}"
            if [ $((i + 1)) -lt "${#tags[@]}" ]; then
                range="${tags[$((i + 1))]}..${tag}"
            else
                range="$tag"
            fi
            block="$(printf '\n%s\n%s\n' "${tag#v}" "$(subjects "$range")")"
            if [ $(( $(wc -c < "$out") + ${#block} )) -gt "$max_bytes" ]; then
                break
            fi
            printf '%s\n' "$block" >> "$out"
            count=$((count + 1))
        done
        cat "$out"
        ;;
    *)
        echo "unknown mode: $mode" >&2
        exit 2
        ;;
esac
