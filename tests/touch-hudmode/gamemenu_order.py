#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The touch game menu (dialog_touch_gamemenu.qc) keeps what a player came for
on a phone's first screen: Resume on the first row, Join game and Spectate on
the second, and Leave and Quit on the last row, which the touch sheet pins to
the bottom edge (touch_menu_sheet.qc). At 2340x1080 the sheet shows about
three rows above its pinned footer; Join and Spectate used to be the fourth."""
import re
import sys

src = open(sys.argv[1], encoding="utf-8").read()
body = src[src.index("void GodnoticGameMenuDialog_fill"):]
rows, cur = [], []
for line in body.splitlines():
    if "me.TR(me)" in line:
        if cur:
            rows.append(cur)
        cur = []
    for label in re.findall(r'me\.TD\(.*?_\("([^"]+)"\)', line):
        cur.append(label)
if cur:
    rows.append(cur)

errors = []
if not rows or rows[0] != ["Resume"]:
    errors.append(f"first row is {rows[:1]}, want Resume alone")
if len(rows) < 2 or sorted(rows[1]) != ["Join game", "Spectate"]:
    errors.append(f"second row is {rows[1:2]}, want Join game and Spectate")
if not rows or sorted(rows[-1]) != ["Leave to main menu", "Quit"]:
    errors.append(f"last row is {rows[-1:]}, want Leave to main menu and Quit (the pinned footer)")
for e in errors:
    print("gamemenu:", e)
if errors:
    sys.exit(1)
print(f"gamemenu: {len(rows)} rows, Resume, Join game / Spectate first, Leave / Quit pinned last")
