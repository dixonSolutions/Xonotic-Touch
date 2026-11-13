#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Play a UI bridge fixture over the Linux stream transport, standing in for the engine.

    scripts/uib-replay.py --listen /run/user/1000/godnotic/uibridge.sock tests/uibridge/fixtures/home.json
    scripts/uib-replay.py --stdio tests/uibridge/fixtures/seq-server-list.json   # spawned by the app
    scripts/uib-replay.py --fd 3 tests/uibridge/fixtures/seq-download.json      # inherited socketpair end

Protocol: docs/UI_BRIDGE.md. On `hello` it answers `hello` and the fixture's snapshot, then
sends the fixture's steps with their delays. It answers `window` from the fixture's rows
(filtered by substring, sorted, windowed), `image` with the path of a generated
placeholder PNG of the requested size, `sync` with a snapshot of the current state,
`ping` with `pong`. `value` events are applied to the bound node and confirmed with a
diff, like the engine does. Every message received is printed to stderr.
Options: --loop (restart the steps after the last one), --speed X (delay multiplier),
--quiet.
"""
import argparse, base64, copy, json, os, select, socket, stat, struct, sys, time, zlib

SCHEMA = "1.0"


def png(w, h):
    # A small Godnotic-navy gradient with an electric-blue frame: obviously a placeholder.
    w, h = max(1, min(w, 2048)), max(1, min(h, 2048))
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            edge = x < 2 or y < 2 or x >= w - 2 or y >= h - 2
            if edge:
                raw += bytes((0x2f, 0x8c, 0xff))
            else:
                t = y / max(1, h - 1)
                raw += bytes((int(12 + 18 * t), int(20 + 26 * t), int(42 + 40 * t)))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


class Conn:
    def __init__(self, rfd, wfd, log):
        self.rfd, self.wfd, self.buf, self.log = rfd, wfd, b"", log

    def send(self, msg):
        data = (json.dumps(msg, ensure_ascii=False, separators=(",", ":")) + "\n").encode()
        view = memoryview(data)
        while view:
            try:
                n = os.write(self.wfd, view)
            except BlockingIOError:
                select.select([], [self.wfd], [], 1.0)
                continue
            view = view[n:]

    def recv(self, timeout):
        """Messages available within timeout; None on EOF."""
        r, _, _ = select.select([self.rfd], [], [], max(0, timeout))
        if not r:
            return []
        try:
            chunk = os.read(self.rfd, 65536)
        except ConnectionResetError:
            return None
        if not chunk:
            return None
        self.buf += chunk
        out = []
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            if line.strip():
                try:
                    out.append(json.loads(line))
                except ValueError as e:
                    out.append({"_bad": str(e)})
        return out


class Player:
    def __init__(self, fx, args):
        self.fx, self.args = fx, args
        self.session = os.urandom(6).hex()
        self.imgdir = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or "/tmp", f"uib-replay-{os.getpid()}")
        os.makedirs(self.imgdir, exist_ok=True)
        self.reset()

    def reset(self):
        snap = self.fx["snapshot"]
        self.nodes = {n["id"]: copy.deepcopy(n) for n in snap["nodes"]}
        self.order = [n["id"] for n in snap["nodes"]]
        self.rev = snap["rev"]
        self.step = 0
        self.next_at = None
        self.msgid = 0
        self.src_rev = {}

    def mid(self):
        self.msgid += 1
        return self.msgid

    def snapshot(self):
        nodes, stack = [], ["app"]
        while stack:
            i = stack.pop()
            n = self.nodes.get(i)
            if not n:
                continue
            nodes.append(n)
            stack.extend(reversed(n.get("children", [])))
        return {"uib": 1, "t": "snapshot", "id": self.mid(), "rev": self.rev, "root": "app", "nodes": nodes}

    def apply(self, m):
        if m["t"] == "diff":
            if m["base"] != self.rev:  # fixture steps are relative to the fixture's own revs
                m = dict(m, base=self.rev, rev=self.rev + 1)
            for d in m["del"]:
                self.nodes.pop(d, None)
            for n in m["set"]:
                self.nodes[n["id"]] = copy.deepcopy(n)
            self.rev = m["rev"]
        elif m["t"] == "snapshot":
            self.nodes = {n["id"]: copy.deepcopy(n) for n in m["nodes"]}
            self.rev = m["rev"]
        elif m["t"] == "source":
            self.src_rev[m["source"]] = m["rev"]
            self.src_total = getattr(self, "src_total", {})
            self.src_total[m["source"]] = m["total"]
        return dict(m, id=self.mid())

    def rows(self, req):
        src = req.get("source", "")
        allrows = self.fx.get("rows", {}).get(src)
        if allrows is None:
            return {"uib": 1, "t": "error", "id": self.mid(), "re": req.get("id", 0), "code": "not_found",
                    "msg": f"no rows for source {src} in this fixture"}
        total_cap = getattr(self, "src_total", {}).get(src)
        rows = allrows[:total_cap] if total_cap is not None else list(allrows)
        flt = (req.get("filter") or "").lower()
        if flt:
            rows = [r for r in rows if any(flt in str(v).lower() for v in r["cells"].values())]
        sort = req.get("sort")
        if sort:
            rows.sort(key=lambda r: (isinstance(r["cells"].get(sort), str), r["cells"].get(sort, "")),
                      reverse=bool(req.get("desc")))
        off, lim = int(req.get("offset", 0)), min(200, int(req.get("limit", 50)))
        out = {"uib": 1, "t": "rows", "id": self.mid(), "re": req.get("id", 0), "source": src,
               "rev": self.src_rev.get(src, 1), "total": len(rows), "offset": off, "rows": rows[off:off + lim]}
        for k in ("sort", "desc", "filter"):
            if k in req:
                out[k] = req[k]
        return out

    def image(self, req):
        w, h = int(req.get("w") or 320), int(req.get("h") or 180)
        data = png(min(w, 512), min(h, 512))
        out = {"uib": 1, "t": "image", "id": self.mid(), "re": req.get("id", 0), "ref": req.get("ref", "asset:placeholder"),
               "mime": "image/png", "w": min(w, 512), "h": min(h, 512)}
        if req.get("as") == "bytes":
            out["data"] = base64.b64encode(data).decode()
        else:
            p = os.path.join(self.imgdir, f"{out['w']}x{out['h']}.png")
            if not os.path.exists(p):
                with open(p, "wb") as f:
                    f.write(data)
            out["path"] = p
        return out

    def value(self, ev):
        n = self.nodes.get(ev.get("node", ""))
        if not n or "bind" not in n.get("props", {}):
            return {"uib": 1, "t": "error", "id": self.mid(), "re": ev.get("id", 0), "code": "denied",
                    "msg": "no bound control with that id"}
        n = copy.deepcopy(n)
        n["props"]["value"] = ev.get("value")
        if n["type"] == "tabs":  # a tab switch: the engine would rebuild the panes; we only echo
            pass
        self.nodes[n["id"]] = n
        self.rev += 1
        return {"uib": 1, "t": "diff", "id": self.mid(), "base": self.rev - 1, "rev": self.rev, "set": [n], "del": []}

    def handle(self, m, conn):
        t = m.get("t")
        if "_bad" in m:
            conn.send({"uib": 1, "t": "error", "id": self.mid(), "code": "bad_request", "msg": m["_bad"]})
        elif m.get("uib") != 1:
            conn.send({"uib": 1, "t": "error", "id": self.mid(), "re": m.get("id", 0), "code": "version",
                       "msg": "this player speaks uib 1"})
        elif t == "hello":
            self.reset()
            conn.send({"uib": 1, "t": "hello", "id": self.mid(), "re": m.get("id", 0), "schema": SCHEMA,
                       "engine": "uib-replay (fixture " + self.fx.get("name", "?") + ")", "session": self.session,
                       "limits": {"message": 4194304, "string": 4096, "nodes": 8192, "depth": 32, "window": 200},
                       "features": ["rows", "image.path", "image.bytes", "replay"]})
            conn.send(self.snapshot())
            self.next_at = time.monotonic()
        elif t == "sync":
            conn.send(self.snapshot())
        elif t == "window":
            conn.send(self.rows(m))
        elif t == "image":
            conn.send(self.image(m))
        elif t == "ping":
            conn.send({"uib": 1, "t": "pong", "id": self.mid(), "re": m.get("id", 0)})
        elif t == "ev" and m.get("ev") == "value":
            conn.send(self.value(m))
        elif t == "bye":
            return False
        return True

    def due(self, conn):
        steps = self.fx.get("steps", [])
        if self.next_at is None:
            return None
        while self.step < len(steps):
            st = steps[self.step]
            when = self.next_at + st.get("delay", 0) * self.args.speed
            now = time.monotonic()
            if now < when:
                return when - now
            conn.send(self.apply(copy.deepcopy(st["msg"])))
            if st.get("note") and not self.args.quiet:
                print(f"[replay] step {self.step}: {st['note']}", file=sys.stderr)
            self.step += 1
            self.next_at = when
        if self.args.loop and steps:
            self.reset_steps(conn)
            return 0.5
        return None

    def reset_steps(self, conn):
        self.reset()
        conn.send(self.snapshot())
        self.next_at = time.monotonic() + 1.0


def serve(conn, player, args):
    while True:
        timeout = player.due(conn)
        msgs = conn.recv(1.0 if timeout is None else timeout)
        if msgs is None:
            return
        for m in msgs:
            if not args.quiet:
                print("[replay] <-", json.dumps(m)[:400], file=sys.stderr)
            if not player.handle(m, conn):
                return


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--listen", metavar="PATH", nargs="?", const="")
    g.add_argument("--stdio", action="store_true")
    g.add_argument("--fd", type=int)
    ap.add_argument("--loop", action="store_true")
    ap.add_argument("--speed", type=float, default=1.0)
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("fixture")
    args = ap.parse_args()
    with open(args.fixture, encoding="utf-8") as f:
        fx = json.load(f)
    player = Player(fx, args)
    if args.stdio:
        serve(Conn(0, 1, args), player, args)
        return
    if args.fd is not None:
        serve(Conn(args.fd, args.fd, args), player, args)
        return
    path = args.listen or os.path.join(os.environ.get("XDG_RUNTIME_DIR") or "/tmp", "godnotic", "uibridge.sock")
    os.makedirs(os.path.dirname(path), mode=0o700, exist_ok=True)
    if os.path.exists(path) and stat.S_ISSOCK(os.stat(path).st_mode):
        os.unlink(path)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    old = os.umask(0o177)
    srv.bind(path)
    os.umask(old)
    srv.listen(1)
    print(f"[replay] listening on {path}", file=sys.stderr)
    try:
        while True:
            c, _ = srv.accept()
            print("[replay] client connected", file=sys.stderr)
            serve(Conn(c.fileno(), c.fileno(), args), player, args)
            c.close()
            print("[replay] client gone; waiting for the next", file=sys.stderr)
    except KeyboardInterrupt:
        pass
    finally:
        os.unlink(path)


if __name__ == "__main__":
    main()
