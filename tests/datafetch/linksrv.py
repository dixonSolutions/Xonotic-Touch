#!/usr/bin/env python3
"""A stand-in for the release storage, for scripts/test-datafetch.sh.

    linksrv.py DIR PORT [--mbit N] [--corrupt-once OFFSET] [--stats FILE]

Serves DIR with explicit byte ranges (a suffix range gets 501, as GitHub's
release storage answers), over one shared link of N Mbit/s (all connections
together, unlike tests/net/range_http.py's per-connection limit), optionally
flips one byte at an absolute offset of a .zip the first time it is served,
and keeps FILE up to date with "<path> <requests> <bytes sent>" lines.
"""
import os
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.realpath(sys.argv[1])
PORT = int(sys.argv[2])


def opt(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


RATE = float(opt("--mbit", 0)) * 1e6 / 8
CORRUPT = [int(opt("--corrupt-once", -1))]
STATS = opt("--stats")
lock = threading.Lock()
stats = {}
bucket = {"t": time.monotonic(), "tokens": 0.0}


def take(n):
    """Blocks until the shared link has room for n bytes."""
    if RATE <= 0:
        return
    while True:
        with lock:
            now = time.monotonic()
            bucket["tokens"] = min(RATE * 0.05, bucket["tokens"] + (now - bucket["t"]) * RATE)
            bucket["t"] = now
            if bucket["tokens"] >= n:
                bucket["tokens"] -= n
                return
            wait = (n - bucket["tokens"]) / RATE
        time.sleep(min(wait, 0.05))


def count(path, requests, sent):
    with lock:
        s = stats.setdefault(path, [0, 0])
        s[0] += requests
        s[1] += sent
        if STATS and (requests or sent == 0):
            with open(STATS + ".tmp", "w") as f:
                for k, v in sorted(stats.items()):
                    f.write("%s %d %d\n" % (k, v[0], v[1]))
            os.replace(STATS + ".tmp", STATS)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        rel = self.path.split("?")[0].lstrip("/")
        path = os.path.join(ROOT, rel)
        if ".." in rel or not os.path.isfile(path):
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        size = os.path.getsize(path)
        rng = self.headers.get("Range", "")
        if re.match(r"bytes=-\d+$", rng):
            self.send_response(501)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        m = re.match(r"bytes=(\d+)-(\d*)$", rng)
        start, end = 0, size - 1
        if m:
            start = int(m.group(1))
            end = min(int(m.group(2)) if m.group(2) else size - 1, size - 1)
            self.send_response(206)
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        else:
            self.send_response(200)
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        count(rel, 1, 0)
        sent = 0
        try:
            with open(path, "rb") as f:
                f.seek(start)
                left = end - start + 1
                while left > 0:
                    chunk = bytearray(f.read(min(16384, left)))
                    if not chunk:
                        break
                    at = start + sent
                    with lock:
                        c = CORRUPT[0]
                        if rel.endswith(".zip") and at <= c < at + len(chunk):
                            chunk[c - at] ^= 0xFF
                            CORRUPT[0] = -1
                    take(len(chunk))
                    self.wfile.write(chunk)
                    left -= len(chunk)
                    sent += len(chunk)
        except OSError:
            pass
        finally:
            count(rel, 0, sent)
            count(rel, 0, 0)


ThreadingHTTPServer.daemon_threads = True
ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
