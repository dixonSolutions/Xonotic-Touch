#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks a client log of scripts/test-native-menu-live.sh (case joinmid)."""
import json
import re
import sys

log, address = sys.argv[1], sys.argv[2]
text = open(log, encoding="utf-8", errors="replace").read().splitlines()
phase, modal, hud, nodes = "start", {}, {}, {}
for line in text:
    m = re.match(r"^MARK (\w+)", line)
    if m:
        phase = m.group(1)
        continue
    m = re.match(r"^TOUCHHUD mode=(\w+)", line)
    if m:
        hud.setdefault(phase, m.group(1))
    if line.startswith('{"uib":1,"t":"snapshot"') and phase not in nodes:
        try:
            nodes[phase] = {n["id"]: n for n in json.loads(line)["nodes"]}
        except ValueError:
            continue
        modal[phase] = nodes[phase].get("app", {}).get("props", {}).get("modal", [])

errors = []
def want(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        errors.append(what)

want(hud.get("asked") == "spectating", f"the player joined mid-match as an observer (mode {hud.get('asked')})")
want("join" in modal.get("asked", []), f"the join popup opened by itself, modal {modal.get('asked')}")
want(hud.get("after") == "spectating" and "join" not in modal.get("after", ["join"]),
     f"Spectate closes it and is respected (mode {hud.get('after')}, modal {modal.get('after')})")
card = nodes.get("paused", {}).get("pause/match", {}).get("props", {})
want(card.get("title") == address, f"the pause page's card names the server joined ({card.get('title')!r})")
want(card.get("subtitle", "").startswith("LAN game"), f"... as a LAN game, not \"Online game\" ({card.get('subtitle')!r})")
want("primary" not in card, "... and has no action, so no renderer shows a chevron that does nothing")
sys.exit(1 if errors else 0)
