#!/bin/bash
# Check that the published release is something the in-app updater can consume.
#
# AppUpdater reads GitHub's "latest release" feed, parses the tag as a dotted
# version, and picks the asset whose filename contains the device's ABI. All
# three of those are conventions, not contracts — renaming the APKs or tagging
# without a leading "v" silently strands every installed copy on its current
# build, with no error anywhere. This asserts them instead.
set -euo pipefail

REPO="${UPDATE_FEED_REPO:-dixonSolutions/Xonotic-Touch}"
ABIS="${UPDATE_FEED_ABIS:-arm64-v8a armeabi-v7a}"
API="${GITHUB_API_URL:-https://api.github.com}"

usage() {
    cat <<EOF
Usage: $(basename "$0") [--repo owner/name] [--abis "arm64-v8a armeabi-v7a"]

Fails when the latest release could not be auto-installed by the shipped app.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --repo) REPO="$2"; shift 2 ;;
        --abis) ABIS="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
done

# A bare `[ ... ] && auth=(...)` would take the whole script down under set -e
# on the common case of no token being set.
token="${GH_TOKEN:-${GITHUB_TOKEN:-}}"
auth=()
if [ -n "$token" ]; then
    auth=(-H "Authorization: Bearer $token")
fi

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT
curl -fsSL "${auth[@]}" \
    -H 'Accept: application/vnd.github+json' \
    "$API/repos/$REPO/releases/latest" > "$tmp"

python3 - "$ABIS" "$tmp" <<'PY'
import json, sys, re

abis = sys.argv[1].split()
with open(sys.argv[2]) as feed:
    release = json.load(feed)

problems = []

tag = release.get("tag_name", "")
version = tag[1:] if tag.startswith("v") else tag
if not re.fullmatch(r"\d+(\.\d+)*", version):
    problems.append(
        f'tag {tag!r} does not parse as a dotted version; AppUpdater would '
        f'read it as 0 and never offer the update'
    )

if release.get("draft"):
    problems.append("latest release is a draft, which the updater ignores")
if release.get("prerelease"):
    problems.append("latest release is a prerelease, which the updater ignores")

names = [a.get("name", "") for a in release.get("assets", [])]
for abi in abis:
    if not any(n.endswith(".apk") and abi in n for n in names):
        problems.append(f"no .apk asset whose name contains {abi!r}")

if problems:
    print(f"Release {tag} cannot drive in-app updates:", file=sys.stderr)
    for p in problems:
        print(f"  - {p}", file=sys.stderr)
    print(f"  assets: {names}", file=sys.stderr)
    sys.exit(1)

print(f"Release {tag} is consumable by the in-app updater ({len(abis)} ABIs).")
PY
