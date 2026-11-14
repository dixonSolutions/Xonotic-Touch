#!/usr/bin/env python3
"""Reads a client log of scripts/test-touch-controls-live.sh and checks it.

  check_live.py input <log> [<log2>]   the scripted-finger tour; with two logs
                                       (two densities) the same swipe in
                                       centimetres must turn the same angle
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

def turn(log, a, b):
    fa, fb = at(log, a, "TOUCHFEEL"), at(log, b, "TOUCHFEEL")
    if not fa or not fb:
        return None
    d = float(fb["yaw"]) - float(fa["yaw"])
    return (d + 180) % 360 - 180

def input_case(path):
    log = samples(path)
    name = path.rsplit("/", 2)[-2]
    slow = turn(log, "base", "slow")
    check(slow is not None and abs(abs(slow) - 72.6) < 5, f"{name}: a slow 2 cm swipe turns {slow} degrees (72.6 expected: 126 dp x 0.16 x 3.6)")
    held, dragged, up = (at(log, m, "TOUCHFEEL") for m in ("fire_held", "fire_dragged", "fire_up"))
    check(held and held["attack"] == "1", f"{name}: a finger on FIRE fires")
    d = turn(log, "fire_held", "fire_dragged")
    check(dragged and dragged["attack"] == "1" and d is not None and abs(abs(d) - 72.6) < 6,
          f"{name}: dragging 2 cm off FIRE turns the view {d} degrees while it keeps firing")
    check(up and up["attack"] == "0", f"{name}: lifting the finger stops the fire")
    d = turn(log, "fire_up", "hop_dragged")
    hop = at(log, "hop_dragged", "TOUCHFEEL")
    check(hop and hop["jump"] == "1" and d is not None and abs(abs(d) - 72.6) < 6, f"{name}: dragging off HOP turns the view {d} degrees while jump stays held")
    # held HOP: the vertical speed turns upwards again and again
    vz, inside = [], False
    for m, k, f in log:
        if m == "hop_dragged":
            inside = True
        if m == "hop_up":
            inside = False
        if inside and k == "TOUCHFEEL":
            vz.append(float(f["vz"]))
    takeoffs = sum(1 for a, b in zip(vz, vz[1:]) if a < 0 and b > 100)
    check(takeoffs >= 2, f"{name}: HOP held for 3.3 s jumped again {takeoffs} times")
    check(at(log, "hop_up", "TOUCHFEEL") and at(log, "hop_up", "TOUCHFEEL")["jump"] == "0", f"{name}: lifting off HOP lets jump go")
    st, su = at(log, "stick_held", "TOUCHFEEL"), at(log, "stick_up", "TOUCHFEEL")
    check(st and float(st["fwd"]) > 100 and float(st["side"]) > 100, f"{name}: the stick pushed up and right moves forward and right ({st and st['fwd']}, {st and st['side']})")
    if st:
        want = math.degrees(math.atan2(0.08 * H_OVER_W[name_dev(name)], 0.05))
        got = math.degrees(math.atan2(float(st["fwd"]), float(st["side"])))
        check(abs(got - want) < 6, f"{name}: the move's angle is the thumb's ({got:.0f} degrees, pushed at {want:.0f}): analog, not eight-way")
    check(su and float(su["fwd"]) == 0 and float(su["side"]) == 0, f"{name}: lifting the thumb stops")
    flick = turn(log, "stick_up", "flick")
    check(flick is not None and slow is not None and abs(flick) > abs(slow) * 1.3, f"{name}: the same 2 cm as a flick turns further ({flick} against {slow})")
    return slow

H_OVER_W = {"tablet": 1200 / 1920, "phone": 1080 / 2340}
def name_dev(name):
    return "phone" if "phone" in name else "tablet"

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
if kind == "input":
    slows = [input_case(p) for p in sys.argv[2:]]
    if len(slows) == 2 and None not in slows:
        check(abs(abs(slows[0]) - abs(slows[1])) < 0.04 * abs(slows[0]),
              f"the same 2 cm swipe turns {slows[0]:.2f} at one density and {slows[1]:.2f} at the other (within 4%)")
elif kind == "join":
    join_case(sys.argv[2], sys.argv[3])
elif kind == "rects":
    rects_case(sys.argv[2])
sys.exit(1 if fails else 0)
