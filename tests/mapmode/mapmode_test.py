#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Create > mode > map > Play starts that map in that mode (scripts/test-mapmode.sh).

A real engine per case, driven over the UI bridge stream transport as a renderer:
open Singleplayer, tap the mode card, read the map picker's rows, choose a map, press
Play, and read the running server's map and gametype back (prvm_globalget).

Paths:
  native          a native front end owns the menu from the start (touch_native_frontend 1,
                  `fe attach` before the first frame, heartbeats): the menu VM's m_draw never
                  runs, as on Android and in the GTK launcher
  native-restart  the same, and the menu VM restarts while the native UI owns the menu
  qcmenu          the QuakeC-drawn menu (menu_touch_uib 1, no native front end)

Checks per mode:
  - the picker's rows are exactly the maps MapInfo lets a match start on, computed here
    independently from the basedir's packs: the mode's `gametype` lines, Duel on every DM
    map (g_duel_not_dm_maps 0), only maps with a .bsp, not hidden/forbidden/frustrating
  - the mode card is offered
  - Play on a map of the picker starts that map in that mode (Duel: a map that has Duel
    only by implication, when there is one)
  - Play on a map the mode cannot start says so and starts nothing (no random map)

  BASEDIR=<basedir> [ENGINE=build/bin/xonotic] [LIBDIR=...] [MODES="dm ctf"] [PATHS="native qcmenu"]
  [JOBS=4] tests/mapmode/mapmode_test.py
"""
import concurrent.futures, json, os, re, socket, subprocess, sys, tempfile, time, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ENGINE = os.environ.get("ENGINE") or os.path.join(ROOT, "build", "bin", "xonotic")
BASEDIR = os.environ["BASEDIR"]
LIBDIR = os.environ.get("LIBDIR", os.environ.get("LD_LIBRARY_PATH", ""))
MODES = os.environ.get("MODES", "dm tdm ctf ca duel ft dom ka kh lms").split()
PATHS = os.environ.get("PATHS", "native native-restart qcmenu").split()
JOBS = int(os.environ.get("JOBS", "4"))
KEEP = os.environ.get("KEEP", "")

# ---------------------------------------------------------------- the oracle: MapInfo by hand

RENAMED = {"nexball": "nb", "freezetag": "ft", "keepaway": "ka", "invasion": "inv", "assault": "as",
           "race": "rc", "ffa": "dm", "cctf": "ctf", "oneflag": "ctf", "tourney": "duel"}


def pack_files(basedir):
    """{path: (reader, name)} as the engine's filesystem resolves it: later packs win."""
    files = {}
    data = os.path.join(basedir, "data")
    for entry in sorted(os.listdir(data)):
        p = os.path.realpath(os.path.join(data, entry))
        if entry.endswith((".pk3", ".pk3dir")) is False:
            continue
        if os.path.isdir(p):
            for d in ("maps",):
                md = os.path.join(p, d)
                if os.path.isdir(md):
                    for f in os.listdir(md):
                        files[f"{d}/{f}".lower()] = ("dir", os.path.join(md, f))
        elif zipfile.is_zipfile(p):
            with zipfile.ZipFile(p) as z:
                for n in z.namelist():
                    if n.lower().startswith("maps/") and n.count("/") == 1:
                        files[n.lower()] = ("zip", p, n)
    return files


def read(ref):
    if ref[0] == "dir":
        return open(ref[1], "rb").read().decode("utf-8", "replace")
    with zipfile.ZipFile(ref[1]) as z:
        return z.read(ref[2]).decode("utf-8", "replace")


def oracle(basedir):
    """{mode: sorted map names} a match can start on, with the engine defaults."""
    files = pack_files(basedir)
    out, noinfo = {}, []
    for path in files:
        if not path.endswith(".bsp"):
            continue
        name = path[5:-4]
        info = files.get(f"maps/{name}.mapinfo")
        if info is None:
            noinfo.append(name)
            continue
        types, flags = set(), set()
        for line in read(info).splitlines():
            w = line.split()
            if not w:
                continue
            if w[0] in ("gametype", "type") and len(w) > 1:
                types.add(RENAMED.get(w[1], w[1]))
            elif w[0] in ("hidden", "forbidden", "frustrating"):
                flags.add(w[0])
        if flags:
            continue
        if "dm" in types:
            types.add("duel")      # g_duel_not_dm_maps 0 (Duel.m_isForcedSupported)
        for t in types:
            out.setdefault(t, []).append(name)
    return {k: sorted(v) for k, v in out.items()}, noinfo


# ---------------------------------------------------------------- the engine as UI back end

class Conn:
    def __init__(self, sock):
        self.s, self.buf, self.mid, self.rev, self.nodes = sock, b"", 0, -1, {}
        self.seen = []   # every toast text, in order (toasts time out and leave the tree)

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
        if m.get("t") == "snapshot":
            self.nodes = {n["id"]: n for n in m["nodes"]}
        elif m.get("t") == "diff":
            for d in m["del"]:
                self.nodes.pop(d, None)
            for n in m["set"]:
                self.nodes[n["id"]] = n
        for n in (m.get("nodes") or m.get("set") or []):
            if n.get("type") == "toast" and n["props"].get("text") not in self.seen[-1:]:
                self.seen.append(n["props"].get("text", ""))
        return m

    def settle(self, quiet=0.6, limit=8):
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

    def ev(self, **m):
        self.send(dict({"t": "ev"}, **m))
        self.settle()

    def toasts(self):
        return list(self.seen)


PRE_NATIVE = """set touch_native_frontend 1
fe attach 1
alias mm_beat "fe beat; defer 1 mm_beat"
mm_beat
"""
# the running server's map and gametype, once a second (silent while no server runs)
PRE_CHECK = """set _mm_map ""
set _mm_gte 0
set _mm_gt ""
alias mm_chk "prvm_globalget server mapname _mm_map; prvm_globalget server MapInfo_LoadedGametype _mm_gte; mm_chk2"
alias mm_chk2 "prvm_edictget server $_mm_gte mdl _mm_gt; mm_chk3"
alias mm_chk3 "echo MMCHECK map=$_mm_map gt=$_mm_gt fe=$_fe_state; defer 1 mm_chk"
defer 1 mm_chk
"""


def run_case(path, mode, plays, oracle_rows):
    """One engine: check the picker for `mode`, then each (map, expect_start) in plays."""
    res = []

    def check(cond, what):
        res.append(("ok   " if cond else "FAIL ") + f"[{path} {mode}] {what}")
        return cond

    ud = tempfile.mkdtemp(prefix=f"mm-{path}-{mode}-", dir=os.environ.get("TMPDIR"))
    os.makedirs(os.path.join(ud, "data"))
    sock = os.path.join(ud, "run", "uib.sock")
    pre = "set touch_profile_done 1\nset menu_touch_uib 1\nset touch_lan_bots 0\nset g_warmup 0\n" + PRE_CHECK
    if path.startswith("native"):
        pre += PRE_NATIVE
    if path == "native-restart":
        pre += 'defer 3 "menu_restart"\n'
    with open(os.path.join(ud, "data", "mm.cfg"), "w") as f:
        f.write(pre)
    env = {k: v for k, v in os.environ.items() if k not in ("WAYLAND_DISPLAY", "DISPLAY", "XAUTHORITY")}
    env.update(SDL_VIDEODRIVER="offscreen", SDL_AUDIODRIVER="dummy")
    if LIBDIR:
        env["LD_LIBRARY_PATH"] = LIBDIR
    log = os.path.join(ud, "engine.log")
    cmd = ["timeout", "-k", "5", "180", ENGINE, "-xonotic", "-basedir", BASEDIR, "-userdir", ud, "-nosound",
           "+vid_fullscreen", "0", "+vid_width", "640", "+vid_height", "400", "+exec", "xonotic.cfg",
           "+set", "_touch_assets_ready", "1", "+exec", "mm.cfg", "-uibridge", "listen:" + sock]
    p = subprocess.Popen(cmd, env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT, cwd=BASEDIR)

    def logtext():
        return open(log, errors="replace").read()

    def wait_log(pattern, since, limit):
        end = time.monotonic() + limit
        while time.monotonic() < end:
            t = logtext()[since:]
            m = re.findall(pattern, t)
            if m:
                return m[-1]
            if p.poll() is not None:
                return None
            time.sleep(0.5)
        return None

    try:
        end = time.monotonic() + 60
        s = None
        while time.monotonic() < end:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(sock)
                break
            except OSError:
                s = None
                time.sleep(0.2)
        if not check(s is not None, "the engine listens for a renderer"):
            return res
        c = Conn(s)
        c.send({"t": "hello", "schema": "1.0", "renderer": {"name": "mapmode_test"}, "caps": {"widthClass": "expanded"}})
        end = time.monotonic() + 60   # a big data set takes a while to the first description
        while time.monotonic() < end and "home/tiles/sp" not in c.nodes:
            c.recv(1.0)
        check("home/tiles/sp" in c.nodes, "Home is described")
        c.settle()
        time.sleep(4 if path == "native-restart" else 1)   # past the menu VM restart
        c.settle()
        if path.startswith("native"):
            fe = wait_log(r"MMCHECK map=\S* gt=\S* fe=(\d)", 0, 10)
            check(fe == "1", f"the native front end owns the menu (_fe_state {fe})")
        c.ev(ev="action", node="home/tiles/sp", action="sp")
        card = f"sp/modes/grid/{mode}"
        if not check(card in c.nodes, f"the {mode} card is offered ({len(oracle_rows)} maps can start it)"):
            return res
        c.ev(ev="action", node=card, action=f"mode.{mode}")
        lst = c.nodes.get("sp/maps/list", {}).get("props", {})
        src = lst.get("source", "")
        r = c.request({"t": "window", "source": src, "offset": 0, "limit": 500})
        rows = sorted(x["key"] for x in (r or {}).get("rows", []))
        missing = sorted(set(oracle_rows) - set(rows))
        extra = sorted(set(rows) - set(oracle_rows))
        check(rows == oracle_rows, f"the picker ({src}) lists exactly MapInfo's {len(oracle_rows)} maps"
              + (f"; missing {missing}" if missing else "") + (f"; not startable {extra}" if extra else ""))
        for mapname, expect in plays:
            before = len(logtext())
            ntoast = len(c.toasts())
            c.ev(ev="row", node="sp/maps/list", row=mapname, kind="select")
            c.ev(ev="action", node="sp", action="play")
            if expect:
                got = wait_log(r"MMCHECK map=(\S+) gt=(\S+)", before, 60)
                check(got == (mapname, mode), f"Play on {mapname}: the match runs {mapname} in {mode} (server: {got})")
                news = c.toasts()[ntoast:]
                check(not news, f"Play on {mapname}: no error toast ({news})")
            else:
                time.sleep(6)
                c.settle()
                got = re.findall(r"MMCHECK map=(\S+) gt=(\S+)", logtext()[before:])
                started = [g for g in got if g[0]]
                check(not started, f"Play on {mapname} (not a {mode} map): no match starts ({started[-1] if started else 'none'})")
                news = [t for t in c.toasts() if mapname.lower() in t.lower() or "cannot" in t]
                check(bool(news), f"Play on {mapname}: a toast says why ({news})")
        s.close()
    except Exception as e:  # a broken engine or socket is a failed case, not a crashed suite
        check(False, f"exception {type(e).__name__}: {e}")
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()
        if not any(x.startswith("FAIL") for x in res) and not KEEP:
            subprocess.run(["rm", "-rf", ud])
        else:
            res.append(f"     log: {log}")
    return res


def main():
    want, noinfo = oracle(BASEDIR)
    print(f"oracle: {sum(1 for _ in want.get('dm', []))} DM maps; per mode: "
          + " ".join(f"{m}={len(want.get(m, []))}" for m in MODES) + (f"; no mapinfo: {noinfo}" if noinfo else ""))
    allmaps = sorted({x for v in want.values() for x in v})
    cases = []
    for mode in MODES:
        rows = want.get(mode, [])
        if not rows:
            print(f"skip {mode}: no map in this data can start it")
            continue
        # Duel: a DM map without its own duel line, so the implied type is exercised
        pick = rows[len(rows) // 2]
        if mode == "duel":
            explicit = set()
            files = pack_files(BASEDIR)
            for m in rows:
                if re.search(r"^gametype duel\b", read(files[f"maps/{m}.mapinfo"]), re.M):
                    explicit.add(m)
            implied = [m for m in rows if m not in explicit]
            if implied:
                pick = implied[0]
        bad = next((m for m in allmaps if m not in rows), None)
        # the refusal first: after a match starts the Create screen is gone
        plays = ([(bad, False)] if bad else []) + [(pick, True)]
        for path in PATHS:
            cases.append((path, mode, plays, rows))
    fails = 0
    with concurrent.futures.ThreadPoolExecutor(JOBS) as ex:
        for res in ex.map(lambda a: run_case(*a), cases):
            for line in res:
                print(line, flush=True)
                fails += line.startswith("FAIL")
    print(f"mapmode_test: FAILED {fails}" if fails else "mapmode_test: all passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
