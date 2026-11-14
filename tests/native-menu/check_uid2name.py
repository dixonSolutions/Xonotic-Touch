#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks a client log of scripts/test-native-menu-live.sh (case uid2name)."""
import re
import sys

log, answer = sys.argv[1], sys.argv[2]
text = open(log, encoding="utf-8", errors="replace").read().splitlines()
phase, events, dumps, uid = "start", [], {}, {}
for line in text:
    m = re.match(r"^MARK (\w+)", line)
    if m:
        phase = m.group(1)
        continue
    m = re.match(r'^FE> app .*"state":"(\w+)"', line)
    if m:
        events.append((phase, m.group(1)))
    m = re.search(r'"id":"app","type":"app","props":\{[^}]*"modal":\[([^\]]*)\]', line)
    if m and line.startswith("{"):
        dumps.setdefault(phase, m.group(1))
    m = re.match(r'^(?:\x1b\[[0-9;]*m)?"?cl_allow_uid2name(?:\x1b\[[0-9;]*m)?"? is "([^"]*)"', line)
    if m:
        uid[phase] = m.group(1)

errors = []
def want(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        errors.append(what)

want(uid.get("asked") == "-1", f"the question is unanswered at first (cl_allow_uid2name {uid.get('asked')})")
want('"uid2name"' in dumps.get("asked", ""), f"it is the bridge's own dialog, modal [{dumps.get('asked')}]")
want(uid.get("answered") == "0", f"{answer} answers No (cl_allow_uid2name {uid.get('answered')})")
want('"uid2name"' not in dumps.get("answered", "x"), f"the dialog is gone, modal [{dumps.get('answered')}]")
after_answer = [s for p, s in events if p == "answer"]
want(after_answer[-1:] == ["ingame"], f"the match is back after the answer (states {after_answer})")
paused = [s for p, s in events if p == "pause"]
want("paused" in paused, f"MENU opens the pause menu (states {paused})")
after_resume = [s for p, s in events if p == "resume"]
want(after_resume == ["ingame"], f"Resume closes it and it stays closed (states {after_resume})")
sys.exit(1 if errors else 0)
