#!/usr/bin/env python3
"""Static checks of the Lua mod examples and docs (scripts/test-mods.sh, CI).

  check-mod-docs.py extract <docs dir>          every ```lua block of docs/*.md on stdout,
                                                each after a "--@@ <file>:<line>" line,
                                                for lua-syntax-check -
  check-mod-docs.py manifests <dir>...          every mod.txt below the dirs follows the
                                                standard (docs/PLUGIN_STANDARD.md)
  check-mod-docs.py links <cookbook> <examples> every recipe links an example folder that
                                                exists, and every example has a README.md

A ```lua block that is not meant to compile on its own (a fragment) is marked
```lua fragment and skipped.
"""
import os
import re
import sys

TIERS = {"standard": 1, "gamerules": 1, "internet": 3, "engine_full": 3}
ALIASES = {"events": 1, "commands": 1, "cvar_read": 1, "cvar_write": 1, "exec": 1, "storage": 1, "display": 1,
           "input": 1, "players": 1, "messages": 1, "state": 1, "hooks": 2, "hooks_write": 2, "game_rules": 3,
           "http": 3, "engine": 3}
SETTING_TYPES = {"bool", "number", "int", "float", "choice", "text", "color", "key"}


def extract(docs):
    for name in sorted(os.listdir(docs)):
        if not name.endswith(".md"):
            continue
        lines = open(os.path.join(docs, name), encoding="utf-8").read().split("\n")
        inblock, start = False, 0
        for i, line in enumerate(lines):
            s = line.strip()
            if not inblock and s.startswith("```lua"):
                if s == "```lua":
                    inblock, start = True, i + 1
                    print(f"--@@ docs/{name}:{i + 1}")
                continue
            if inblock and s.startswith("```"):
                inblock = False
                continue
            if inblock:
                print(line)
    return 0


def check_manifest(path, folder, kind):
    errs = []
    keys = {}
    settings = set()
    api = 0
    for n, raw in enumerate(open(path, encoding="utf-8").read().split("\n"), 1):
        line = raw.strip()
        if not line or line.startswith("#") or ":" not in line:
            continue
        k, v = [x.strip() for x in line.split(":", 1)]
        keys[k] = v
        if k == "api":
            if not v.isdigit() or not 1 <= int(v) <= 3:
                errs.append(f"line {n}: api {v}: 1 to 3")
            else:
                api = int(v)
        elif k == "permissions":
            for p in re.split(r"[,\s]+", v):
                if p and p not in TIERS and p not in ALIASES:
                    errs.append(f"line {n}: unknown permission {p}")
        elif k.startswith("reason."):
            p = k[7:]
            if p not in TIERS and p not in ALIASES:
                errs.append(f"line {n}: {k}: no such permission")
        elif k.startswith("setting."):
            rest = k[8:]
            if "." in rest:
                nm, field = rest.split(".", 1)
                if nm not in settings or field not in ("desc", "group"):
                    errs.append(f"line {n}: {k}: after setting.{nm}, desc or group")
                continue
            if not re.fullmatch(r"[a-z0-9]{1,32}", rest):
                errs.append(f"line {n}: setting name {rest}: a-z 0-9")
            spec = v.split("|")[0].split()
            if not spec or spec[0] not in SETTING_TYPES or len(spec) < 2:
                errs.append(f"line {n}: setting {rest}: <type> <default> ...")
            settings.add(rest)
        elif k == "http":
            if not re.fullmatch(r"[A-Za-z0-9.\-_:, ]+", v):
                errs.append(f"line {n}: http: host names")
        elif k == "realm":
            want = {"mods": "client", "plugins": "server"}.get(kind)
            if v not in ("client", "server", "both"):
                errs.append(f"line {n}: realm {v}")
            elif want and v not in (want, "both"):
                errs.append(f"line {n}: realm {v} in {kind}/")
    if keys.get("id") and keys["id"] != folder:
        errs.append(f"id {keys['id']} is not the folder name {folder}")
    if "permissions" in keys and not api:
        errs.append("permissions: needs an api: line")
    for p in re.split(r"[,\s]+", keys.get("permissions", "")):
        if p and api and max(TIERS.get(p, 0), ALIASES.get(p, 0)) > api:
            errs.append(f"permission {p} needs a higher api: than {api}")
    return errs


def manifests(dirs):
    bad = 0
    count = 0
    for top in dirs:
        for d, _, files in os.walk(top):
            if "mod.txt" not in files:
                continue
            count += 1
            folder = os.path.basename(d)
            kind = os.path.basename(os.path.dirname(d))
            errs = check_manifest(os.path.join(d, "mod.txt"), folder, kind)
            if not os.path.isfile(os.path.join(d, "init.lua")):
                errs.append("no init.lua")
            for e in errs:
                print(f"{d}/mod.txt: {e}")
                bad += 1
    print(f"{count} manifests, {bad} problem(s)")
    return 1 if bad else 0


def links(cookbook, examples):
    text = open(cookbook, encoding="utf-8").read()
    bad = 0
    found = re.findall(r"\]\((\.\./touch/lua/examples/[^)#]+)\)", text)
    if not found:
        print("the cookbook links no example")
        bad += 1
    for rel in found:
        path = os.path.normpath(os.path.join(os.path.dirname(cookbook), rel))
        if not os.path.exists(path):
            print(f"cookbook link to a missing example: {rel}")
            bad += 1
    for kind in ("mods", "plugins"):
        base = os.path.join(examples, kind)
        for name in sorted(os.listdir(base)):
            d = os.path.join(base, name)
            if os.path.isdir(d) and os.path.isfile(os.path.join(d, "mod.txt")) and "api: 3" in open(os.path.join(d, "mod.txt")).read():
                if not os.path.isfile(os.path.join(d, "README.md")):
                    print(f"{kind}/{name}: no README.md")
                    bad += 1
    print(f"{len(found)} cookbook links, {bad} problem(s)")
    return 1 if bad else 0


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    cmd = sys.argv[1]
    if cmd == "extract":
        return extract(sys.argv[2])
    if cmd == "manifests":
        return manifests(sys.argv[2:])
    if cmd == "links":
        return links(sys.argv[2], sys.argv[3])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
