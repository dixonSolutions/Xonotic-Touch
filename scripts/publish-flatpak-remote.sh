#!/bin/bash
# Import Flatpak bundles into the public OSTree repo, preserving prior commits.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
FLATPAK_REMOTE_URL="${FLATPAK_REMOTE_URL:-https://dixonSolutions.github.io/Xonotic-Touch/flatpak}"
FLATPAK_APP_ID="${FLATPAK_APP_ID:-io.github.dixonSolutions.XonoticTouch}"
REPO_DIR="${REPO_DIR:-$ROOT/combined-repo}"
SITE_DIR="${SITE_DIR:-$ROOT/site/flatpak}"
X86_BUNDLE="${X86_BUNDLE:-}"
A64_BUNDLE="${A64_BUNDLE:-}"
PACKAGE_VERSION="${PACKAGE_VERSION:-}"
PRUNE_DEPTH="${PRUNE_DEPTH:-40}"

usage() {
    cat <<EOF
Usage: $(basename "$0") --x86 BUNDLE --aarch64 BUNDLE [--version VER]

Pulls the existing Pages OSTree repo (if reachable), imports new bundles,
and writes site/flatpak/ for GitHub Pages deploy. Prior commits are kept
(prune-depth=${PRUNE_DEPTH}; set PRUNE_DEPTH=0 to disable pruning).
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --x86) X86_BUNDLE="$2"; shift 2 ;;
        --aarch64) A64_BUNDLE="$2"; shift 2 ;;
        --version) PACKAGE_VERSION="$2"; shift 2 ;;
        --repo) REPO_DIR="$2"; shift 2 ;;
        --site) SITE_DIR="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

test -n "$X86_BUNDLE" && test -f "$X86_BUNDLE" || {
    echo "Missing x86_64 bundle (--x86)" >&2
    exit 1
}
test -n "$A64_BUNDLE" && test -f "$A64_BUNDLE" || {
    echo "Missing aarch64 bundle (--aarch64)" >&2
    exit 1
}

command -v flatpak >/dev/null
command -v ostree >/dev/null
command -v curl >/dev/null

rm -rf "$REPO_DIR"
mkdir -p "$REPO_DIR"
ostree init --repo="$REPO_DIR" --mode=archive-z2

pull_existing_remote() {
    if ! curl -fsSIL "$FLATPAK_REMOTE_URL/config" >/dev/null 2>&1; then
        printf 'No existing remote at %s — starting fresh\n' "$FLATPAK_REMOTE_URL"
        return 1
    fi

    printf 'Mirroring existing Flatpak remote from %s\n' "$FLATPAK_REMOTE_URL"
    ostree remote delete --repo="$REPO_DIR" origin 2>/dev/null || true
    ostree remote add --repo="$REPO_DIR" --no-gpg-verify origin "$FLATPAK_REMOTE_URL"

    # Mirror every ref the public remote advertises (apps, appstream, …).
    if ostree pull --repo="$REPO_DIR" --mirror origin; then
        printf 'Mirrored existing OSTree history\n'
        ostree remote delete --repo="$REPO_DIR" origin 2>/dev/null || true
        return 0
    fi

    printf 'Mirror pull failed — trying per-arch app refs\n' >&2
    local ok=0
    for arch in x86_64 aarch64; do
        if ostree pull --repo="$REPO_DIR" origin "app/${FLATPAK_APP_ID}/${arch}/master"; then
            ok=1
        fi
        ostree pull --repo="$REPO_DIR" origin "appstream2/${arch}" 2>/dev/null || true
    done
    ostree remote delete --repo="$REPO_DIR" origin 2>/dev/null || true
    [ "$ok" = "1" ]
}

pull_existing_remote || true

printf 'Importing %s\n' "$X86_BUNDLE"
flatpak build-import-bundle "$REPO_DIR" "$X86_BUNDLE"
printf 'Importing %s\n' "$A64_BUNDLE"
flatpak build-import-bundle "$REPO_DIR" "$A64_BUNDLE"

UPDATE_ARGS=(--generate-static-deltas)
if [ -n "${PRUNE_DEPTH}" ] && [ "${PRUNE_DEPTH}" != "0" ]; then
    UPDATE_ARGS+=(--prune --prune-depth="$PRUNE_DEPTH")
fi
flatpak build-update-repo "$REPO_DIR" "${UPDATE_ARGS[@]}"

rm -rf "$SITE_DIR"
mkdir -p "$SITE_DIR"
cp -a "$REPO_DIR"/. "$SITE_DIR"/

VERSION_NOTE=""
if [ -n "$PACKAGE_VERSION" ]; then
    VERSION_NOTE="Latest imported build: <code>${PACKAGE_VERSION}</code><br>"
fi

cat > "$SITE_DIR/index.html" <<EOF
<!DOCTYPE html>
<html lang="en">
<head><meta charset="utf-8"><title>Xonotic Touch Flatpak remote</title></head>
<body>
<h1>Xonotic Touch Flatpak remote</h1>
<p>${VERSION_NOTE}OSTree history is retained (last ${PRUNE_DEPTH} commits per ref) so older builds remain available for rollback.</p>
<pre>
flatpak remote-add --user --if-not-exists xonotic-touch \\
  ${FLATPAK_REMOTE_URL}
flatpak install --user xonotic-touch io.github.dixonSolutions.XonoticTouch
flatpak update --user io.github.dixonSolutions.XonoticTouch

# Inspect / roll back to an older commit:
flatpak remote-info --log xonotic-touch io.github.dixonSolutions.XonoticTouch
</pre>
</body>
</html>
EOF

printf 'Flatpak remote staged at %s\n' "$SITE_DIR"
