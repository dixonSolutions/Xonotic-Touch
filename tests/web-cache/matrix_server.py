#!/usr/bin/env python3
"""The data host for tests/web-cache/matrix.sh: web/serve.py (vercel.json's
headers, ranges) plus controls a test needs.

    python3 tests/web-cache/matrix_server.py ROOT --port P

GET /__ctl?root=DIR      serve another build from now on (a "deploy")
GET /__ctl?fault=MODE    MODE: none, 5xx:RATE (blob requests answer 503 at
                         that rate), drop:RATE (the body stops half way),
                         offline (every request is refused)
GET /__ctl?rate=BYTES    blobs are sent at BYTES a second per connection
                         (0: as fast as they go)
GET /__ctl?reset=1       zero the counters
GET /__stats             {"blobRequests", "blobBytes", "root", "fault"}
"""
import argparse
import importlib.util
import json
import os
import random
import socket
import sys
import threading
import time
import urllib.parse
import http.server

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("serve", os.path.join(here, "..", "..", "web", "serve.py"))
serve = importlib.util.module_from_spec(spec)
spec.loader.exec_module(serve)

STATE = {"root": "", "fault": "none", "rate": 0, "blobRequests": 0, "blobBytes": 0}
LOCK = threading.Lock()


class Handler(serve.Handler):
    def __init__(self, *a, **kw):
        kw["directory"] = STATE["root"]
        super().__init__(*a, **kw)

    def translate_path(self, path):
        # The root is looked up for every request, not once per connection:
        # a browser's kept-alive connection would go on serving the root it
        # was opened under after /__root switched it (Firefox does).
        self.directory = STATE["root"]
        return super().translate_path(path)

    def _json(self, obj):
        body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        http.server.BaseHTTPRequestHandler.end_headers(self)
        self.wfile.write(body)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        if url.path == "/__stats":
            with LOCK:
                return self._json(dict(STATE))
        if url.path == "/__ctl":
            q = urllib.parse.parse_qs(url.query)
            with LOCK:
                if "root" in q:
                    STATE["root"] = os.path.abspath(q["root"][0])
                if "fault" in q:
                    STATE["fault"] = q["fault"][0]
                if "rate" in q:
                    STATE["rate"] = int(q["rate"][0])
                if "reset" in q:
                    STATE["blobRequests"] = STATE["blobBytes"] = 0
                return self._json(dict(STATE))
        fault = STATE["fault"]
        if fault == "offline":
            self.connection.shutdown(socket.SHUT_RDWR)
            return
        blob = url.path.startswith("/data/blobs/")
        if blob:
            with LOCK:
                STATE["blobRequests"] += 1
            kind, _, rate = fault.partition(":")
            if kind == "5xx" and random.random() < float(rate or 0):
                self.send_error(503)
                return
            if kind == "drop" and random.random() < float(rate or 0):
                self._drop = True
        return super().do_GET()

    def copyfile(self, source, outputfile):
        # Count what is sent, and cut a body short for "drop".
        class Counter:
            def __init__(s, out):
                s.out = out
                s.n = 0

            def write(s, b):
                if getattr(self, "_drop", False) and s.n > (1 << 20):
                    raise ConnectionResetError("dropped")
                blob = self.path.startswith("/data/blobs/")
                rate = STATE["rate"] if blob else 0
                if rate:
                    # A slow connection: 8 KiB at a time.
                    for i in range(0, len(b), 8192):
                        piece = b[i:i + 8192]
                        s.out.write(piece)
                        time.sleep(len(piece) / rate)
                else:
                    s.out.write(b)
                s.n += len(b)
                if blob:
                    with LOCK:
                        STATE["blobBytes"] += len(b)

        try:
            super().copyfile(source, Counter(outputfile))
        except (ConnectionResetError, BrokenPipeError):
            self.close_connection = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--port", type=int, default=8790)
    args = ap.parse_args()
    STATE["root"] = os.path.abspath(args.root)
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"matrix server on http://127.0.0.1:{args.port}/ serving {STATE['root']}", flush=True)
    httpd.serve_forever()


if __name__ == "__main__":
    main()
