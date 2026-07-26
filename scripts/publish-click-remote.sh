#!/bin/bash
# Stage Ubuntu Touch .click packages onto the GitHub Pages site (download "remote").
# Click has no OSTree equivalent — this publishes stable download URLs + history index.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
SITE_CLICK="${SITE_CLICK:-$ROOT/site/click}"
CLICK_REMOTE_URL="${CLICK_REMOTE_URL:-https://dixonSolutions.github.io/Xonotic-Touch/click}"
PACKAGE_VERSION="${PACKAGE_VERSION:-}"
CLICK_NAME="${CLICK_NAME:-xonotictouch.dixonsolutions}"
ARM64_CLICK="${ARM64_CLICK:-}"
ARMHF_CLICK="${ARMHF_CLICK:-}"

usage() {
    cat <<EOF
Usage: $(basename "$0") --arm64 FILE.click --armhf FILE.click [--version VER]

Writes site/click/ with versioned packages, latest-* aliases, and an index.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --arm64) ARM64_CLICK="$2"; shift 2 ;;
        --armhf) ARMHF_CLICK="$2"; shift 2 ;;
        --version) PACKAGE_VERSION="$2"; shift 2 ;;
        --site) SITE_CLICK="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

test -n "$ARM64_CLICK" && test -f "$ARM64_CLICK" || {
    echo "Missing arm64 .click (--arm64)" >&2
    exit 1
}
test -n "$ARMHF_CLICK" && test -f "$ARMHF_CLICK" || {
    echo "Missing armhf .click (--armhf)" >&2
    exit 1
}
test -n "$PACKAGE_VERSION" || PACKAGE_VERSION="unknown"

mkdir -p "$SITE_CLICK"
install -m 644 "$ARM64_CLICK" "$SITE_CLICK/${CLICK_NAME}_${PACKAGE_VERSION}_arm64.click"
install -m 644 "$ARMHF_CLICK" "$SITE_CLICK/${CLICK_NAME}_${PACKAGE_VERSION}_armhf.click"
# Stable aliases for scripts / forum posts
install -m 644 "$ARM64_CLICK" "$SITE_CLICK/latest-arm64.click"
install -m 644 "$ARMHF_CLICK" "$SITE_CLICK/latest-armhf.click"

cat > "$SITE_CLICK/latest.json" <<EOF
{
  "name": "${CLICK_NAME}",
  "version": "${PACKAGE_VERSION}",
  "framework": "ubuntu-touch-24.04-1.x",
  "packages": {
    "arm64": "${CLICK_REMOTE_URL}/latest-arm64.click",
    "armhf": "${CLICK_REMOTE_URL}/latest-armhf.click"
  },
  "versioned": {
    "arm64": "${CLICK_REMOTE_URL}/${CLICK_NAME}_${PACKAGE_VERSION}_arm64.click",
    "armhf": "${CLICK_REMOTE_URL}/${CLICK_NAME}_${PACKAGE_VERSION}_armhf.click"
  }
}
EOF

cat > "$SITE_CLICK/index.html" <<EOF
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Xonotic Touch — Ubuntu Touch (.click)</title>
  <style>
    body { font-family: system-ui, sans-serif; max-width: 44rem; margin: 2rem auto; padding: 0 1rem; line-height: 1.45; }
    code, pre { background: #f2f2f2; padding: 0.15em 0.35em; border-radius: 4px; }
    pre { padding: 0.85rem 1rem; overflow-x: auto; }
    a { color: #0b5fff; }
  </style>
</head>
<body>
  <h1>Xonotic Touch for Ubuntu Touch</h1>
  <p>Latest build: <strong>${PACKAGE_VERSION}</strong> (<code>${CLICK_NAME}</code>)</p>
  <p>Download and install on the device:</p>
  <ul>
    <li><a href="latest-arm64.click">latest-arm64.click</a> (phones / tablets, 64-bit)</li>
    <li><a href="latest-armhf.click">latest-armhf.click</a> (older 32-bit ARM)</li>
  </ul>
  <pre>wget ${CLICK_REMOTE_URL}/latest-arm64.click
pkcon install-local --allow-untrusted latest-arm64.click</pre>
  <p>Versioned files also published:
    <a href="${CLICK_NAME}_${PACKAGE_VERSION}_arm64.click">${CLICK_NAME}_${PACKAGE_VERSION}_arm64.click</a>,
    <a href="${CLICK_NAME}_${PACKAGE_VERSION}_armhf.click">${CLICK_NAME}_${PACKAGE_VERSION}_armhf.click</a>
  </p>
  <p>Machine-readable: <a href="latest.json">latest.json</a>.
  Flatpak remote (Linux desktop/tablet):
  <a href="../flatpak/">../flatpak/</a></p>
</body>
</html>
EOF

printf 'Click download remote staged at %s (version %s)\n' "$SITE_CLICK" "$PACKAGE_VERSION"
