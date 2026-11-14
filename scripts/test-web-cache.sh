#!/bin/bash
# Web loader tests: the data store and loader flow (node, no browser), the
# reproducibility of the staged data, and, where a browser is installed, the
# split data set in headless Chromium and Firefox against a local server
# (tests/web-cache/split.sh; XT_SKIP_BROWSERS=1 leaves it out).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
node "$ROOT/tests/web-cache/cache.test.mjs"
bash "$ROOT/scripts/test-web-cache-repro.sh"

engines=""
# (No match makes ls fail, and with pipefail the assignment: "|| true".)
chrome="${CHROME:-$(ls -d "$HOME"/.cache/ms-playwright/chromium-*/chrome-linux64/chrome 2>/dev/null | sort -V | tail -1 || true)}"
[ -z "$chrome" ] || [ ! -x "$chrome" ] || engines="chromium"
if [ -n "${FIREFOX:-}" ] || { command -v flatpak >/dev/null 2>&1 && flatpak info org.mozilla.firefox >/dev/null 2>&1; }; then
    engines="${engines:+$engines,}firefox"
fi
if [ -n "${XT_SKIP_BROWSERS:-}" ] || [ -z "$engines" ]; then
    echo "split set in browsers: skipped (no Chromium or Firefox here; see tests/web-cache/README.md)"
else
    # Headless, and never on a desktop session's display.
    env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY CHROME="$chrome" bash "$ROOT/tests/web-cache/split.sh" --engines "$engines"
fi
