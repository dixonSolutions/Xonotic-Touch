#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# UI bridge tests (docs/UI_BRIDGE.md).
#
#   scripts/test-uibridge.sh            schema, fixtures, store property test, fixture player
#   UIB_BASEDIR=<basedir with game data> [UIB_ENGINE=build/bin/xonotic] [UIB_LIBDIR=...] \
#   scripts/test-uibridge.sh --engine   also the engine: transport, round trips, images, hostile
#                                       mods, reconnect, zero cost, the QuakeC renderer under
#                                       scripted input, the first start, and fixture drift (--check)
#
# The first part needs only python3 and a C compiler and runs in CI (.github/workflows/tests.yml).
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/uibtest.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

echo "== schema is current"
python3 scripts/uib-schema.py check

echo "== fixtures validate"
python3 scripts/uib-schema.py validate tests/uibridge/fixtures/*.json

echo "== the validator refuses what it should"
python3 - <<'PY'
import json, importlib.util, sys
spec = importlib.util.spec_from_file_location("s", "scripts/uib-schema.py"); s = importlib.util.module_from_spec(spec); spec.loader.exec_module(s)
base = json.load(open("tests/uibridge/fixtures/home.json"))
bad = []
def mut(f):
    x = json.loads(json.dumps(base)); f(x); return x
bad.append(mut(lambda x: x["snapshot"]["nodes"][1]["props"].__setitem__("bogus", 1)))       # unknown prop
bad.append(mut(lambda x: x["snapshot"]["nodes"][1].__setitem__("type", "spaceship")))       # unknown type
bad.append(mut(lambda x: x["snapshot"]["nodes"][0]["children"].append("nowhere")))          # missing child
bad.append(mut(lambda x: x["snapshot"]["nodes"][2]["props"].__setitem__("aspect", 1.5)))    # wrong type
bad.append(mut(lambda x: x["snapshot"]["nodes"][1]["props"].__setitem__("actions", {"id": "q"})))  # object for array
n = 0
for b in bad:
    try:
        s.validate(b, s.SCHEMA, s.SCHEMA); s.check_tree(b["snapshot"]["nodes"], "x")
    except s.Invalid:
        n += 1
print(f"{n}/{len(bad)} broken fixtures refused")
sys.exit(0 if n == len(bad) else 1)
PY

echo "== store and JSON: unit and property tests"
cc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Iengine/darkplaces \
	tests/uibridge/store_test.c engine/darkplaces/touch_uib_store.c engine/darkplaces/touch_uib_json.c -lm -o "$tmp/store_test" 2>/dev/null \
	|| cc -O1 -g -Iengine/darkplaces tests/uibridge/store_test.c engine/darkplaces/touch_uib_store.c engine/darkplaces/touch_uib_json.c -lm -o "$tmp/store_test"
"$tmp/store_test"

echo "== fixture player over a socketpair"
python3 tests/uibridge/replay_test.py

if [ "${1:-}" = "--engine" ]; then
	: "${UIB_BASEDIR:?set UIB_BASEDIR to a basedir with game data}"
	echo "== engine: transport, round trips, images, mods, reconnect, zero cost"
	python3 tests/uibridge/engine_test.py
	echo "== engine: the QuakeC renderer under scripted input (default menu, taps, drags, keys, classic dialogs, a match)"
	python3 tests/uibridge/input_test.py 1280x720 mouse
	echo "== engine: first start on a touchscreen (welcome, screen keyboard, game data still downloading)"
	python3 tests/uibridge/firstrun_test.py
	echo "== engine: fixtures have not drifted"
	python3 scripts/uib-fixtures.py --check
fi
echo "test-uibridge: all passed"
