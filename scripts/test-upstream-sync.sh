#!/bin/bash
# Offline end-to-end test of scripts/upstream-sync.py.
#
# Builds throwaway "upstream" repositories (a superproject at engine/ and a
# nested component at engine/lib) and a port repository with a Touch hook in
# an upstream file, then drives check / vendor / merge / resolve / hooks /
# patched through a clean sync, an asset-only change, a conflict resolved
# wrongly and then rightly, leftover conflict markers, a stale checkout and
# bad input. Needs only git and python3; touches nothing outside a temp dir.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME=test GIT_AUTHOR_EMAIL=test@example.invalid
export GIT_COMMITTER_NAME=test GIT_COMMITTER_EMAIL=test@example.invalid

n=0
ok() { n=$((n + 1)); echo "ok $n - $*"; }
fail() { echo "FAIL: $*" >&2; [ -f "$T/out" ] && sed 's/^/    /' "$T/out" >&2; exit 1; }

# run WANT_STATUS cmd...: run in the port repo, keep output in $T/out
run() {
    local want="$1"; shift
    set +e
    (cd "$T/port" && "$@") > "$T/out" 2>&1
    local got=$?
    set -e
    [ "$got" = "$want" ] || fail "'$*' exited $got, wanted $want"
}
has() { grep -q -- "$1" "$T/out" || fail "output lacks '$1'"; }
sync() { run "$1" python3 scripts/upstream-sync.py "${@:2}"; }

mk_upstream() {
    git init -q -b master "$T/$1"
    git -C "$T/$1" config uploadpack.allowFilter true
    git -C "$T/$1" config uploadpack.allowAnySHA1InWant true
}
up_commit() { git -C "$T/$1" add -A && git -C "$T/$1" commit -q -m "$2"; }
head_of() { git -C "$T/$1" rev-parse HEAD; }

# --- upstream ---------------------------------------------------------------
mk_upstream game
mk_upstream lib
cat > "$T/game/main.c" <<'EOF'
int start(void)
{
    int a = 1;
    int b = 2;
    int c = 3;
    return a + b + c;
}

int finish(void)
{
    return 0;
}
EOF
echo "upstream readme" > "$T/game/readme.txt"
mkdir -p "$T/game/assets" && echo "pixels v1" > "$T/game/assets/big.tga"
up_commit game "game v1"
echo "int lib(void) { return 1; }" > "$T/lib/lib.c"
up_commit lib "lib v1"

# --- port repository ----------------------------------------------------------
git init -q -b main "$T/port"
mkdir -p "$T/port/scripts" "$T/port/engine/lib"
cp "$HERE/upstream-sync.py" "$T/port/scripts/"
echo "engine/assets/" > "$T/port/.gitignore"
cp "$T/game/main.c" "$T/game/readme.txt" "$T/port/engine/"
cp "$T/lib/lib.c" "$T/port/engine/lib/"
# The Touch hook in an upstream file, and a Touch-only file.
sed -i 's/^    int b = 2;$/    int b = 2;\n    TouchHook_Start();/' "$T/port/engine/main.c"
echo "void TouchHook_Start(void) {}" > "$T/port/engine/touch_ui.c"
cat > "$T/port/upstream.lock" <<EOF
# name  path        branch  commit                url
game    engine      master  $(head_of game)  file://$T/game
lib     engine/lib  master  $(head_of lib)  file://$T/lib

vendor 0000000000000000000000000000000000000000
EOF
git -C "$T/port" add -A && git -C "$T/port" commit -q -m "port"

sync 0 vendor --at lock --root
vendor=$(git -C "$T/port" rev-parse upstream/vendor)
sed -i "s/^vendor .*/vendor $vendor/" "$T/port/upstream.lock"
git -C "$T/port" commit -q -am "record vendor"
[ -z "$(git -C "$T/port" ls-tree -r --name-only upstream/vendor | grep -E 'assets|touch_ui|scripts')" ] \
    || fail "vendor tree holds ignored, Touch-only or non-engine files"
ok "first vendor commit holds only upstream, non-ignored files"

sync 0 check
! grep -q UPDATE "$T/out" || fail "check reports an update right after vendoring"
sync 0 patched engine/main.c engine/touch_ui.c
has "engine/main.c: UPSTREAM code, already patched"
has "engine/touch_ui.c: Touch-only file"
ok "check is quiet and patched classifies files"

# --- 1. clean sync -------------------------------------------------------------
sed -i 's/^int finish(void)$/int finish_game(void)/' "$T/game/main.c"
echo "new upstream file" > "$T/game/new.txt"
git -C "$T/game" rm -q readme.txt
echo "pixels v2" > "$T/game/assets/big.tga"
up_commit game "game v2"
sync 0 check
has "UPDATE"
sync 0 vendor
sync 0 merge --report "$T/report.md"
grep -q "^| game " "$T/report.md" || fail "report lacks the game row"
merged() { git -C "$T/port" show "upstream-sync:$1"; }
merged engine/main.c | grep -q TouchHook_Start || fail "Touch hook lost in a clean merge"
merged engine/main.c | grep -q finish_game || fail "upstream change missing"
merged engine/new.txt > /dev/null || fail "new upstream file missing"
! merged engine/readme.txt > /dev/null 2>&1 || fail "file deleted upstream is still there"
! merged engine/assets/big.tga > /dev/null 2>&1 || fail "ignored asset was merged"
merged engine/touch_ui.c > /dev/null || fail "Touch-only file lost"
merged upstream.lock | grep -q "$(head_of game)" || fail "lock not updated to the new upstream commit"
merged upstream.lock | grep -q "^vendor $(git -C "$T/port" rev-parse upstream/vendor)$" \
    || fail "lock vendor line not updated"
sync 0 hooks --merged upstream-sync
ok "clean sync keeps the hook, takes upstream adds/deletes, skips assets, updates the lock"
git -C "$T/port" merge -q --ff-only upstream-sync

# --- 2. asset-only upstream change -----------------------------------------------
tip=$(git -C "$T/port" rev-parse upstream/vendor)
echo "pixels v3" > "$T/game/assets/big.tga"
up_commit game "game v3 (assets only)"
sync 0 vendor
[ "$(git -C "$T/port" rev-parse upstream/vendor)" = "$tip" ] || fail "asset-only change made a vendor commit"
sync 3 merge
ok "asset-only upstream change: no vendor commit, nothing to merge (exit 3)"

# --- 3. conflict on the hook's neighbour -----------------------------------------------
sed -i 's/^    int b = 2;$/    int b = 20;/' "$T/game/main.c"
up_commit game "game v4 (touches the line next to the hook)"
sync 0 vendor
sync 1 merge
has "engine/main.c"
ok "conflicting upstream change is reported (exit 1) with the file"

sync 0 resolve --allow-stale
git -C "$T/port" diff --name-only --diff-filter=U | grep -qx engine/main.c || fail "conflict not staged"
grep -q '^<<<<<<< ' "$T/port/engine/main.c" || fail "no conflict markers in the work tree"
ok "resolve stages the conflict like git merge"

# Wrong resolution: take upstream's side and drop the hook.
git -C "$T/port" checkout -q --theirs engine/main.c
git -C "$T/port" add engine/main.c && git -C "$T/port" commit -q --no-edit
[ "$(git -C "$T/port" rev-list --parents -n1 HEAD | wc -w)" = 3 ] || fail "resolve did not make a merge commit"
sync 1 hooks
has "TouchHook_Start"
ok "hooks catches a hook dropped while resolving"

# Leftover conflict markers.
{ echo "<<<<<<< HEAD"; cat "$T/port/engine/main.c"; echo ">>>>>>> upstream"; } > "$T/port/engine/x"
mv "$T/port/engine/x" "$T/port/engine/main.c"
sed -i 's/^    int b = 20;$/    int b = 20;\n    TouchHook_Start();/' "$T/port/engine/main.c"
git -C "$T/port" commit -q -a --amend --no-edit
sync 1 hooks
has "conflict markers"
ok "hooks catches committed conflict markers"

# Right resolution: both changes.
sed -i '/^<<<<<<< \|^>>>>>>> /d' "$T/port/engine/main.c"
git -C "$T/port" commit -q -a --amend --no-edit
sync 0 hooks
grep -q "int b = 20" "$T/port/engine/main.c" || fail "upstream side missing"
ok "hooks passes once both sides are kept"

# --- 4. resolve refuses a stale checkout -----------------------------------------------
git clone -q --bare "$T/port" "$T/origin.git"
git clone -q "$T/origin.git" "$T/other"
echo "newer Touch work" > "$T/other/touch_notes.txt"
git -C "$T/other" add -A && git -C "$T/other" commit -q -m "newer work on main"
git -C "$T/other" push -q origin main
git -C "$T/port" remote add origin "$T/origin.git"
sed -i 's/^int start(void)$/int start_game(void)/' "$T/game/main.c"
up_commit game "game v5"
sync 0 vendor
sync 2 resolve
has "does not contain origin/main"
ok "resolve refuses a checkout that is behind origin/main"

# --- 5. bad input is an error (2), not a conflict (1) -----------------------------------
sync 2 merge --vendor 0123456789abcdef0123456789abcdef01234567
sync 2 merge --vendor main
ok "missing or non-vendor commits exit 2"

echo "all $n upstream-sync checks passed"
