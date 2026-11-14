#!/usr/bin/env python3
"""Reads a client log of scripts/test-touch-controls-live.sh and checks it.

  check_live.py join <log> <dm|ctf>    the join dialog on a stock server
  check_live.py rects <log>            no two controls intersect in any state
"""
import math
import re
import sys

def samples(path):
    """[(mark or None, kind, fields)] in order."""
    out, mark = [], None
    for line in open(path, errors="replace"):
        line = line.strip()
        m = re.match(r"MARK (\S+)", line)
        if m:
            mark = m.group(1)
            continue
        for kind in ("TOUCHFEEL", "TOUCHHUD", "TOUCHMENU", "TOUCHRECTS"):
            if line.startswith(kind + " "):
                fields = dict(kv.split("=", 1) for kv in line.split()[1:] if "=" in kv)
                out.append((mark, kind, fields))
        if line.startswith("TOUCHRECT "):
            out.append((mark, "TOUCHRECT", line.split()[1:]))
    return out

fails = []
def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        fails.append(what)

def at(log, mark, kind):
    for m, k, f in log:
        if m == mark and k == kind:
            return f
    return None

def join_case(path, gametype):
    log = samples(path)
    def mode(mark):
        f = at(log, mark, "TOUCHHUD")
        return f["mode"] if f else None
    def menu(mark):
        f = at(log, mark, "TOUCHMENU")
        return f["up"] if f else None
    check(mode("arrived") in ("spectating", "following") and menu("arrived") == "1", f"{gametype}: an observer at match start gets the join dialog (mode {mode('arrived')}, menu up {menu('arrived')})")
    check(mode("spectate") in ("spectating", "following") and menu("spectate") == "0", f"{gametype}: Spectate keeps observing and the dialog is gone (mode {mode('spectate')}, menu up {menu('spectate')})")
    check(menu("later") == "0", f"{gametype}: it does not come back by itself")
    check(menu("reopened") == "1", f"{gametype}: MENU > Join game opens it again")
    check(mode("joined") == "playing", f"{gametype}: choosing {'a team' if gametype == 'ctf' else 'Join'} gives mode playing (got {mode('joined')})")
    check(menu("joined") == "0", f"{gametype}: the dialog closed itself once in the game")
    text = open(path, errors="replace").read()
    if gametype == "ctf":
        check("is now playing on the" in text, "ctf: joined a team")
    check(" ours=1" not in "".join(l for l in text.splitlines() if l.startswith("TOUCHHUD")), f"{gametype}: the server's own game code ran (the public-server path)")

def hit(a, b):
    def box(r):
        if r[0] == "r":
            x, y, w, h = map(float, r[2:6]); return x, y, x + w, y + h
        cx, cy, rad = map(float, r[2:5]); return cx - rad, cy - rad, cx + rad, cy + rad
    if a[0] == "c" and b[0] == "c":
        return math.hypot(float(a[2]) - float(b[2]), float(a[3]) - float(b[3])) < float(a[4]) + float(b[4]) - 0.5
    if a[0] == "c" or b[0] == "c":
        c, r = (a, b) if a[0] == "c" else (b, a)
        cx, cy, rad = map(float, c[2:5]); x0, y0, x1, y1 = box(r)
        nx, ny = min(max(cx, x0), x1), min(max(cy, y0), y1)
        return math.hypot(cx - nx, cy - ny) < rad - 0.5
    ax0, ay0, ax1, ay1 = box(a); bx0, by0, bx1, by1 = box(b)
    return ax0 < bx1 - 0.5 and bx0 < ax1 - 0.5 and ay0 < by1 - 0.5 and by0 < ay1 - 0.5

def rects_case(path):
    log = samples(path)
    name = path.rsplit("/", 2)[-2]
    states, cur = {}, None
    for m, k, f in log:
        if k == "TOUCHRECTS":
            cur = f"{m} ({f.get('mode')}, {f.get('size')})"
            states[cur] = []
        elif k == "TOUCHRECT" and cur:
            states[cur].append(f)
    want = {"spectating", "playing", "scoreboard", "dead", "intermission", "mapvote"}
    seen = {s.split(" ")[0] for s in states}
    check(want <= seen, f"{name}: every state was sampled (missing: {sorted(want - seen)})")
    for state, rects in states.items():
        pairs = [(a[1], b[1]) for i, a in enumerate(rects) for b in rects[i + 1:] if hit(a, b)]
        check(len(rects) >= 2 and not pairs, f"{name} {state}: {len(rects)} rects, intersections: {pairs or 'none'}")

kind = sys.argv[1]
if kind == "join":
    join_case(sys.argv[2], sys.argv[3])
elif kind == "rects":
    rects_case(sys.argv[2])
sys.exit(1 if fails else 0)
