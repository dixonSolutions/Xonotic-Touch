#!/bin/bash
# Run tests/web-cache/e2e.mjs against a headless GPU Chrome started here.
#   tests/web-cache/e2e.sh PROFILE_DIR URL LABEL [e2e.mjs options...]
# CHROME: the Chrome binary (default: Playwright's Chromium in ~/.cache).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
profile="$1" url="$2" label="$3"
shift 3
CHROME="${CHROME:-$(ls -d "$HOME"/.cache/ms-playwright/chromium-*/chrome-linux64/chrome 2>/dev/null | sort -V | tail -1)}"
port="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')"
mkdir -p "$profile"
"$CHROME" --headless=new --use-angle=gl-egl --enable-gpu --ignore-gpu-blocklist \
    --disable-breakpad --disable-crash-reporter --no-first-run --no-default-browser-check \
    --autoplay-policy=no-user-gesture-required --window-size=1280,720 \
    --user-data-dir="$profile" --remote-debugging-port="$port" about:blank \
    2>"${profile%/}.stderr.log" &
pid=$!
trap 'kill -9 $pid 2>/dev/null || true; wait $pid 2>/dev/null || true' EXIT
for _ in $(seq 100); do curl -sf "http://127.0.0.1:$port/json/version" >/dev/null && break; sleep 0.1; done
node "$here/e2e.mjs" --port "$port" --url "$url" --label "$label" "$@"
