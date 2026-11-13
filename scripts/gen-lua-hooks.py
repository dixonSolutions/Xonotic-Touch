#!/usr/bin/env python3
"""Generate the Lua bridge to Xonotic's mutator hooks from their declarations.

Xonotic declares every hook a mutator can use once, as

    /// Called when ... Return true to ...
    #define params(i, o) \\
        i(entity, 0) /* player */ \\
        i(float,  1) /* damage */ \\
        o(float,  1) \\
        /**/
    MUTATOR_HOOKABLE(Name, params);

(i: the hook's caller passes it in, o: a mutator may change it). This script
reads those declarations and writes the three files that make the hooks
reachable from Lua server plugins, so the bridge follows upstream by itself
when a sync adds, removes or changes a hook:

  engine/darkplaces/touch_lua_hooks_gen.h       the hook table the engine uses
  qcsrc/server/touch_lua_hooks.qc               one small QC callback per hook
  docs/LUA_HOOKS.md                             the reference for mod authors

Nothing here is hand-edited. Run it after an upstream sync (the test suite runs
it with --check and fails when a generated file is stale):

  python3 scripts/gen-lua-hooks.py           # write the files
  python3 scripts/gen-lua-hooks.py --check   # exit 1 if any file would change

See docs/PLUGIN_STANDARD.md, "Hooks".
"""
import hashlib
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
QCSRC = os.path.join(ROOT, "engine/data/xonotic-data.pk3dir/qcsrc")
# Every file whose hooks exist in the server program (SVQC). Client-only hooks
# (client/mutators/events.qh) live in CSQC, which a remote server may replace,
# and are out of this bridge's reach.
SOURCES = [
    "common/mutators/events.qh",
    "server/mutators/events.qh",
    "common/mutators/mutator/nades/sv_nades.qh",
    "common/mutators/mutator/random_items/sv_random_items.qh",
]
OUT_H = os.path.join(ROOT, "engine/darkplaces/touch_lua_hooks_gen.h")
OUT_QC = os.path.join(QCSRC, "server/touch_lua_hooks.qc")
OUT_MD = os.path.join(ROOT, "docs/LUA_HOOKS.md")

TYPES = {"entity": "e", "float": "f", "int": "i", "bool": "b", "vector": "v", "string": "s"}
LUA_TYPE = {"e": "entity number", "f": "number", "i": "integer", "b": "boolean", "v": "{x, y, z}", "s": "string"}
KEYWORDS = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in",
            "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while", "name", "cancel"}
ARG_RE = re.compile(r"\b([io])\(\s*(\w+)\s*,\s*(\d+)\s*\)\s*(?:/\*+\s*(.*?)\s*\*/)?")
HOOK_RE = re.compile(r"MUTATOR_HOOKABLE\(\s*(\w+)\s*,\s*(\w+)\s*\)")
CANCEL_RE = re.compile(r"\breturn(s)?\s+(true|1)\b", re.I)
MAX_ARGS = 16


def clean_doc(lines):
    text = " ".join(l.strip() for l in lines if l.strip())
    return re.sub(r"\s+", " ", text).strip()


def parse(path):
    hooks = []
    doc, in_block = [], False
    params = None
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    i = 0
    while i < len(lines):
        raw = lines[i]
        line = raw.strip()
        i += 1
        if in_block:
            end = "*/" in line
            text = line.split("*/")[0].lstrip("*").strip()
            if text:
                doc.append(text)
            in_block = not end
            continue
        if line.startswith("/**") and not line.startswith("/**/"):
            text = line[3:]
            if "*/" in text:
                doc.append(text.split("*/")[0].strip())
            else:
                doc.append(text.strip())
                in_block = True
            continue
        if line.startswith("///"):
            doc.append(line[3:].strip())
            continue
        if line.startswith("#define params(i, o)") or line.startswith("#define params(i,o)"):
            body = []
            cur = raw
            while cur.rstrip().endswith("\\"):
                if i >= len(lines):
                    break
                cur = lines[i]
                i += 1
                body.append(cur)
            params = []
            for b in body:
                for m in ARG_RE.finditer(b):
                    io, typ, idx, comment = m.group(1), m.group(2), int(m.group(3)), (m.group(4) or "").strip()
                    if typ not in TYPES:
                        raise SystemExit(f"{path}: unknown hook argument type {typ}")
                    params.append((io, TYPES[typ], idx, comment))
            continue
        m = HOOK_RE.search(line)
        if m:
            name, kind = m.group(1), m.group(2)
            args = params if kind == "params" else []
            if kind not in ("params", "EV_NO_ARGS"):
                raise SystemExit(f"{path}: {name} uses {kind}, which this script does not know")
            hooks.append(make_hook(name, args, clean_doc(doc), os.path.relpath(path, QCSRC)))
            doc, params = [], None
            continue
        if line.startswith("#undef params"):
            continue
        if line and not line.startswith("//"):
            doc = []  # code ends a doc comment that belonged to nothing
    return hooks


def alias_for(comment):
    text = re.split(r"[,(;:]", comment.replace("*", " "))[0].lower()
    words = re.findall(r"[a-z0-9_]+", text)
    if not 1 <= len(words) <= 3:
        return None
    a = "_".join(words)
    if not re.match(r"^[a-z_][a-z0-9_]*$", a) or a in KEYWORDS or len(a) > 32:
        return None
    return a


def make_hook(name, params, doc, source):
    slots = []  # (type, idx, in, out, comment)
    for io, t, idx, comment in params:
        for s in slots:
            if s["t"] == t and s["idx"] == idx:
                s[io] = True
                if comment and not s["comment"]:
                    s["comment"] = comment
                break
        else:
            slots.append({"t": t, "idx": idx, "i": io == "i", "o": io == "o", "comment": comment})
    if len(slots) > MAX_ARGS:
        raise SystemExit(f"{name}: more than {MAX_ARGS} arguments")
    seen = {}
    for s in slots:
        s["key"] = f"{s['t']}{s['idx']}"
        a = alias_for(s["comment"]) if s["comment"] else None
        s["alias"] = a
        if a:
            seen[a] = seen.get(a, 0) + 1
    for s in slots:
        if s["alias"] and (seen[s["alias"]] > 1 or re.match(r"^[efibvs]\d+$", s["alias"])):
            s["alias"] = None
        # an entity is never written back (a wrong one breaks the game code)
        s["writable"] = s["o"] and s["t"] != "e"
    return {"name": name, "slots": slots, "doc": doc, "cancel": bool(CANCEL_RE.search(doc)), "source": source}


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def gen_h(hooks, sig):
    out = ["// Generated by scripts/gen-lua-hooks.py from the MUTATOR_HOOKABLE declarations",
           "// in " + ", ".join(SOURCES) + ". Do not edit.",
           "#ifndef TOUCH_LUA_HOOKS_GEN_H", "#define TOUCH_LUA_HOOKS_GEN_H", "",
           f"#define TLUA_HOOKS_GEN_COUNT {len(hooks)}",
           f"#define TLUA_HOOKS_GEN_SIGNATURE {sig} // the QC side passes the same number",
           f"#define TLUA_HOOKS_MAXARGS {MAX_ARGS}", "",
           "// t: e entity, f float, i int, b bool, v vector, s string; in/out as declared",
           "typedef struct tlua_hookarg_s { char t; unsigned char idx, in, out, writable; const char *alias; } tlua_hookarg_t;",
           "typedef struct tlua_hookdef_s { const char *name; unsigned char cancel, nargs; tlua_hookarg_t args[TLUA_HOOKS_MAXARGS]; } tlua_hookdef_t;",
           "", "static const tlua_hookdef_t tlua_hookdefs[TLUA_HOOKS_GEN_COUNT] = {"]
    for h in hooks:
        args = ", ".join("{'%s', %d, %d, %d, %d, %s}" % (s["t"], s["idx"], int(s["i"]), int(s["o"]), int(s["writable"]),
                                                       c_str(s["alias"]) if s["alias"] else "NULL") for s in h["slots"])
        out.append("\t{%s, %d, %d, {%s}}," % (c_str(h["name"]), int(h["cancel"]), len(h["slots"]), args))
    out += ["};", "", "#endif", ""]
    return "\n".join(out)


QC_PUSH = {"e": "touch_lua_hook_arge", "f": "touch_lua_hook_argf", "i": "touch_lua_hook_argf",
           "b": "touch_lua_hook_argf", "v": "touch_lua_hook_argv", "s": "touch_lua_hook_args"}
QC_OUT = {"f": "touch_lua_hook_outf", "i": "touch_lua_hook_outf", "b": "touch_lua_hook_outf",
          "v": "touch_lua_hook_outv", "s": "touch_lua_hook_outs"}
QC_TYPE = {"e": "entity", "f": "float", "i": "int", "b": "bool", "v": "vector", "s": "string"}


def gen_qc(hooks, sig):
    out = ["// Generated by scripts/gen-lua-hooks.py from the MUTATOR_HOOKABLE declarations. Do not edit.",
           "// The Lua bridge to the game's mutator hooks (docs/LUA_HOOKS.md): one callback per hook,",
           "// added to the hook's chain only while a server plugin subscribes to it (so a hook nobody",
           "// subscribes to costs nothing), handing the hook's arguments to the engine and taking back",
           "// the outputs a plugin with hooks_write changed. Included by server/touch_lua.qc.",
           "",
           f"const float TOUCH_LUA_HOOKS_SIGNATURE = {sig};",
           "// a hook's chain through the preprocessor, as MUTATOR_HOOKABLE names it (a hook name",
           "// may itself be a macro, e.g. PutClientInServer)",
           "#define TOUCH_LUA_HOOK(name) _TOUCH_LUA_HOOK(name)",
           "#define _TOUCH_LUA_HOOK(name) HOOK_##name",
           "float touch_lua_hooks_gen;",
           "bool touch_lua_hooks_made;",
           ""]
    for n, h in enumerate(hooks):
        out.append(f"Callback touch_lua_cb_{h['name']};")
        out.append(f"bool touch_lua_cb_{h['name']}_on;")
        out.append(f"bool touch_lua_hk_{h['name']}()")
        out.append("{")
        out.append(f"\tif (!touch_lua_hook_begin({n})) return false;")
        for s in h["slots"]:
            out.append(f"\t{QC_PUSH[s['t']]}(M_ARGV({s['idx']}, {QC_TYPE[s['t']]}));")
        out.append("\tfloat r = touch_lua_hook_call();")
        wr = [ (k, s) for k, s in enumerate(h["slots"]) if s["writable"] ]
        if wr:
            out.append("\tif (r & 2)")
            out.append("\t{")
            for k, s in wr:
                out.append(f"\t\tif (touch_lua_hook_changed({k})) M_ARGV({s['idx']}, {QC_TYPE[s['t']]}) = {QC_OUT[s['t']]}({k});")
            out.append("\t}")
        out.append("\treturn (r & 1) != 0;")
        out.append("}")
        out.append("")
    out.append("void touch_lua_hooks_make()")
    out.append("{")
    out.append("\ttouch_lua_hooks_made = true;")
    for h in hooks:
        out.append(f"\ttouch_lua_cb_{h['name']} = NEW(Callback, touch_lua_hk_{h['name']});")
    out.append("}")
    out.append("")
    out.append("// once per server frame: puts each hook's callback in or out of its chain")
    out.append("void touch_lua_hooks_sync()")
    out.append("{")
    out.append("\tfloat g = touch_lua_hook_gen(TOUCH_LUA_HOOKS_SIGNATURE);")
    out.append("\tif (g == touch_lua_hooks_gen || g < 0) return;")
    out.append("\ttouch_lua_hooks_gen = g;")
    out.append("\tif (!touch_lua_hooks_made) touch_lua_hooks_make();")
    out.append("\tbool w;")
    for n, h in enumerate(hooks):
        nm = h["name"]
        out.append(f"\tw = touch_lua_hook_wanted({n});")
        out.append(f"\tif (w != touch_lua_cb_{nm}_on) {{ touch_lua_cb_{nm}_on = w; if (w) CallbackChain_Add(TOUCH_LUA_HOOK({nm}), touch_lua_cb_{nm}, CBC_ORDER_ANY); else CallbackChain_Remove(TOUCH_LUA_HOOK({nm}), touch_lua_cb_{nm}); }}")
    out.append("}")
    out.append("")
    return "\n".join(out)


def gen_md(hooks, total_args):
    w = sum(1 for h in hooks if any(s["writable"] for s in h["slots"]))
    c = sum(1 for h in hooks if h["cancel"])
    out = ["# Lua hook reference", "",
           "<!-- Generated by scripts/gen-lua-hooks.py from the MUTATOR_HOOKABLE declarations. Do not edit. -->", "",
           f"Server mods (mods that run in games you host) can subscribe to **{len(hooks)} hooks** of the game code (Xonotic's mutator hooks): "
           f"{w} have outputs a server mod may change and {c} can be cancelled. How to use them, the permissions "
           "and the limits are in [PLUGIN_STANDARD.md](PLUGIN_STANDARD.md#hooks); this page lists every hook.", "",
           "```lua",
           "-- mod.txt: api: 2, permissions: hooks, hooks_write",
           "xt.hook(\"PlayerDies\", function(h)",
           "  print(\"player\", h.target, \"died, deathtype\", h.deathtype)",
           "end)",
           "xt.hook(\"Damage_Calculate\", function(h)",
           "  h.damage = h.damage * 0.5     -- an output: needs hooks_write",
           "end)",
           "```", "",
           "Each handler gets one table `h`. Every argument is in it under its position key, the "
           "type letter and the game's argument index (`e0`, `f4`, `s1`, `v2`, `i1`, `b5`), and, "
           "where the declaration names it plainly, under that name as well (`h.damage` is `h.f4` "
           "in `Damage_Calculate`). Types: `e` an entity number (a player's is their player id, "
           "1..maxplayers; 0 is none; `xt.entity(n)` describes one), `f` number, `i` integer, `b` "
           "boolean, `v` `{x, y, z}`, `s` string.", "",
           "**out** arguments are what the hook lets mutators change: set the field and, with "
           "`hooks_write` granted, the game uses the new value (entities are never written). "
           "**Cancel**: where the game's own description says to return true, a handler returning "
           "`true` (with `hooks_write`) does that. Without `hooks_write` changes and return values "
           "are ignored.", "",
           "| Hook | Arguments | Cancel | What the game says |", "|---|---|---|---|"]
    for h in hooks:
        parts = []
        for s in h["slots"]:
            name = f"`{s['key']}`" + (f" `{s['alias']}`" if s["alias"] else "")
            io = "in/out" if s["i"] and s["o"] else ("out" if s["o"] else "in")
            if s["o"] and s["t"] == "e":
                io = "out (read only: entity)"
            desc = s["comment"].replace("|", "\\|")
            parts.append(f"{name} {io}" + (f": {desc}" if desc else ""))
        doc = h["doc"].replace("|", "\\|") or "-"
        out.append(f"| `{h['name']}` | {'<br>'.join(parts) if parts else '-'} | {'yes' if h['cancel'] else ''} | {doc} |")
    out += ["", f"Sources: " + ", ".join(f"`qcsrc/{s}`" for s in SOURCES) + ".", ""]
    return "\n".join(out)


def main():
    check = "--check" in sys.argv[1:]
    hooks = []
    for src in SOURCES:
        hooks += parse(os.path.join(QCSRC, src))
    names = [h["name"] for h in hooks]
    dup = {n for n in names if names.count(n) > 1}
    if dup:
        raise SystemExit("duplicate hooks: " + ", ".join(sorted(dup)))
    canon = ";".join(h["name"] + ":" + ",".join(s["key"] + ("w" if s["writable"] else "") for s in h["slots"]) for h in hooks)
    sig = int(hashlib.sha1(canon.encode()).hexdigest()[:6], 16)  # fits a QC float exactly
    files = {OUT_H: gen_h(hooks, sig), OUT_QC: gen_qc(hooks, sig), OUT_MD: gen_md(hooks, 0)}
    stale = []
    for path, text in files.items():
        old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
        if old != text:
            stale.append(os.path.relpath(path, ROOT))
            if not check:
                with open(path, "w", encoding="utf-8") as f:
                    f.write(text)
    if check:
        if stale:
            print("stale (run scripts/gen-lua-hooks.py): " + ", ".join(stale))
            return 1
        print(f"{len(hooks)} hooks, generated files up to date")
        return 0
    print(f"{len(hooks)} hooks; wrote " + (", ".join(stale) if stale else "nothing (up to date)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
