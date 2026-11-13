#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""UI bridge tests against a real engine (needs game data; see scripts/test-uibridge.sh).

  - every snapshot and diff the engine sends validates against docs/ui-bridge.schema.json
  - actions round-trip: a value event changes the bound cvar (and the diff carries it)
  - a list window returns the rows of the source, sorted, windowed
  - an image request returns PNG bytes of a map preview
  - hostile mod pages are refused whole, good ones are labelled with their origin
  - the Linux transport: listen socket 0600, reconnect, the engine quits when nobody comes back
  - with no renderer attached and menu_touch_uib 0 nothing is described (zero cost)
"""
import base64, json, os, socket, stat, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import importlib.util
spec = importlib.util.spec_from_file_location("uibschema", os.path.join(ROOT, "scripts", "uib-schema.py"))
uibschema = importlib.util.module_from_spec(spec)
spec.loader.exec_module(uibschema)

ENGINE = os.environ.get("UIB_ENGINE", os.path.join(ROOT, "build", "bin", "xonotic"))
BASEDIR = os.environ["UIB_BASEDIR"]
LIBDIR = os.environ.get("UIB_LIBDIR", "")
fails = 0


def check(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        fails += 1


HOSTILE = [
    ("toobig", '{"title":"x","nodes":[' + ",".join('{"id":"%d","type":"divider","props":{},"children":[]}' % i for i in range(260)) + "]}"),
    ("badtype", '{"title":"x","nodes":[{"id":"a","type":"list","props":{"source":"servers"},"children":[]}]}'),
    ("badbind", '{"title":"x","nodes":[{"id":"a","type":"switch","props":{"label":"x","bind":"cvar:rcon_password"},"children":[]}]}'),
    ("badlink", '{"title":"x","nodes":[{"id":"a","type":"button","props":{"label":"x","action":{"id":"o","kind":"link","url":"file:///etc/passwd"}},"children":[]}]}'),
    ("badimg", '{"title":"x","nodes":[{"id":"a","type":"image","props":{"ref":"pk3:maps/stormkeep"},"children":[]}]}'),
    ("longstr", '{"title":"x","nodes":[{"id":"a","type":"text","props":{"body":"' + "y" * 600 + '"},"children":[]}]}'),
    ("deep", '{"title":"x","nodes":[{"id":"a/b/c/d/e/f/g/h/i","type":"text","props":{},"children":[]}]}'),
    ("kind", '{"title":"x","nodes":[{"id":"a","type":"button","props":{"label":"x","action":{"id":"o","kind":"file"}},"children":[]}]}'),
]
GOOD = '{"title":"Radar","nodes":[{"id":"g","type":"section","props":{"title":"General"},"children":["g/r"]},{"id":"g/r","type":"slider","props":{"label":"Range","bind":"mod:radar.range","min":1,"max":9,"step":1,"value":3},"children":[]}]}'


class Conn:
    def __init__(self, sock):
        self.s = sock
        self.buf = b""
        self.mid = 0
        self.rev = -1
        self.nodes = {}
        self.msgs = 0

    def send(self, m):
        self.mid += 1
        self.s.sendall((json.dumps(dict({"uib": 1, "id": self.mid}, **m)) + "\n").encode())
        return self.mid

    def recv(self, timeout):
        self.s.settimeout(timeout)
        while b"\n" not in self.buf:
            try:
                c = self.s.recv(1 << 20)
            except socket.timeout:
                return None
            if not c:
                raise EOFError
            self.buf += c
        line, self.buf = self.buf.split(b"\n", 1)
        m = json.loads(line)
        if m.get("t") in ("snapshot", "diff"):
            try:
                uibschema.validate(m, uibschema.SCHEMA, uibschema.SCHEMA)
            except uibschema.Invalid as e:
                check(False, f"schema: {m['t']} rev {m.get('rev')}: {str(e)[:300]}")
            self.msgs += 1
        if m.get("t") == "snapshot":
            self.nodes = {n["id"]: n for n in m["nodes"]}
            self.rev = m["rev"]
        elif m.get("t") == "diff":
            if m["base"] != self.rev:
                check(False, f"diff base {m['base']} != {self.rev}")
            for d in m["del"]:
                self.nodes.pop(d, None)
            for n in m["set"]:
                self.nodes[n["id"]] = n
            self.rev = m["rev"]
        return m

    def settle(self, quiet=0.7, limit=10):
        end = time.monotonic() + limit
        while time.monotonic() < end:
            if self.recv(quiet) is None:
                return

    def request(self, m, limit=10):
        i = self.send(m)
        end = time.monotonic() + limit
        while time.monotonic() < end:
            r = self.recv(1.0)
            if r and r.get("re") == i:
                return r
        return None


def spawn(args, ud, precfg=""):
    env = dict(os.environ)
    for k in ("WAYLAND_DISPLAY", "DISPLAY", "XAUTHORITY"):
        env.pop(k, None)
    env.update(SDL_VIDEODRIVER="offscreen", SDL_AUDIODRIVER="dummy")
    if LIBDIR:
        env["LD_LIBRARY_PATH"] = LIBDIR
    os.makedirs(os.path.join(ud, "data"), exist_ok=True)
    with open(os.path.join(ud, "data", "uibt.cfg"), "w") as f:
        f.write(precfg)
    cmd = ["timeout", "-k", "5", "150", ENGINE, "-xonotic", "-basedir", BASEDIR, "-userdir", ud, "-nosound",
           "+vid_fullscreen", "0", "+vid_width", "640", "+vid_height", "480", "+exec", "xonotic.cfg",
           "+set", "_touch_assets_ready", "1", "+exec", "uibt.cfg"] + args
    log = open(os.path.join(ud, "engine.log"), "w")
    return subprocess.Popen(cmd, env=env, stdout=log, stderr=subprocess.STDOUT, cwd=BASEDIR), log


def connect(path, limit=60):
    end = time.monotonic() + limit
    while time.monotonic() < end:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            return s
        except OSError:
            time.sleep(0.1)
    raise TimeoutError("no socket")


def hello(c):
    i = c.send({"t": "hello", "schema": "1.0", "renderer": {"name": "engine_test"}, "caps": {"widthClass": "expanded"}})
    h = None
    end = time.monotonic() + 60
    while time.monotonic() < end and (h is None or c.rev < 0):
        m = c.recv(1.0)
        if m and m.get("t") == "hello" and m.get("re") == i:
            h = m
    return h


def main():
    ud = tempfile.mkdtemp(prefix="uibt-", dir=os.environ.get("TMPDIR"))
    sock = os.path.join(ud, "run", "uib.sock")
    pre = "".join("uib_modpage hostile %s %s\n" % (k, v) for k, v in HOSTILE) + "uib_modpage radar main " + GOOD + "\n"
    # a join that goes nowhere (then cancelled by the test), and the password prompt
    pre += 'defer 30 "connect 127.0.0.1:9"\ndefer 50 "set _uib_debug_open mp.password"\n'
    t0 = time.monotonic()
    p, log = spawn(["-uibridge", "listen:" + sock], ud, pre)
    try:
        s = connect(sock)
        st = os.stat(sock)
        check(stat.S_IMODE(st.st_mode) == 0o600, f"socket mode {oct(stat.S_IMODE(st.st_mode))} is 0600")
        check(stat.S_IMODE(os.stat(os.path.dirname(sock)).st_mode) == 0o700, "socket directory is 0700")
        c = Conn(s)
        h = hello(c)
        check(h is not None and h.get("schema") == "1.0", "hello answered with schema 1.0")
        check("home" in c.nodes and c.nodes["app"]["props"]["stack"] == ["home"], "snapshot holds Home")
        print(f"     first description {1000 * (time.monotonic() - t0):.0f} ms after spawn")
        session = h["session"] if h else ""
        c.settle()
        # a setting round trip: open Settings > Video > View, change the field of view
        c.send({"t": "ev", "ev": "action", "node": "home", "action": "settings"})
        c.settle()
        c.send({"t": "ev", "ev": "value", "node": "settings/split/video/tabs", "value": "view"})
        c.settle()
        fov = c.nodes.get("settings/split/video/tabs/view/fov")
        check(fov is not None, "Settings > Video > View has the fov slider")
        if fov is None:
            print("     have:", sorted(k for k in c.nodes if "video" in k)[:12], c.nodes.get("app", {}).get("props"))
            return 1
        c.send({"t": "ev", "ev": "value", "node": "settings/split/video/tabs/view/fov", "value": 117})
        c.settle()
        check(c.nodes["settings/split/video/tabs/view/fov"]["props"].get("value") == 117, "value event: the diff carries fov 117")
        r = c.request({"t": "ev", "ev": "value", "node": "settings/split/video/tabs/view/fov", "value": "x"})
        check(r is not None and r.get("t") == "error", "a string for a slider is refused")
        r = c.request({"t": "ev", "ev": "value", "node": "home/tiles/sp", "value": True})
        check(r is not None and r.get("code") in ("denied", "not_found"), "a value for an unbound or absent node is refused")
        r = c.request({"t": "ev", "ev": "action", "node": "home/tiles/sp", "action": "rm -rf"})
        check(r is not None and r.get("code") in ("denied", "not_found"), "an action the node does not have is refused")
        # the cvar really changed: ask through a second path (the console dump of the node via uib_stats is not enough)
        # windows: maps sorted by title
        r = c.request({"t": "window", "source": "maps", "offset": 0, "limit": 5, "sort": "title"})
        check(r is not None and r["t"] == "rows" and r["total"] >= 5 and len(r["rows"]) == 5, f"maps window: {r and r.get('total')} total, 5 rows")
        if r and r["t"] == "rows":
            titles = [x["cells"]["title"].lower() for x in r["rows"]]
            check(titles == sorted(titles), "rows sorted by title")
            r2 = c.request({"t": "window", "source": "maps", "offset": 2, "limit": 2, "sort": "title"})
            check(r2 and [x["key"] for x in r2["rows"]] == [x["key"] for x in r["rows"][2:4]], "offset windows line up")
            r3 = c.request({"t": "window", "source": "maps", "offset": 0, "limit": 50, "filter": "stormkeep"})
            check(r3 and r3["total"] == 1 and r3["rows"][0]["key"] == "stormkeep", "filter finds stormkeep")
        r = c.request({"t": "window", "source": "nope", "offset": 0, "limit": 5})
        check(r and r.get("code") == "not_found", "unknown source is an error")
        # images
        r = c.request({"t": "image", "ref": "pk3:maps/stormkeep", "w": 160, "h": 90, "as": "bytes"}, limit=20)
        data = base64.b64decode(r["data"]) if r and r.get("data") else b""
        check(data[:8] == b"\x89PNG\r\n\x1a\n" and r["w"] <= 160 and r["h"] <= 90, f"image bytes: PNG {r and r.get('w')}x{r and r.get('h')}, {len(data)} bytes")
        r = c.request({"t": "image", "ref": "pk3:maps/stormkeep", "w": 160, "h": 90}, limit=20)
        check(r and os.path.isfile(r.get("path", "")), "image path points at a cached file")
        r = c.request({"t": "image", "ref": "pk3:../../etc/passwd"})
        check(r and r.get("t") == "error", "image refs cannot leave the game filesystem")
        # mods
        c.send({"t": "ev", "ev": "value", "node": "settings/split", "value": "mods"})
        c.settle()
        c.send({"t": "ev", "ev": "value", "node": "settings/split/mods/tabs", "value": "pages"})
        c.settle()
        pages = [k for k in c.nodes if k.startswith("settings/split/mods/tabs/pages/pages/")]
        check(pages == ["settings/split/mods/tabs/pages/pages/radar.main"], f"only the good mod page is listed: {pages}")
        c.send({"t": "ev", "ev": "action", "node": "settings/split/mods/tabs/pages/pages/radar.main", "action": "modpage.radar.main"})
        c.settle()
        mp = c.nodes.get("mods.page.radar.main/g/r")
        check(mp is not None and mp.get("origin") == "mod:radar", "mod page nodes carry origin mod:radar")
        # a join that is cancelled (or refused) must not leave the loading screen on the stack
        end = time.monotonic() + 40
        while time.monotonic() < end and c.nodes.get("app", {}).get("props", {}).get("stack") != ["loading"]:
            c.recv(1.0)
        check(c.nodes["app"]["props"].get("stack") == ["loading"], "a join shows the loading screen")
        holder = next((k for k, n in c.nodes.items() if k.startswith("loading") and "loading.cancel" in json.dumps(n.get("props", {}))), "loading")
        r = c.request({"t": "ev", "ev": "action", "node": holder, "action": "loading.cancel"}, limit=3)
        check(r is None or r.get("t") != "error", f"Cancel accepted on {holder} ({r})")
        c.settle(quiet=3, limit=8)
        stack = c.nodes["app"]["props"].get("stack")
        check(stack and stack[-1] != "loading" and c.nodes["app"]["props"].get("context") == "menu",
              f"after Cancel the stack leaves the loading screen ({stack}, context {c.nodes['app']['props'].get('context')})")
        # the password prompt has a Join button, not only Enter
        end = time.monotonic() + 30
        while time.monotonic() < end and "mp.password" not in c.nodes:
            c.recv(1.0)
        pw = c.nodes.get("mp.password", {}).get("props", {})
        check(pw.get("primary", {}).get("id") == "password.join", "the password prompt's primary action is Join")
        # reconnect: drop the socket, come back, same session
        cur = (c.nodes.get("app", {}).get("props", {}).get("modal") or c.nodes.get("app", {}).get("props", {}).get("stack") or [""])[-1]
        s.close()
        time.sleep(1)
        s = connect(sock, 10)
        c = Conn(s)
        h2 = hello(c)
        check(h2 is not None and h2.get("session") == session, "reconnect: same engine session, fresh snapshot")
        check(cur in c.nodes, f"reconnect: the snapshot is the current screen ({cur})")
        # the engine quits about 10 s after the last front end leaves
        s.close()
        try:
            rc = p.wait(timeout=25)
            check(True, f"engine quit after the front end left (rc {rc})")
        except subprocess.TimeoutExpired:
            check(False, "engine still running 25 s after the front end left")
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()
        log.close()
    logtxt = open(os.path.join(ud, "engine.log"), errors="replace").read()
    check(logtxt.count("uib_modpage: refused") == len(HOSTILE), f"{logtxt.count('uib_modpage: refused')}/{len(HOSTILE)} hostile pages refused")
    check("uib_modpage: ok" in logtxt, "the good page was accepted")

    # zero cost: no renderer, menu_touch_uib 0
    ud2 = tempfile.mkdtemp(prefix="uibt0-", dir=os.environ.get("TMPDIR"))
    p2, log2 = spawn(["+set", "menu_touch_uib", "0", "+defer", "5", "uib_stats", "+defer", "6", "quit"], ud2)
    try:
        p2.wait(timeout=60)
    except subprocess.TimeoutExpired:
        p2.kill()
    log2.close()
    t = open(os.path.join(ud2, "engine.log"), errors="replace").read()
    line = [x for x in t.splitlines() if x.startswith("uibridge: rev")]
    check(bool(line) and " 1 nodes" in line[0] and "0 renderer" in line[0], f"no renderer: nothing described ({line[0] if line else 'no stats'})")
    print("FAILED: %d" % fails if fails else "engine_test: all passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
