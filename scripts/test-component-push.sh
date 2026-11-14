#!/bin/bash
# scripts/component-push.sh against local stand-ins for the component
# repositories: it pushes when the component's main is the pinned commit,
# refuses when someone else's work landed there since (and leaves it alone),
# lets through a move that only touches the repository's own .github/, pushes
# over a head only when told that head, and with "all" pushes nothing when
# either component has moved.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
fails=0
pass() { echo "ok - $1"; }
fail() { echo "FAIL - $1" >&2; fails=$((fails + 1)); }
g() { git -c user.name=t -c user.email=t@example.invalid -c init.defaultBranch=main -c advice.detachedHead=false "$@"; }

# A Godnotic checkout with both components, and the two component repositories.
GOD="$T/god"
mkdir -p "$GOD/scripts" "$GOD/engine/darkplaces" "$GOD/engine/data/xonotic-data.pk3dir"
cp "$ROOT/scripts/component-push.sh" "$ROOT/scripts/export-components.py" "$GOD/scripts/"
echo 'int a;' > "$GOD/engine/darkplaces/a.c"
echo 'int b;' > "$GOD/engine/darkplaces/b.c"
echo 'void q() {}' > "$GOD/engine/data/xonotic-data.pk3dir/q.qc"
for c in darkplaces:engine/darkplaces data:engine/data/xonotic-data.pk3dir; do
    n="${c%%:*}"; p="${c#*:}"
    bare="$T/remote/dixonSolutions/Godnotic-$n.git"
    g init -q --bare "$bare"
    seed="$T/seed-$n"; g init -q "$seed"
    cp -r "$GOD/$p/." "$seed/"
    g -C "$seed" add -A; g -C "$seed" commit -qm seed; g -C "$seed" push -q "$bare" HEAD:main
done
pin_of() { g -C "$T/seed-$1" rev-parse HEAD; }
cat > "$GOD/components.json" <<EOF
{
  "format": 1,
  "components": {
    "darkplaces": {"repository": "dixonSolutions/Godnotic-darkplaces", "path": "engine/darkplaces", "revision": "$(pin_of darkplaces)"},
    "data": {"repository": "dixonSolutions/Godnotic-data", "path": "engine/data/xonotic-data.pk3dir", "revision": "$(pin_of data)"}
  }
}
EOF
g -C "$GOD" init -q; g -C "$GOD" add -A; g -C "$GOD" commit -qm base
export GODNOTIC_COMPONENT_REMOTE_BASE="file://$T/remote" GODNOTIC_COMPONENT_CACHE="$T/cache"
push() { (cd "$GOD" && g -c user.name=t -c user.email=t@example.invalid -C "$GOD" config user.name t && bash scripts/component-push.sh "$@") >"$T/out" 2>&1; }
remote_head() { g --git-dir="$T/remote/dixonSolutions/Godnotic-$1.git" rev-parse main; }
pinned() { python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["components"][sys.argv[2]]["revision"])' "$GOD/components.json" "$1"; }
# another agent's push straight to the component repository
other_push() {
    local n="$1" file="$2" text="$3" o="$T/other-$RANDOM"
    g clone -q "$T/remote/dixonSolutions/Godnotic-$n.git" "$o"
    mkdir -p "$(dirname "$o/$file")"; echo "$text" > "$o/$file"
    g -C "$o" add -A; g -C "$o" commit -qm "other: $file"; g -C "$o" push -q origin HEAD:main
}

# 1. main is the pin: the change goes out and is pinned
echo 'int b2;' > "$GOD/engine/darkplaces/b.c"
if push darkplaces "b2" && [ "$(remote_head darkplaces)" = "$(pinned darkplaces)" ] \
    && g --git-dir="$T/remote/dixonSolutions/Godnotic-darkplaces.git" show main:b.c | grep -q b2; then
    pass "pushes and pins when main is the pinned commit"
else fail "push at the pin: $(cat "$T/out")"; fi

# 2. someone else's work landed: refused, and their commit is still there
other_push darkplaces a.c 'int theirs;'
before="$(remote_head darkplaces)"; pin_before="$(pinned darkplaces)"
echo 'int b3;' > "$GOD/engine/darkplaces/b.c"
if ! push darkplaces "b3"; then
    if [ "$(remote_head darkplaces)" = "$before" ] && [ "$(pinned darkplaces)" = "$pin_before" ] \
        && grep -q 'refusing to push' "$T/out" && grep -q '    a.c' "$T/out" && grep -q 'export-components.py --import darkplaces' "$T/out"; then
        pass "refuses over someone else's work, names the files and the merge"
    else fail "refusal without the details: $(cat "$T/out")"; fi
else fail "pushed over someone else's work"; fi

# 3. after merging their work in (their a.c, pinned at their head), it goes out
cp "$T/cache/darkplaces/a.c" "$GOD/engine/darkplaces/a.c" 2>/dev/null || g --git-dir="$T/remote/dixonSolutions/Godnotic-darkplaces.git" show main:a.c > "$GOD/engine/darkplaces/a.c"
(cd "$GOD" && python3 scripts/export-components.py --pin darkplaces "$before" >/dev/null)
if push darkplaces "b3 merged" && g --git-dir="$T/remote/dixonSolutions/Godnotic-darkplaces.git" show main:a.c | grep -q theirs \
    && g --git-dir="$T/remote/dixonSolutions/Godnotic-darkplaces.git" show main:b.c | grep -q b3; then
    pass "pushes once their work is merged, keeping both"
else fail "push after the merge: $(cat "$T/out")"; fi

# 4. a move that only touches the repository's own .github/ is not a conflict
other_push darkplaces .github/README.md '# engine'
echo 'int b4;' > "$GOD/engine/darkplaces/b.c"
if push darkplaces "b4" && g --git-dir="$T/remote/dixonSolutions/Godnotic-darkplaces.git" show main:.github/README.md | grep -q engine; then
    pass "a .github-only move is not refused, and .github is kept"
else fail ".github-only move: $(cat "$T/out")"; fi

# 5. COMPONENT_PUSH_OVER replaces exactly the head it names
other_push darkplaces a.c 'int replaced;'
h="$(remote_head darkplaces)"
echo 'int b5;' > "$GOD/engine/darkplaces/b.c"
if COMPONENT_PUSH_OVER=0123456789012345678901234567890123456789 push darkplaces "b5"; then
    fail "an override for another head let the push through"
elif COMPONENT_PUSH_OVER="$h" push darkplaces "b5" && [ "$(remote_head darkplaces)" != "$h" ]; then
    pass "COMPONENT_PUSH_OVER pushes over that head only"
else fail "override: $(cat "$T/out")"; fi

# 6. all: data moved, so darkplaces is not pushed either
other_push data q.qc 'void theirs() {}'
dp="$(remote_head darkplaces)"
echo 'int b6;' > "$GOD/engine/darkplaces/b.c"
if ! push all "both" && [ "$(remote_head darkplaces)" = "$dp" ] && grep -q '^data: refusing' "$T/out"; then
    pass "all: one moved component stops both"
else fail "all with a moved component: $(cat "$T/out")"; fi

[ "$fails" = 0 ] && echo "component push: all checks passed" || { echo "component push: $fails failed" >&2; exit 1; }
