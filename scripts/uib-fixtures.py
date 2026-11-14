#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Drive a real engine over the UI bridge stream transport and write (or check) fixtures.

    scripts/uib-fixtures.py --engine build/bin/xonotic --basedir DIR [--write | --check] [--report FILE]

The engine runs headless (SDL offscreen, no display, no sound) as the UI back end
(-uibridge fd:3 on a socketpair); this script is the renderer. It walks every screen
by sending the same events a renderer sends, saves each snapshot with the rows of its
lists, records diff sequences, and measures start time and message sizes.
--check compares with tests/uibridge/fixtures and fails on drift, ignoring volatile
fields (session, message ids, engine version, pings, times). Needs game data, so it is
not a CI job; scripts/test-uibridge.sh runs it when data is there.

The run is the same on every machine: a new user directory and a fixed environment
(no HOME configuration, LANG=C, TZ=UTC), the engine's own defaults (the package's
xonotic.cfg is not read, so the fixtures do not depend on which basedir staged it),
no network (a private network namespace where the system allows one, and no master
servers or LAN search in any case, so the server list is empty everywhere), and the
diffs one event produced are folded into one (how many frames the engine took to
describe a screen is timing, not content).
"""
import argparse, copy, json, os, socket, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(ROOT, "tests", "uibridge", "fixtures")

CAPS = {"nodes": [], "hints": [], "width": 1280, "height": 720, "dpi": 96, "widthClass": "expanded",
        "heightClass": "medium", "input": ["mouse", "keyboard"], "locale": "en", "reducedMotion": False,
        "powerSave": False, "thermal": "nominal", "insets": {"top": 0, "right": 0, "bottom": 0, "left": 0}}


class Engine:
    def __init__(self, args, extra=(), precfg=""):
        self.args = args
        self.ud = tempfile.mkdtemp(prefix="uib-fx-")
        os.makedirs(os.path.join(self.ud, "data"), exist_ok=True)
        if precfg:
            with open(os.path.join(self.ud, "data", "uib_pre.cfg"), "w") as f:
                f.write(precfg)
            extra = ["+exec", "uib_pre.cfg"] + list(extra)
        a, b = socket.socketpair()
        self.sock = a
        # a fixed environment: nothing of this machine's configuration reaches the engine
        env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": self.ud, "XDG_CONFIG_HOME": self.ud,
               "XDG_DATA_HOME": self.ud, "XDG_RUNTIME_DIR": self.ud, "LANG": "C", "LC_ALL": "C", "TZ": "UTC",
               "SDL_VIDEODRIVER": "offscreen", "SDL_AUDIODRIVER": "dummy"}
        if args.libdir:
            env["LD_LIBRARY_PATH"] = args.libdir
        cmd = ["timeout", "-k", "5", str(args.timeout), args.engine, "-xonotic", "-basedir", args.basedir,
               "-userdir", self.ud, "-nosound", "+vid_fullscreen", "0", "+vid_width", "1280", "+vid_height", "720",
               "+set", "_touch_assets_ready", "1", "+set", "vid_touchscreen", "0",
               # no network: no master servers, no LAN search, no remembered servers, no update check
               "+set", "net_slist_lan_only", "1", "+set", "net_slist_lan_ports", "0", "+set", "net_slist_lan_sweeps", "0",
               "+set", "net_slist_lan_tailscale", "0", "+set", "net_slist_cache", "0", "+set", "cl_curl_enabled", "0",
               "+set", "touch_lan_hostname", "Godnotic server",
               "-uibridge", f"fd:{b.fileno()}"] + list(extra)
        # ... and where the system allows it, no network interface at all
        if subprocess.run(["unshare", "-rn", "true"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
            cmd = ["unshare", "-rn"] + cmd
        self.t0 = time.monotonic()
        self.log = open(os.path.join(self.ud, "engine.log"), "w")
        self.proc = subprocess.Popen(cmd, env=env, pass_fds=[b.fileno()], stdout=self.log, stderr=subprocess.STDOUT,
                                     cwd=args.basedir)
        b.close()
        self.buf = b""
        self.mid = 0
        self.nodes = {}
        self.rev = -1
        self.sizes = {"snapshot": [], "diff": []}
        self.diffs = []
        self.sources = {}

    def send(self, m):
        self.mid += 1
        m = dict({"uib": 1, "id": self.mid}, **m)
        self.sock.sendall((json.dumps(m) + "\n").encode())
        return self.mid

    def recv(self, timeout):
        self.sock.settimeout(timeout)
        while b"\n" not in self.buf:
            try:
                chunk = self.sock.recv(1 << 20)
            except socket.timeout:
                return None
            if not chunk:
                raise EOFError("engine closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        m = json.loads(line)
        self.apply(m, len(line))
        return m

    def apply(self, m, size):
        t = m.get("t")
        if t == "snapshot":
            self.nodes = {n["id"]: n for n in m["nodes"]}
            self.rev = m["rev"]
            self.sizes["snapshot"].append(size)
        elif t == "diff":
            if m["base"] != self.rev:
                raise AssertionError(f"diff base {m['base']} but held rev {self.rev}")
            for d in m["del"]:
                self.nodes.pop(d, None)
            for n in m["set"]:
                self.nodes[n["id"]] = n
            self.rev = m["rev"]
            self.sizes["diff"].append(size)
            self.diffs.append(m)
        elif t == "source":
            self.sources[m["source"]] = m

    def settle(self, quiet=0.35, limit=8.0):
        """Read until nothing arrives for `quiet` seconds."""
        end = time.monotonic() + limit
        out = []
        while time.monotonic() < end:
            m = self.recv(quiet)
            if m is None:
                break
            out.append(m)
        return out

    def request(self, m, limit=10.0):
        i = self.send(m)
        end = time.monotonic() + limit
        while time.monotonic() < end:
            r = self.recv(limit)
            if r is None:
                break
            if r.get("re") == i:
                return r
        raise TimeoutError(f"no reply to {m}")

    def hello(self):
        i = self.send({"t": "hello", "schema": "1.0", "renderer": {"name": "uib-fixtures", "version": "1"}, "caps": CAPS})
        hello = snap = None
        end = time.monotonic() + self.args.timeout
        while time.monotonic() < end and not (hello and snap):
            m = self.recv(1.0)
            if m and m.get("t") == "hello" and m.get("re") == i:
                hello = m
                self.hello_ms = (time.monotonic() - self.t0) * 1000
            elif m and m.get("t") == "snapshot":
                snap = m
                self.snap_ms = (time.monotonic() - self.t0) * 1000
        if not snap:
            raise TimeoutError("no snapshot")
        return hello, snap

    def ev(self, **kw):
        self.send(dict({"t": "ev"}, **kw))
        return self.settle()

    def take_diff(self):
        """The diffs received since the last call, folded into one (None if there were none)."""
        ds, self.diffs = self.diffs, []
        if not ds:
            return None
        sets, dels = {}, []
        for d in ds:
            for i in d["del"]:
                sets.pop(i, None)
                if i not in dels:
                    dels.append(i)
            for n in d["set"]:
                sets[n["id"]] = n
                if n["id"] in dels:
                    dels.remove(n["id"])
        # parents before children, as the engine sends them
        order = sorted(sets.values(), key=lambda n: (n["id"] != "app", n["id"].count("/"), n["id"]))
        return {"uib": 1, "t": "diff", "id": ds[-1]["id"], "base": ds[0]["base"], "rev": ds[-1]["rev"], "set": order, "del": dels}

    def snapshot_msg(self):
        nodes, stack = [], ["app"]
        while stack:
            i = stack.pop()
            n = self.nodes[i]
            nodes.append(n)
            stack.extend(reversed(n["children"]))
        # rev 1: the engine's own counter depends on how many frames it took to get here
        return {"uib": 1, "t": "snapshot", "id": 2, "rev": 1, "root": "app", "nodes": copy.deepcopy(nodes)}

    def lists(self):
        out = {}
        for n in self.nodes.values():
            if n["type"] == "list":
                src = n["props"]["source"]
                r = self.request({"t": "window", "source": src, "offset": 0, "limit": 200})
                if r.get("t") == "rows":
                    out[src] = r["rows"]
        return out

    def close(self):
        try:
            self.send({"t": "bye", "reason": "done"})
            self.sock.close()
            self.proc.wait(timeout=15)
        except Exception:
            self.proc.kill()
            self.proc.wait()
        self.log.close()


def renumber(base, steps):
    """Folded diffs chain from the base snapshot: base, base+1, ..."""
    rev = base["rev"]
    for i, st in enumerate(steps):
        st["msg"]["id"] = 3 + i
        st["msg"]["base"] = rev
        rev += 1
        st["msg"]["rev"] = rev
    return steps


def fixture(e, name, desc, rows=True, steps=None, base=None):
    f = {"name": name, "description": desc, "generated": "engine", "caps": CAPS,
         "snapshot": base or e.snapshot_msg()}
    if steps:
        f["steps"] = steps
    if rows:
        r = e.lists()
        if r:
            f["rows"] = r
    return f


VOLATILE = {"session", "id", "re", "engine"}


def scrub(x, key=None):
    if isinstance(x, dict):
        return {k: scrub(v, k) for k, v in x.items() if k not in VOLATILE and k not in ("ping", "when", "rev", "base")}
    if isinstance(x, list):
        return [scrub(v) for v in x]
    return x


MODPAGE = ('uib_modpage radar main {"title":"Radar","nodes":[{"id":"general","type":"section","props":{"title":"General"},'
           '"children":["general/range","general/teammates"]},{"id":"general/range","type":"slider","props":{"label":"Range",'
           '"bind":"mod:radar.range","min":500,"max":8000,"step":100,"value":2000},"children":[]},{"id":"general/teammates",'
           '"type":"switch","props":{"label":"Show teammates","bind":"mod:radar.teammates","value":true},"children":[]}]}\n')


# The fixtures must not depend on which public servers answer today: no master is asked
# and nothing is shown from a saved list (docs/NETWORK.md, "The server list").
NO_NETWORK = "".join('%s ""\n' % m for m in ("sv_master1", "sv_master2", "sv_master3", "sv_master4",
                                               "sv_masterextra1", "sv_masterextra2", "sv_masterextra3")) + "net_slist_saved 0\n"


def run(args):
    e = Engine(args, precfg=NO_NETWORK + MODPAGE)
    out = {}
    report = {}
    try:
        hello, snap = e.hello()
        report["hello_ms_after_spawn"] = round(e.hello_ms)
        report["snapshot_ms_after_spawn"] = round(e.snap_ms)
        e.settle()
        out["home"] = fixture(e, "home", "Home: the banner and exactly three cards (Singleplayer, Campaign, Multiplayer); Settings, Credits and Quit are header actions.")
        # navigate: Home -> Singleplayer (recorded) -> back
        base = e.snapshot_msg()
        e.take_diff()
        e.ev(ev="action", node="home/tiles/sp", action="sp")
        out["singleplayer"] = fixture(e, "singleplayer", "Singleplayer: a local game. Mode cards, the map picker over maps:<mode> (two rows of tiles scrolling sideways; tap chooses, double tap plays), match options, recent.")
        nav = [{"delay": 0.5, "msg": e.take_diff()}]
        e.ev(ev="back")
        nav += [{"delay": 1.0, "msg": e.take_diff()}]
        out["seq-navigate"] = fixture(e, "seq-navigate", "Home, push Singleplayer, back to Home (engine diffs, one per event).", rows=False, steps=renumber(base, nav), base=base)
        # the map picker: a tap chooses (row/select), the preview and the list follow
        e.ev(ev="action", node="home/tiles/sp", action="sp")
        base = e.snapshot_msg()
        e.take_diff()
        e.ev(ev="row", node="sp/maps/list", row="xoylent", kind="select")
        steps = [{"delay": 0.5, "msg": e.take_diff(), "note": "row select xoylent"}]
        e.ev(ev="row", node="sp/maps/list", row="stormkeep", kind="select")
        steps += [{"delay": 0.5, "msg": e.take_diff(), "note": "row select stormkeep"}]
        out["seq-map-select"] = fixture(e, "seq-map-select", "Map picker: taps on Xoylent and Stormkeep send row/select; the engine answers with "
                                        "the list's selected row. A double tap sends row/activate on the chosen row and starts the match.", steps=renumber(base, steps), base=base)
        # map details
        e.ev(ev="row", node="sp/maps/list", row="stormkeep", kind="action", action="map.preview")
        out["map-preview"] = fixture(e, "map-preview", "Map details page pushed over Singleplayer.")
        e.ev(ev="back")
        e.ev(ev="back")
        # campaign
        e.ev(ev="action", node="home/tiles/campaign", action="campaign")
        out["campaign"] = fixture(e, "campaign", "Campaign: the chosen level beside its picture, the levels as cards (won, next, locked).")
        e.ev(ev="back")
        # servers
        e.ev(ev="action", node="home/tiles/mp", action="mp")
        out["servers"] = fixture(e, "servers", "Servers: search, checkbox row (LAN, Empty, Full, Laggy), Refresh, dense list, address, Create, Join.")
        e.ev(ev="value", node="mp/bar/filters/lan", value=True)
        out["servers-lan"] = fixture(e, "servers-lan", "Servers with LAN ticked.")
        e.ev(ev="value", node="mp/bar/filters/lan", value=False)
        # Create (from Multiplayer) with LAN + password
        e.ev(ev="action", node="mp/join/create", action="create")
        out["create"] = fixture(e, "create", "Create a game, LAN unticked: the same local game as Singleplayer; only the LAN checkbox is about the network.")
        e.ev(ev="value", node="sp/match/form/lan", value=True)
        e.ev(ev="value", node="sp/match/form/pwon", value=True)
        e.ev(ev="value", node="sp/match/form/password", value="hunter2")
        out["singleplayer-create-lan"] = fixture(e, "singleplayer-create-lan", "Create a game with LAN ticked and a password: Player slots, Port and Password inline, closed lock; the password never appears.")
        e.ev(ev="value", node="sp/match/form/pwon", value=False)
        e.ev(ev="value", node="sp/match/form/lan", value=False)
        e.ev(ev="back")
        e.ev(ev="back")
        # settings: vertical tabs, a setting changing (recorded), sub-tabs
        e.ev(ev="action", node="home", action="settings")
        out["settings"] = fixture(e, "settings", "Settings: vertical tabs (split) with Video selected and its horizontal sub-tabs.")
        e.ev(ev="value", node="settings/split/video/tabs", value="view")
        base = e.snapshot_msg()
        e.take_diff()
        e.ev(ev="value", node="settings/split/video/tabs/view/fov", value=110)
        steps = [{"delay": 0.5, "msg": e.take_diff(), "note": "fov 110"}]
        e.ev(ev="value", node="settings/split/video/tabs/view/fov", value=100)
        steps += [{"delay": 0.5, "msg": e.take_diff(), "note": "fov 100"}]
        out["seq-setting"] = fixture(e, "seq-setting", "Field of view changed twice: value diffs from the engine.", rows=False, steps=renumber(base, steps), base=base)
        for sec in ("audio", "game", "input", "touch", "esp", "player", "privacy", "updates", "data", "menu", "mods"):
            e.ev(ev="value", node="settings/split", value=sec)
            out[f"settings-{sec}"] = fixture(e, f"settings-{sec}", f"Settings > {sec}.")
        # a mod page (registered at start through uib_modpage, the path the Lua API shares)
        e.ev(ev="value", node="settings/split/mods/tabs", value="pages")
        out["settings-mods-pages"] = fixture(e, "settings-mods-pages", "Settings > Mods > Pages from mods.")
        e.ev(ev="action", node="settings/split/mods/tabs/pages/pages/radar.main", action="modpage.radar.main")
        out["mod-page"] = fixture(e, "mod-page", "A page contributed by a mod (origin mod:radar).")
        e.ev(ev="back")
        e.ev(ev="back")
        e.ev(ev="action", node="home", action="credits")
        out["credits"] = fixture(e, "credits", "Credits and licence.")
        e.ev(ev="back")
        report["snapshot_bytes_max"] = max(e.sizes["snapshot"])
        report["diff_bytes_median"] = sorted(e.sizes["diff"])[len(e.sizes["diff"]) // 2] if e.sizes["diff"] else 0
        report["diff_bytes_max"] = max(e.sizes["diff"]) if e.sizes["diff"] else 0
        report["diffs"] = len(e.sizes["diff"])
        # an image
        r = e.request({"t": "image", "ref": "pk3:maps/stormkeep", "w": 320, "h": 180, "as": "bytes"})
        report["image"] = {"t": r["t"], "w": r.get("w"), "h": r.get("h"), "bytes": len(r.get("data", "")) * 3 // 4}
    finally:
        e.close()
    return out, report


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--engine", default=os.environ.get("UIB_ENGINE", os.path.join(ROOT, "build", "bin", "xonotic")))
    ap.add_argument("--basedir", default=os.environ.get("UIB_BASEDIR"), required=not os.environ.get("UIB_BASEDIR"))
    ap.add_argument("--libdir", default=os.environ.get("UIB_LIBDIR", ""))
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--report")
    args = ap.parse_args()
    out, report = run(args)
    if args.report:
        with open(args.report, "w") as f:
            json.dump(report, f, indent=1)
    print(json.dumps(report, indent=1))
    bad = 0
    for name, fx in sorted(out.items()):
        path = os.path.join(FIX, name + ".json")
        if args.write:
            with open(path, "w", encoding="utf-8") as f:
                json.dump(fx, f, indent=1, ensure_ascii=False)
                f.write("\n")
        elif args.check:
            try:
                with open(path, encoding="utf-8") as f:
                    old = json.load(f)
            except FileNotFoundError:
                print("MISSING", name)
                bad += 1
                continue
            if scrub(old) != scrub(fx):
                print("DRIFT  ", name)
                bad += 1
            else:
                print("same   ", name)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
