#!/usr/bin/env python3
"""Serve a built web bundle (build/web) for local testing.

    python3 web/serve.py [DIR] [--port 8080] [--host 127.0.0.1]

Browsers only allow the Cache API, pointer lock with raw input, fullscreen
keyboard lock and wake lock on secure origins; http://localhost counts as
one. To test on a phone on the same network, put the bundle behind https
(any static host or a local reverse proxy with a certificate).

Headers are vercel.json's, read from it (Cache-Control, CORS on data/,
the blobs' content type), and nothing else: no COOP/COEP, as on Vercel. Byte ranges are answered (206), as on Vercel: the page
resumes a cut-off chunk download with one. XT_SERVE_LOG=FILE appends every
request ("METHOD path status bytes") to FILE, for tests that count requests.
"""
import argparse
import functools
import http.server
import os
import re
import sys
import time

IMMUTABLE = re.compile(r"^/(a/|data/blobs/|data/m/)")
VERCEL_JSON = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "vercel.json")


def _vercel_rules():
    """vercel.json's "headers": (regex, [(key, value)]) in file order. Its
    sources are path-to-regexp patterns; the ones used here ("/(.*)",
    "/data/(.*)", "/(a|data/blobs|data/m)/(.*)") are regexes as written."""
    import json
    try:
        with open(VERCEL_JSON) as f:
            rules = json.load(f).get("headers", [])
    except OSError:
        rules = [{"source": "/(.*)", "headers": [{"key": "Cache-Control", "value": "no-cache"}]},
                 {"source": "/(a|data/blobs|data/m)/(.*)", "headers": [{"key": "Cache-Control", "value": "public, max-age=31536000, immutable"}]}]
    return [(re.compile("^" + r["source"] + "$"), [(h["key"], h["value"]) for h in r["headers"]]) for r in rules]


RULES = _vercel_rules()


def vercel_headers(path):
    """The headers Vercel sends for this path: every matching rule, a later
    rule replacing an earlier one's key (as Vercel does)."""
    out = {}
    for rx, headers in RULES:
        if rx.match(path):
            for k, v in headers:
                out[k.lower()] = (k, v)
    return list(out.values())

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
        for key, value in vercel_headers(path):
            if key.lower() == "content-type":
                continue  # set with the body, below
            self.send_header(key, value)
        self.send_header("Accept-Ranges", "bytes")
        etag = getattr(self, "_etag", None)
        if etag:
            self.send_header("ETag", etag)
            self._etag = None
        super().end_headers()

    def guess_type(self, path):
        url = getattr(self, "path", "").split("?", 1)[0]
        for key, value in vercel_headers(url):
            if key.lower() == "content-type":
                return value
        return super().guess_type(path)

    def send_head(self):
        # Revalidation by content (ETag), as Vercel does, not by modification
        # time: a build copied with its old timestamps must not answer 304
        # to a page from a newer one.
        path = self.translate_path(self.path)
        if os.path.isdir(path) and os.path.isfile(os.path.join(path, "index.html")):
            path = os.path.join(path, "index.html")
        if os.path.isfile(path) and not IMMUTABLE.match(self.path.split("?", 1)[0]):
            import hashlib
            with open(path, "rb") as f:
                etag = '"' + hashlib.sha1(f.read()).hexdigest() + '"'
            if "If-Modified-Since" in self.headers:
                del self.headers["If-Modified-Since"]
            if self.headers.get("If-None-Match") == etag:
                self.send_response(304)
                self.send_header("ETag", etag)
                self.end_headers()
                return None
            self._etag = etag
        rng = self.headers.get("Range")
        m = re.fullmatch(r"bytes=(\d+)-(\d*)", rng or "")
        path = self.translate_path(self.path)
        if not m or not os.path.isfile(path):
            return super().send_head()
        size = os.path.getsize(path)
        start = int(m.group(1))
        end = min(int(m.group(2)) if m.group(2) else size - 1, size - 1)
        if start >= size or end < start:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{size}")
            self.end_headers()
            return None
        f = open(path, "rb")
        f.seek(start)  # (a range of a whole file: answered by any build)
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Length", str(end - start + 1))
        self.end_headers()
        self._range_left = end - start + 1
        return f

    def copyfile(self, source, outputfile):
        left = getattr(self, "_range_left", None)
        if left is None:
            return super().copyfile(source, outputfile)
        while left > 0:
            block = source.read(min(1 << 20, left))
            if not block:
                break
            outputfile.write(block)
            left -= len(block)
        self._range_left = None

    def log_request(self, code="-", size="-"):
        log = os.environ.get("XT_SERVE_LOG")
        if log:
            with open(log, "a") as f:
                f.write(f"{self.command} {self.path} {code} {self.headers.get('Range') or ''}\n")
        super().log_request(code, size)

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
    if not os.path.isfile(os.path.join(args.dir, "index.html")):
        sys.exit(f"{args.dir} has no index.html: run scripts/build-web.sh first")
    handler = functools.partial(Handler, directory=args.dir)
    httpd = http.server.ThreadingHTTPServer((args.host, args.port), handler)
    print(f"Serving {args.dir} at http://{'localhost' if args.host in ('127.0.0.1', '::1') else args.host}:{args.port}/")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
