#!/usr/bin/env python3
"""Serve a built web bundle (build/web) for local testing.

    python3 web/serve.py [DIR] [--port 8080] [--host 127.0.0.1]

Browsers only allow the Cache API, pointer lock with raw input, fullscreen
keyboard lock and wake lock on secure origins; http://localhost counts as
one. To test on a phone on the same network, put the bundle behind https
(any static host or a local reverse proxy with a certificate).

The engine and page are sent with "no-cache" so a rebuild shows up on the
next reload; the pk3s are cached by the page itself (Cache API), keyed on the
content hash in data/manifest.json.
"""
import argparse
import functools
import http.server
import os
import sys
import time

TYPES = {
    ".wasm": "application/wasm",
    ".js": "text/javascript",
    ".mjs": "text/javascript",
    ".json": "application/json",
    ".webmanifest": "application/manifest+json",
    ".pk3": "application/octet-stream",
    ".cfg": "text/plain; charset=utf-8",
}


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, **TYPES}

    def end_headers(self):
        path = self.path.split("?", 1)[0]
        if not path.endswith(".pk3"):
            self.send_header("Cache-Control", "no-cache")
        # Not needed today (no threads), but keeps SharedArrayBuffer available
        # if the engine is ever built with -pthread.
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        super().end_headers()

    def do_POST(self):
        # Debug log sink for shell.js ?logsink (dev server only).
        if self.path != "/__log":
            self.send_error(404)
            return
        n = int(self.headers.get("Content-Length") or 0)
        line = self.rfile.read(min(n, 65536)).decode("utf-8", "replace").rstrip("\n")
        sys.stdout.write(f"[page {time.strftime('%H:%M:%S')}] {line}\n")
        sys.stdout.flush()
        self.send_response(204)
        self.end_headers()

    def log_message(self, fmt, *args):
        if os.environ.get("XT_SERVE_QUIET") or (args and "/__log" in str(args[0])):
            return
        super().log_message(fmt, *args)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir", nargs="?", default=os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--port", type=int, default=int(os.environ.get("PORT", "8080")))
    ap.add_argument("--host", default="127.0.0.1")
    args = ap.parse_args()
    if not os.path.isfile(os.path.join(args.dir, "xonotic-touch.wasm")):
        sys.exit(f"{args.dir} has no xonotic-touch.wasm: run scripts/build-web.sh first")
    handler = functools.partial(Handler, directory=args.dir)
    httpd = http.server.ThreadingHTTPServer((args.host, args.port), handler)
    print(f"Serving {args.dir} at http://{'localhost' if args.host in ('127.0.0.1', '::1') else args.host}:{args.port}/")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
