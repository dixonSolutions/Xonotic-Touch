#!/bin/bash
# Lua mods in the real web build, in headless GPU Chromium (browser.mjs).
#   env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY tests/web-mods/browser.sh [build/web]
# Needs a built page with staged data (tests/web-cache/README.md, "Setup").
# CHROME: the Chrome binary (default: Playwright's Chromium in ~/.cache).
set -euo pipefail
if [ -n "${WAYLAND_DISPLAY:-}" ] || [ "${DISPLAY:-}" = ":0" ] || [ "${DISPLAY:-}" = ":1" ]; then
    echo "browser.sh: unset WAYLAND_DISPLAY/DISPLAY (env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY ...)" >&2
    exit 2
fi
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
site="$(cd "${1:-$root/build/web}" && pwd)"
CHROME="${CHROME:-$(ls -d "$HOME"/.cache/ms-playwright/chromium-*/chrome-linux64/chrome 2>/dev/null | sort -V | tail -1)}"
# short: Chromium puts a Unix socket in its profile, and those paths are limited to 107 bytes
work="$(mktemp -d /tmp/gwm.XXXXXX)"
pids=()
cleanup() {
    for p in "${pids[@]}"; do kill -9 "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
    rm -rf "$work" 2>/dev/null || { sleep 1; rm -rf "$work"; }
}
trap cleanup EXIT
free() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }

# Two test mods: hello (installs, nothing else) and webcheck (xt.http).
python3 -I - "$work" <<'PY'
import sys, zipfile, os
w = sys.argv[1]
def pack(name, files):
    with zipfile.ZipFile(os.path.join(w, name + '.zip'), 'w') as z:
        for n, t in files.items():
            z.writestr(f'{name}/{n}', t)
pack('hello', {
    'mod.txt': 'id: hello\ntitle: Hello\nversion: 1.0\napi: 3\nrealm: client\npermissions:\n',
    'init.lua': 'xt.command("greet", function() print("hello from the web") end, "say hi")\n',
})
pack('webcheck', {
    'mod.txt': 'id: webcheck\ntitle: Web check\nversion: 1.0\napi: 3\nrealm: client\npermissions: internet\nreason.internet: the test\nhttp: 127.0.0.1\n',
    'init.lua': '''
local function show(s, b) print("webcheck answer " .. tostring(s) .. " " .. tostring(b)) end
xt.command("has", function() print("webcheck has http " .. tostring(xt.game.has("http"))) end, "")
xt.command("get", function(u) xt.http.get(u, show) end, "")
xt.command("post", function(u) xt.http.post(u, "ping", show, { type = "text/plain" }) end, "")
''',
})
open(os.path.join(w, 'answer.txt'), 'w').write('hello from the test host')
PY

# The mod hosts: one allows requests from web pages (CORS), one does not.
cat > "$work/host.py" <<'PY'
import http.server, sys, os
cors = sys.argv[2] == 'cors'
os.chdir(sys.argv[3])
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        if cors:
            self.send_header('Access-Control-Allow-Origin', '*')
            self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        super().end_headers()
    def do_OPTIONS(self):
        self.send_response(204); self.end_headers()
    def do_POST(self):
        n = int(self.headers.get('Content-Length') or 0)
        body = b'posted:' + self.rfile.read(n)
        self.send_response(200); self.send_header('Content-Length', str(len(body))); self.end_headers()
        self.wfile.write(body)
    def log_message(self, *a): pass
http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1])), H).serve_forever()
PY
pc="$(free)"; pn="$(free)"; pp="$(free)"; cdp="$(free)"
python3 "$work/host.py" "$pc" cors "$work" & pids+=($!)
python3 "$work/host.py" "$pn" nocors "$work" & pids+=($!)
python3 "$root/web/serve.py" "$site" --port "$pp" >"$work/serve.log" 2>&1 & pids+=($!)
"$CHROME" --headless=new --use-angle=gl-egl --enable-gpu --ignore-gpu-blocklist \
    --disable-breakpad --disable-crash-reporter --no-first-run --no-default-browser-check \
    --autoplay-policy=no-user-gesture-required --window-size=1280,720 \
    --user-data-dir="$work/profile" --remote-debugging-port="$cdp" about:blank \
    2>"$work/chrome.log" & pids+=($!)
for _ in $(seq 100); do curl -sf "http://127.0.0.1:$cdp/json/version" >/dev/null && curl -sf "http://127.0.0.1:$pp/" >/dev/null && break; sleep 0.1; done
node "$here/browser.mjs" --port "$cdp" --url "http://127.0.0.1:$pp/" \
    --cors "http://127.0.0.1:$pc" --nocors "http://127.0.0.1:$pn" \
    --zip "$work/hello.zip" --httpzip "$work/webcheck.zip" ${VERBOSE:+--verbose} ${SHOTS:+--shots "$SHOTS"}
