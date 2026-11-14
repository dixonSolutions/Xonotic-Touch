#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks a client log of scripts/test-native-menu-live.sh (case joinname).

    check_joinname.py <client.log> <server hostname> <address joined from the list> <address typed>

Every join screen names the server "<name> (<address>)": the name with its colour
codes out, cut to 32 characters with an ellipsis (engine/darkplaces/touch_joinname.c).
"""
import json
import re
import sys

log, host, listed, typed = sys.argv[1:5]


def clean(n):
    # what the engine does: colour codes out, one line, 32 characters at most
    n = re.sub(r"\^(x[0-9a-fA-F]{3}|[0-9])", "", n).replace("^^", "^")
    n = " ".join(n.split())
    return n if len(n) <= 32 else n[:31].rstrip() + "…"


name = clean(host)
text = open(log, encoding="utf-8", errors="replace").read().splitlines()
phase = "start"
titles = {}    # phase -> loading card titles in order (repeats folded)
labels = {}    # phase -> the connect topic's labels in order (repeats folded)
cards = {}     # phase -> the pause page's card
for line in text:
    m = re.match(r"^MARK (\w+)", line)
    if m:
        phase = m.group(1)
        continue
    if line.startswith('{"uib":1,"t":"snapshot"'):
        try:
            nodes = {n["id"]: n for n in json.loads(line)["nodes"]}
        except ValueError:
            continue
        t = nodes.get("loading/status", {}).get("props", {}).get("title")
        if t and titles.setdefault(phase, [""])[-1] != t:
            titles[phase].append(t)
        if "pause/match" in nodes:
            cards[phase] = nodes["pause/match"].get("props", {})
    elif line.startswith("FE> connect "):
        try:
            c = json.loads(line[len("FE> connect "):])
        except ValueError:
            continue
        if c.get("address") and labels.setdefault(phase, [""])[-1] != c.get("label"):
            labels[phase].append(c.get("label"))
for d in (titles, labels):
    for k in d:
        d[k] = d[k][1:]

errors = []
def want(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        errors.append(what)

print(f"the server's name as shown: {name!r}")
want("^" not in name and len(name) == 32 and name.endswith("…"), "colour codes out, cut to 32 characters with an ellipsis")

full = f"Joining {name} ({listed})"
seen = titles.get("list", [])
want(bool(seen) and all(t == full for t in seen),
     f"joined from the server list: the loading card says {full!r} from the first frame ({seen})")
want(labels.get("list", [""])[0] == f"{name} ({listed})",
     f"... and the connect topic's label (what Android's card shows) names it at once ({labels.get('list')})")
card = cards.get("listpause", {})
want(card.get("title") == f"{name} ({listed})", f"... and so does the pause page's card ({card.get('title')!r} / {card.get('subtitle')!r})")

seen = titles.get("typed", [])
first, then = f"Joining {typed}", f"Joining {name} ({typed})"
want(len(seen) >= 2 and seen[0] == first and seen[-1] == then and set(seen) == {first, then},
     f"joined by a typed address: the card says {first!r}, then {then!r} once the server answers ({seen})")
lab = labels.get("typed", [])
want(len(lab) >= 2 and lab[0] == typed and lab[-1] == f"{name} ({typed})",
     f"... and the connect topic's label follows ({lab})")
card = cards.get("typedpause", {})
want(card.get("title") == f"{name} ({typed})", f"... and the pause page's card names it ({card.get('title')!r} / {card.get('subtitle')!r})")
want(not any("^" in t for v in titles.values() for t in v), "no colour code reaches a title")
sys.exit(1 if errors else 0)
