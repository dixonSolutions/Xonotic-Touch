#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""scripts/uib-replay.py over a socketpair: hello, snapshot, the diff sequence, windows,
images, a value event confirmed by a diff, bye. No engine needed."""
import json, os, socket, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
a, b = socket.socketpair()
p = subprocess.Popen([sys.executable, os.path.join(ROOT, "scripts", "uib-replay.py"), "--fd", str(b.fileno()), "--speed", "0.05",
                      "--quiet", os.path.join(ROOT, "tests", "uibridge", "fixtures", "seq-server-list.json")], pass_fds=[b.fileno()])
b.close()
a.settimeout(20)
f = a.makefile("rwb")
fails = 0


def send(m):
    f.write((json.dumps(m) + "\n").encode())
    f.flush()


def recv():
    return json.loads(f.readline())


def check(c, what):
    global fails
    print(("ok   " if c else "FAIL ") + what)
    fails += not c


send({"uib": 1, "t": "hello", "id": 1, "schema": "1.0", "renderer": {"name": "test"}, "caps": {}})
h, s = recv(), recv()
check(h["t"] == "hello" and s["t"] == "snapshot", "hello then snapshot")
rev = s["rev"]
kinds = []
for _ in range(6):
    m = recv()
    kinds.append(m["t"])
    if m["t"] == "diff":
        check(m["base"] == rev, f"diff base {m['base']} follows rev {rev}")
        rev = m["rev"]
check(kinds.count("source") == 3 and kinds.count("diff") == 3, f"sequence: {kinds}")
send({"uib": 1, "t": "window", "id": 2, "source": "servers", "offset": 0, "limit": 5, "sort": "ping"})
m = recv()
pings = [r["cells"]["ping"] for r in m["rows"]]
check(m["t"] == "rows" and m["total"] == 40 and pings == sorted(pings), f"window: {m['total']} rows, sorted pings {pings}")
send({"uib": 1, "t": "image", "id": 3, "ref": "pk3:maps/stormkeep", "w": 64, "h": 36})
m = recv()
check(m["t"] == "image" and os.path.getsize(m["path"]) > 50, "image path to a PNG")
send({"uib": 1, "t": "ev", "id": 4, "ev": "value", "node": "mp/bar/filters/lan", "value": True})
m = recv()
check(m["t"] == "diff" and m["set"][0]["props"]["value"] is True, "value event confirmed by a diff")
send({"uib": 1, "t": "bye"})
p.wait(10)
check(p.returncode == 0, "player exits on bye")
sys.exit(1 if fails else 0)
