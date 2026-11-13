#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate docs/ui-bridge.schema.json (UI bridge schema 1.0) and validate JSON against it.

    scripts/uib-schema.py write            # regenerate docs/ui-bridge.schema.json
    scripts/uib-schema.py check            # fail if the committed schema differs
    scripts/uib-schema.py validate FILE..  # validate messages, fixtures or snapshots

The validator implements the subset of JSON Schema 2020-12 this schema uses, so the
tests need no third-party module. The schema is closed (additionalProperties false)
because it checks what the engine emits; renderers must still ignore unknown fields at
run time (docs/UI_BRIDGE.md 3.0).
"""
import json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCHEMA_PATH = os.path.join(ROOT, "docs", "ui-bridge.schema.json")

S = {"type": "string", "maxLength": 4096}
B = {"type": "boolean"}
I = {"type": "integer", "minimum": 0, "maximum": 9007199254740991}
N = {"type": "number"}
FRAC = {"type": "number", "minimum": 0, "maximum": 1}
ID = {"type": "string", "pattern": "^[A-Za-z0-9_.:/-]{1,128}$"}
ICON = {"type": "string", "pattern": "^[a-z0-9_]{1,40}$"}
IMG = {"type": "string", "pattern": "^(pk3:|asset:|mod:)[^\\s]{1,250}$"}


def enum(*v):
    return {"type": "string", "enum": list(v)}


def obj(props, required=(), closed=True):
    o = {"type": "object", "properties": props}
    if required:
        o["required"] = list(required)
    if closed:
        o["additionalProperties"] = False
    return o


def ref(name):
    return {"$ref": "#/$defs/" + name}


TONE = enum("neutral", "accent", "positive", "warning", "danger")
STYLE = enum("primary", "secondary", "tonal", "destructive", "link")
SCALAR = {"type": ["string", "number", "boolean"]}

DEFS = {}
DEFS["action"] = obj({
    "id": ID, "label": S, "icon": ICON, "style": STYLE, "enabled": B,
    "kind": enum("normal", "back", "link", "file", "share", "copy"),
    "confirm": obj({"title": S, "body": S, "ok": S, "cancel": S, "destructive": B}, ["title"]),
    "url": {"type": "string", "pattern": "^https?://[^\\s]{1,2000}$"},
    "accept": {"type": "array", "items": enum("pk3", "zip", "godnotic-mod", "cfg", "dem"), "maxItems": 8},
    "text": S,
    "a11y": S,
}, ["id"])
DEFS["badge"] = obj({"text": S, "icon": ICON, "tone": TONE})
DEFS["chip"] = obj({"text": S, "icon": ICON})
DEFS["column"] = obj({
    "key": ID, "label": S,
    "type": enum("text", "number", "ping", "players", "bool", "icon", "lock", "map", "mode", "size", "date"),
    "sortable": B, "width": enum("xs", "s", "m", "l", "fill"), "align": enum("start", "end"),
    "priority": {"type": "integer", "minimum": 1, "maximum": 3},
}, ["key", "type"])
DEFS["option"] = obj({"value": S, "label": S, "icon": ICON, "image": IMG, "help": S}, ["value", "label"])
DEFS["attr"] = obj({"label": S, "value": S, "icon": ICON}, ["label", "value"])
DEFS["mark"] = obj({"value": N, "label": S}, ["value"])
DEFS["pageState"] = obj({"title": S, "text": S, "icon": ICON, "action": ref("action")})
DEFS["busy"] = obj({"label": S, "detail": S, "progress": FRAC, "indeterminate": B, "cancel": ref("action")})

COMMON = {
    "title": S, "label": S, "subtitle": S, "icon": ICON, "hidden": B, "enabled": B,
    "help": S, "a11y": S, "actions": {"type": "array", "items": ref("action"), "maxItems": 16},
    # hints
    "density": enum("comfortable", "compact"), "emphasis": enum("low", "normal", "high"),
    "accent": enum("blue", "ember", "green", "red", "neutral"), "featured": B,
    "aspect": enum("16:9", "4:3", "1:1", "3:4", "21:9"),
    "importance": {"type": "integer", "minimum": 1, "maximum": 3}, "span": enum("full"),
    "tone": TONE, "live": B,
}
CONTROL = {"bind": {"type": "string", "pattern": "^(cvar|vm|mod):[^\\s]{1,120}$"}, "error": S,
           "apply": enum("immediate", "video", "sound", "reconnect", "restart")}
FORMAT = enum("int", "float1", "float2", "percent", "ms", "dp", "db", "x", "deg")

TYPES = {
    "app": {"stack": {"type": "array", "items": ID}, "modal": {"type": "array", "items": ID},
            "busy": ref("busy"), "canBack": B, "context": enum("menu", "game", "loading")},
    "screen": {"presentation": enum("page", "sheet", "dialog", "fullscreen"),
               "kind": enum("home", "page", "detail", "game", "loading", "status"),
               "primary": ref("action"), "background": IMG, "dismissable": B,
               "header": obj({"collapsible": B, "compact": B, "hidden": B})},
    "toolbar": {},
    "actionbar": {"align": enum("start", "center", "end", "spread")},
    "toast": {"text": S, "kind": enum("info", "success", "warning", "error"), "action": ref("action"),
              "timeout": I},
    "section": {"footer": S, "collapsible": B, "collapsed": B, "link": ref("action"), "sticky": B},
    "row": {"wrap": B, "align": enum("start", "center", "end", "spread")},
    "grid": {"minItem": I, "maxColumns": I, "scroll": enum("vertical", "horizontal")},
    "tabs": {"style": enum("tabs", "segmented"), "presentation": enum("horizontal"), "swipe": B, "value": S, **CONTROL},
    "pane": {"key": ID, "badge": S},
    "split": {"style": enum("list-detail", "vertical-tabs"), "value": S, "detailOpen": B, **CONTROL},
    "form": {"submit": ref("action"), "error": S},
    "card": {"image": IMG, "badges": {"type": "array", "items": ref("badge"), "maxItems": 8},
             "meta": {"type": "array", "items": ref("chip"), "maxItems": 8}, "progress": FRAC,
             "primary": ref("action"), "selected": B},
    "hero": {"image": IMG, "badges": {"type": "array", "items": ref("badge"), "maxItems": 8},
             "meta": {"type": "array", "items": ref("chip"), "maxItems": 8}, "primary": ref("action")},
    "list": {"source": {"type": "string", "pattern": "^[a-z][a-z0-9_.]*(:[^\\s]{1,120})?$"},
             "columns": {"type": "array", "items": ref("column"), "maxItems": 16},
             "sort": S, "desc": B, "filter": S, "searchable": B,
             "selection": enum("none", "single", "multi"),
             "selected": {"type": "array", "items": S, "maxItems": 256},
             "rowPrimary": ref("action"), "rowActions": {"type": "array", "items": ref("action"), "maxItems": 8},
             "state": enum("ready", "loading", "error"), "empty": ref("pageState"), "error": ref("pageState"),
             "count": I, "display": enum("table", "rows", "cards"),
             "rowHeight": enum("compact", "comfortable", "card", "estimated"), "estimate": I,
             "minItem": I, "aspectItem": enum("16:9", "4:3", "1:1", "3:4", "21:9")},
    "preview": {"image": IMG, "body": {"type": "string", "maxLength": 8192},
                "attrs": {"type": "array", "items": ref("attr"), "maxItems": 24},
                "badges": {"type": "array", "items": ref("badge"), "maxItems": 8}, "primary": ref("action")},
    "text": {"body": {"type": "string", "maxLength": 8192},
             "style": enum("body", "caption", "title", "mono", "note", "warning"), "selectable": B},
    "image": {"ref": IMG, "fit": enum("cover", "contain"), "alt": S},
    "divider": {},
    "status": {"body": S, "progress": FRAC, "indeterminate": B, "primary": ref("action")},
    "progress": {"value": FRAC, "indeterminate": B, "detail": S, "cancel": ref("action")},
    "switch": {**CONTROL, "value": B, "style": enum("switch", "checkbox")},
    "slider": {**CONTROL, "value": N, "min": N, "max": N, "step": N, "format": FORMAT, "unit": S,
               "marks": {"type": "array", "items": ref("mark"), "maxItems": 16}},
    "stepper": {**CONTROL, "value": N, "min": N, "max": N, "step": N, "format": FORMAT, "unit": S},
    "choice": {**CONTROL, "value": S, "options": {"type": "array", "items": ref("option"), "maxItems": 64},
               "style": enum("auto", "segmented", "dropdown", "radio", "cards")},
    "field": {**CONTROL, "value": S,
              "kind": enum("text", "name", "number", "password", "url", "search", "address"),
              "placeholder": S, "maxLength": I, "private": B, "hasValue": B, "submit": ref("action")},
    "button": {"action": ref("action"), "style": STYLE},
    "colour": {**CONTROL, "value": {"type": "string", "pattern": "^#[0-9a-f]{6}$"},
               "palette": {"type": "array", "items": {"type": "string", "pattern": "^#[0-9a-f]{6}$"},
                           "maxItems": 64}, "preview": IMG},
    "keybind": {**CONTROL, "value": {"type": "array", "items": {"type": "string", "maxLength": 32}, "maxItems": 4},
                "command": S, "max": I},
}

DEFS["node"] = {
    "type": "object",
    "required": ["id", "type", "props", "children"],
    "additionalProperties": False,
    "properties": {
        "id": ID,
        "type": enum(*TYPES.keys()),
        "props": {"type": "object"},
        "children": {"type": "array", "items": ID, "maxItems": 1024},
        "origin": {"type": "string", "pattern": "^(core|mod:[A-Za-z0-9_.-]{1,64})$"},
    },
    "allOf": [
        {"if": {"properties": {"type": {"const": t}}},
         "then": {"properties": {"props": obj({**COMMON, **p})}}}
        for t, p in TYPES.items()
    ],
}

ROW = obj({"key": {"type": "string", "maxLength": 256},
           "cells": {"type": "object", "additionalProperties": SCALAR},
           "image": IMG, "badges": {"type": "array", "items": ref("badge"), "maxItems": 8},
           "tone": TONE, "disabled": B}, ["key", "cells"])
DEFS["row"] = ROW

CAPS = obj({
    "nodes": {"type": "array", "items": S}, "hints": {"type": "array", "items": S},
    "width": I, "height": I, "dpi": I,
    "widthClass": enum("compact", "medium", "expanded"), "heightClass": enum("compact", "medium", "expanded"),
    "input": {"type": "array", "items": enum("touch", "mouse", "keyboard", "gamepad")},
    "locale": S, "reducedMotion": B, "powerSave": B,
    "thermal": enum("nominal", "fair", "serious", "critical"),
    "insets": obj({"top": I, "right": I, "bottom": I, "left": I}),
}, closed=False)
DEFS["caps"] = CAPS

ENV = {"uib": {"const": 1}, "t": S, "id": I, "re": I}


def msg(t, props, required=()):
    return {"if": {"properties": {"t": {"const": t}}, "required": ["t"]},
            "then": obj({**ENV, **props}, ["uib", "t"] + list(required))}


EV = enum("action", "value", "submit", "row", "shown", "hidden", "back", "dismiss", "visible")
MESSAGES = [
    msg("hello", {"schema": {"type": "string", "pattern": "^1\\.[0-9]+$"}, "engine": S, "session": S,
                  "limits": {"type": "object"}, "features": {"type": "array", "items": S},
                  "renderer": obj({"name": S, "version": S}, ["name"]), "caps": ref("caps")}, ["schema"]),
    msg("snapshot", {"rev": I, "root": {"const": "app"}, "nodes": {"type": "array", "items": ref("node")}},
        ["rev", "root", "nodes"]),
    msg("diff", {"base": I, "rev": I, "set": {"type": "array", "items": ref("node")},
                 "del": {"type": "array", "items": ID}}, ["base", "rev", "set", "del"]),
    msg("rows", {"source": S, "rev": I, "total": I, "offset": I, "sort": S, "desc": B, "filter": S,
                 "rows": {"type": "array", "items": ref("row"), "maxItems": 200}},
        ["source", "rev", "total", "offset", "rows"]),
    msg("source", {"source": S, "rev": I, "total": I}, ["source", "rev", "total"]),
    msg("image", {"ref": IMG, "w": I, "h": I, "mime": enum("image/png", "image/jpeg"),
                  "path": S, "data": {"type": "string"}, "as": enum("path", "bytes")}, ["ref"]),
    msg("ack", {}),
    msg("pong", {}),
    msg("ping", {}),
    msg("error", {"code": enum("version", "bad_request", "unknown", "not_found", "denied", "too_large",
                               "busy", "internal"), "msg": S}, ["code"]),
    msg("bye", {"reason": S}),
    msg("caps", {"caps": ref("caps")}, ["caps"]),
    msg("sync", {"have": I}),
    msg("window", {"source": S, "offset": I, "limit": {"type": "integer", "minimum": 1, "maximum": 200},
                   "sort": S, "desc": B, "filter": S}, ["source", "offset", "limit"]),
    msg("ev", {"ev": EV, "node": ID, "action": ID, "payload": {"type": "object", "additionalProperties": SCALAR},
               "value": {"type": ["string", "number", "boolean", "array"], "items": {"type": "string"}},
               "row": {"type": "string", "maxLength": 256}, "kind": enum("activate", "select", "action"),
               "offset": I, "limit": I}, ["ev"]),
]
DEFS["message"] = {
    "type": "object",
    "required": ["uib", "t"],
    "properties": {"t": enum("hello", "snapshot", "diff", "rows", "source", "image", "ack", "pong", "ping",
                              "error", "bye", "caps", "sync", "window", "ev")},
    "allOf": MESSAGES,
}

FIXTURE = obj({
    "name": S, "description": S, "generated": enum("seed", "engine"),
    "caps": ref("caps"),
    "snapshot": ref("message"),
    "steps": {"type": "array", "items": obj({"delay": N, "msg": ref("message"), "note": S}, ["msg"])},
    "rows": {"type": "object", "additionalProperties": {"type": "array", "items": ref("row")}},
    "images": {"type": "object", "additionalProperties": S},
}, ["name", "snapshot"])
DEFS["fixture"] = FIXTURE

SCHEMA = {
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://github.com/dixonSolutions/Godnotic/docs/ui-bridge.schema.json",
    "title": "Godnotic UI bridge, protocol 1, schema 1.0",
    "description": "Generated by scripts/uib-schema.py; see docs/UI_BRIDGE.md.",
    "oneOf": [ref("message"), ref("fixture")],
    "$defs": DEFS,
}

# ---------------------------------------------------------------- validator


class Invalid(Exception):
    pass


def _type_ok(v, t):
    if isinstance(t, list):
        return any(_type_ok(v, x) for x in t)
    if t == "object": return isinstance(v, dict)
    if t == "array": return isinstance(v, list)
    if t == "string": return isinstance(v, str)
    if t == "boolean": return isinstance(v, bool)
    if t == "integer": return isinstance(v, int) and not isinstance(v, bool)
    if t == "number": return isinstance(v, (int, float)) and not isinstance(v, bool)
    if t == "null": return v is None
    raise ValueError(t)


def validate(v, s, root, path="$"):
    if "$ref" in s:
        s = root["$defs"][s["$ref"].split("/")[-1]]
    if "type" in s and not _type_ok(v, s["type"]):
        raise Invalid(f"{path}: expected {s['type']}, got {type(v).__name__} {json.dumps(v)[:60]}")
    if "const" in s and v != s["const"]:
        raise Invalid(f"{path}: expected {s['const']!r}, got {v!r}")
    if "enum" in s and v not in s["enum"]:
        raise Invalid(f"{path}: {v!r} not in {s['enum']}")
    if isinstance(v, str):
        if "maxLength" in s and len(v.encode()) > s["maxLength"]:
            raise Invalid(f"{path}: longer than {s['maxLength']}")
        if "pattern" in s and not re.search(s["pattern"], v):
            raise Invalid(f"{path}: {v!r} does not match {s['pattern']}")
    if isinstance(v, (int, float)) and not isinstance(v, bool):
        if "minimum" in s and v < s["minimum"]:
            raise Invalid(f"{path}: {v} < {s['minimum']}")
        if "maximum" in s and v > s["maximum"]:
            raise Invalid(f"{path}: {v} > {s['maximum']}")
    if isinstance(v, list):
        if "maxItems" in s and len(v) > s["maxItems"]:
            raise Invalid(f"{path}: more than {s['maxItems']} items")
        if "items" in s:
            for i, x in enumerate(v):
                validate(x, s["items"], root, f"{path}[{i}]")
    if isinstance(v, dict):
        for k in s.get("required", []):
            if k not in v:
                raise Invalid(f"{path}: missing {k!r}")
        props = s.get("properties", {})
        for k, x in v.items():
            if k in props:
                validate(x, props[k], root, f"{path}.{k}")
            elif s.get("additionalProperties") is False:
                raise Invalid(f"{path}: unknown field {k!r}")
            elif isinstance(s.get("additionalProperties"), dict):
                validate(x, s["additionalProperties"], root, f"{path}.{k}")
    for sub in s.get("allOf", []):
        validate(v, sub, root, path)
    if "if" in s:
        try:
            validate(v, s["if"], root, path)
            ok = True
        except Invalid:
            ok = False
        if ok and "then" in s:
            validate(v, s["then"], root, path)
    if "oneOf" in s:
        errs = []
        for sub in s["oneOf"]:
            try:
                validate(v, sub, root, path)
                break
            except Invalid as e:
                errs.append(str(e))
        else:
            raise Invalid(" | ".join(errs))


def check_tree(nodes, path):
    """Structural rules the schema cannot express: unique ids, children exist, one root,
    parents before children, depth bound."""
    ids = {}
    for n in nodes:
        if n["id"] in ids:
            raise Invalid(f"{path}: duplicate id {n['id']}")
        ids[n["id"]] = n
    if "app" not in ids or ids["app"]["type"] != "app":
        raise Invalid(f"{path}: no app root")
    seen = set()
    for n in nodes:
        if n["id"] != "app" and n["id"] not in seen:
            raise Invalid(f"{path}: {n['id']} listed before its parent (or orphan)")
        for c in n["children"]:
            if c not in ids:
                raise Invalid(f"{path}: {n['id']} has missing child {c}")
            seen.add(c)
    app = ids["app"]["props"]
    for s in app.get("stack", []) + app.get("modal", []):
        if s not in ids or ids[s]["type"] != "screen":
            raise Invalid(f"{path}: app stack/modal names {s}, not a screen")


def apply_diff(nodes_by_id, diff):
    for d in diff["del"]:
        nodes_by_id.pop(d, None)
    for n in diff["set"]:
        nodes_by_id[n["id"]] = n


def validate_file(fn, schema):
    with open(fn, encoding="utf-8") as f:
        data = json.load(f)
    validate(data, schema, schema)
    if "snapshot" in data:  # fixture
        snap = data["snapshot"]
        check_tree(snap["nodes"], fn)
        cur = {n["id"]: n for n in snap["nodes"]}
        rev = snap["rev"]
        for i, st in enumerate(data.get("steps", [])):
            m = st["msg"]
            if m["t"] == "diff":
                if m["base"] != rev:
                    raise Invalid(f"{fn}: step {i} base {m['base']} != rev {rev}")
                apply_diff(cur, m)
                rev = m["rev"]
                order = topo(cur)
                check_tree(order, f"{fn} step {i}")
            elif m["t"] == "snapshot":
                cur = {n["id"]: n for n in m["nodes"]}
                rev = m["rev"]
    elif data.get("t") == "snapshot":
        check_tree(data["nodes"], fn)


def topo(cur):
    out, stack = [], ["app"]
    while stack:
        i = stack.pop()
        if i not in cur:
            raise Invalid(f"missing node {i} after diff")
        out.append(cur[i])
        stack.extend(reversed(cur[i]["children"]))
    if len(out) != len(cur):
        extra = set(cur) - {n["id"] for n in out}
        raise Invalid(f"unreachable nodes after diff: {sorted(extra)[:5]}")
    return out


def main(argv):
    text = json.dumps(SCHEMA, indent=1, sort_keys=False) + "\n"
    if not argv or argv[0] == "write":
        with open(SCHEMA_PATH, "w") as f:
            f.write(text)
        print("wrote", SCHEMA_PATH)
        return 0
    if argv[0] == "check":
        with open(SCHEMA_PATH) as f:
            if f.read() != text:
                print("docs/ui-bridge.schema.json is stale: run scripts/uib-schema.py write")
                return 1
        return 0
    if argv[0] == "validate":
        bad = 0
        for fn in argv[1:]:
            try:
                validate_file(fn, SCHEMA)
                print("ok  ", fn)
            except Invalid as e:
                bad += 1
                print("FAIL", fn, str(e)[:600])
        return 1 if bad else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
