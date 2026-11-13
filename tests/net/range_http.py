#!/usr/bin/env python3
"""A small HTTP file server with Range support and an optional per-connection
speed limit, for scripts/bench-download.sh (Python's own http.server has no
Range support, and real map hosts often cap each connection).

    range_http.py DIR PORT [PER_CONNECTION_KBPS] [--no-ranges]
"""
import os
import re
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.abspath(sys.argv[1])
PORT = int(sys.argv[2])
RATE = float(sys.argv[3]) * 1024 if len(sys.argv) > 3 and not sys.argv[3].startswith("--") and float(sys.argv[3]) > 0 else 0
RANGES = "--no-ranges" not in sys.argv


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (time.strftime("%H:%M:%S"), fmt % args))

    def _file(self):
        path = os.path.abspath(os.path.join(ROOT, self.path.split("?")[0].lstrip("/")))
        if not path.startswith(ROOT + os.sep) or not os.path.isfile(path):
            self.send_error(404)
            return None
        return path

    def do_HEAD(self):
        path = self._file()
        if path:
            self.send_response(200)
            self.send_header("Content-Length", str(os.path.getsize(path)))
            self.send_header("Accept-Ranges", "bytes" if RANGES else "none")
            self.end_headers()

    def do_GET(self):
        path = self._file()
        if not path:
            return
        size = os.path.getsize(path)
        start, end = 0, size - 1
        m = re.match(r"bytes=(\d*)-(\d*)$", self.headers.get("Range", "")) if RANGES else None
        if m and (m.group(1) or m.group(2)):
            if m.group(1):
                start = int(m.group(1))
                end = int(m.group(2)) if m.group(2) else size - 1
            else:
                start = size - int(m.group(2))
            end = min(end, size - 1)
            if start > end or start >= size:
                self.send_response(416)
                self.send_header("Content-Range", "bytes */%d" % size)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.send_response(206)
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        else:
            self.send_response(200)
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes" if RANGES else "none")
        self.end_headers()
        left = end - start + 1
        t0 = time.time()
        sent = 0
        with open(path, "rb") as f:
            f.seek(start)
            while left > 0:
                chunk = f.read(min(65536, left))
                if not chunk:
                    break
                try:
                    self.wfile.write(chunk)
                except OSError:
                    return
                left -= len(chunk)
                sent += len(chunk)
                if RATE:
                    ahead = sent / RATE - (time.time() - t0)
                    if ahead > 0:
                        time.sleep(ahead)


ThreadingHTTPServer.daemon_threads = True
ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
