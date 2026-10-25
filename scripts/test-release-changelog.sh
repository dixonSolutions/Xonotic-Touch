#!/bin/bash
# Checks scripts/release-changelog.sh against a throwaway repository: the
# ranges between release tags, the order, and the size cap the in-app Updates
# screen depends on. No network, no engine.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
script="$ROOT/scripts/release-changelog.sh"
repo="$(mktemp -d)"
trap 'rm -rf "$repo"' EXIT
fail=0

check() {
    if [ "$2" != "$3" ]; then
        printf 'FAIL %s\n--- expected\n%s\n--- got\n%s\n' "$1" "$2" "$3"
        fail=1
    fi
}

cd "$repo"
git init -q
git config user.name test
git config user.email test@example.invalid
commit() { git commit -q --allow-empty -m "$1"; }

commit "First"
git tag v1.2.1
commit "Second"
commit "Docs only [skip ci]"
git tag v1.2.2
commit "Third"
git checkout -q -b side
commit "On a side branch"
git checkout -q -
git merge -q --no-ff side -m "Merge branch 'side'"
commit "Fourth"

check "markdown for the release being built" "### Changes

- Third
- On a side branch
- Fourth" "$(bash "$script" markdown 1.2.3)"

check "text, newest release first" "1.2.3
- Third
- On a side branch
- Fourth

1.2.2
- Second
- Docs only

1.2.1
- First" "$(bash "$script" text 1.2.3)"

# A re-run, with the release's own tag already pushed, lists the same changes.
git tag v1.2.3
check "re-run with its own tag present" "### Changes

- Third
- On a side branch
- Fourth" "$(bash "$script" markdown 1.2.3)"

# Nothing new since the last tag still says so rather than printing a bare
# heading.
check "no changes" "### Changes

- No code changes since the previous release." "$(bash "$script" markdown 1.2.4)"

# The cap stops at a release boundary, never mid-list.
capped="$(CHANGELOG_MAX_BYTES=60 bash "$script" text 1.2.4 v1.2.3)"
check "size cap keeps whole releases" "1.2.4

1.2.3
- Third
- On a side branch
- Fourth" "$capped"

[ "$fail" = 0 ] && echo "release changelog: markdown, text, re-run and size cap OK"
exit "$fail"
