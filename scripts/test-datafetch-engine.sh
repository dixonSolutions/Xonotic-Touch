#!/bin/bash
# The game's half of the progressive data download (docs/DATA_FETCH.md), in a
# headless engine with a real split set:
#
#   1. only the first_play packs: a bot match on the default map prints no
#      missing-file line that the whole set does not print too;
#   2. "map <second map>" while that map is still to come is held back, the
#      request file names the map, and when the packs land (status file with
#      "landed" lines, nothing else) the engine mounts them and the match
#      starts, again with no new missing-file line;
#   3. joining a server on a third map that is still to come: no error, the
#      request, and the client is in the game after the packs land.
#
#   ENGINE=build/bin/xonotic scripts/test-datafetch-engine.sh SPLITDIR [MAP2 [MAP3]]
#
# SPLITDIR holds godnotic-data.json and the pk3s (scripts/split-data-packs.py).
# Offscreen and silent; needs LD_LIBRARY_PATH for libjpeg where the engine
# dlopens it.
set -euo pipefail
export LC_ALL=C
root="$(cd "$(dirname "$0")/.." && pwd)"
SPLIT="$(realpath "${1:?split directory}")"
MAP2="${2:-boil}"; MAP3="${3:-xoylent}"
ENGINE="$(realpath "${ENGINE:-$root/build/bin/xonotic}")"
SECS="${SECONDS_PER_MAP:-20}"
W="$(mktemp -d "${TMPDIR:-/tmp}/datafetch-engine.XXXXXX")"
trap 'kill $(jobs -p) 2>/dev/null || true; [ -n "${KEEP:-}" ] || rm -rf "$W"' EXIT
fail=0
ok()  { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

names() { python3 - "$SPLIT/godnotic-data.json" "$@" <<'PY'
import json, sys
m = json.load(open(sys.argv[1])); P = {p["name"]: p for p in m["packs"]}
what = sys.argv[2]
if what == "first": n = m["first_play"]
elif what == "default": print(m["default_map"]); sys.exit()
elif what == "all": n = [p["name"] for p in m["packs"]]
else: n = m["maps"][what]["needs"]
print("\n".join(P[x]["install_as"] for x in n))
PY
}
MAP1="$(names default)"

mkbase() { # dir, pack names...
	local D="$1"; shift
	mkdir -p "$D/data"
	for p in "$@"; do ln -sf "$SPLIT/$p" "$D/data/$p"; done
	ln -s "$root/engine/data/xonotic-data.pk3dir" "$D/data/zzz-xonotic-data.pk3dir"
	ln -s "$root/engine/data/xonotic.cfg" "$D/data/xonotic.cfg"
	ln -s "$root/touch" "$D/data/touch"
	for f in "$root"/engine/data/font-*.pk3dir; do [ -e "$f" ] && ln -s "$f" "$D/data/$(basename "$f")"; done
	return 0
}
run() { # basedir userdir seconds args...
	local B="$1" U="$2" T="$3"; shift 3
	timeout "$T" env SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy "$ENGINE" -xonotic -basedir "$B" -userdir "$U" -condebug \
		+exec xonotic.cfg +vid_fullscreen 0 +vid_width 640 +vid_height 360 +set _touch_assets_ready 1 \
		+sv_cheats 1 +bot_number 4 +sv_public 0 +skill 4 "$@" >/dev/null 2>&1 || true
}
# Known and reported, not failures: no music yet (it follows first_play), the
# rig has no gfx/touch (the packages stage it), a server that has the detail
# pack names LOD meshes the client does not have yet, and one picture of the
# default HUD skin that the data split left in extras.
KNOWN='BGM track|gfx/touch|_lod\.iqm|gfx/hud/default/progressbar'
missing() { grep -a -i -E 'not found|couldn.t load|could not load|failed to load|missing' "$1" | grep -a -v -E 'config\.cfg|autoexec|cdtracks|crypto|libcurl|d0_blind|\.so|server\.cfg|touch\.layout|font_size_snapping|userkey' | grep -a -v -E "$KNOWN" | sed -E 's/[0-9]+(\.[0-9]+)?//g' | sort -u || true; }
known() { grep -a -i -E "$KNOWN" "$1" | sed -E 's/\^[0-9]//g' | sort | uniq -c | sed 's/^/     note: /' | head -20 || true; }
status() { # userdir state [landed...]
	local U="$1" S="$2"; shift 2
	mkdir -p "$U/data/data/touch"
	{ echo "format 1"; echo "set test"; echo "state $S"; echo "done_kb 1000"; echo "total_kb 4000"; for p in "$@"; do echo "landed $p"; done; } > "$U/data/data/touch/data-status.tmp"
	mv "$U/data/data/touch/data-status.tmp" "$U/data/data/touch/data-status.txt"
}

mapfile -t FIRST < <(names first)
mapfile -t ALL < <(names all)

# --- baseline: the whole set ---------------------------------------------
mkbase "$W/whole" "${ALL[@]}"
for m in "$MAP1" "$MAP2"; do
	run "$W/whole" "$W/whole-u-$m" $((SECS + 90)) +alias cl_hook_gamestart_all "defer $SECS quit" +map "$m"
	missing "$W/whole-u-$m/data/qconsole.log" > "$W/base-$m.txt"
	grep -a -q "^Server spawned\|Server spawned" "$W/whole-u-$m/data/qconsole.log" || bad "baseline: no match on $m with the whole set"
done

# --- 1. first_play only ----------------------------------------------------
mkbase "$W/fp" "${FIRST[@]}"
run "$W/fp" "$W/fp-u" $((SECS + 90)) +alias cl_hook_gamestart_all "defer $SECS quit" +map "$MAP1"
L="$W/fp-u/data/qconsole.log"
grep -a -q "Server spawned" "$L" && ok "first_play only (${#FIRST[@]} packs): a match on $MAP1 starts" || bad "first_play only: no match on $MAP1"
missing "$L" > "$W/fp.txt"; known "$L"
new="$(comm -23 "$W/fp.txt" "$W/base-$MAP1.txt")"
[ -z "$new" ] && ok "first_play only: no missing-file line the whole set does not print ($(wc -l < "$W/fp.txt") lines, all in the baseline)" || { bad "first_play only: new missing-file lines:"; echo "$new" | head -20; }

# --- 2. a second map lands while the engine runs ---------------------------
mkbase "$W/late" "${FIRST[@]}"
U="$W/late-u"; status "$U" fetching
mapfile -t NEED2 < <(names "$MAP2")
run "$W/late" "$U" $((SECS * 2 + 150)) +alias cl_hook_gamestart_all "defer $SECS quit" +defer 6 "map $MAP2" &
req="$U/data/data/touch/data-request.txt"
for i in $(seq 1 240); do [ -s "$req" ] && break; sleep 0.5; done
if [ "$(cut -d' ' -f1-2 "$req" 2>/dev/null)" = "want $MAP2" ]; then ok "map $MAP2 while it is still to come: request file says '$(cat "$req")'"; else bad "no request for $MAP2 (got: $(cat "$req" 2>/dev/null))"; fi
sleep 2
grep -a -q "Server spawned" "$U/data/qconsole.log" && bad "the match started before the map was there" || ok "the match did not start before the packs landed"
late=()
for p in "${NEED2[@]}"; do [ -e "$W/late/data/$p" ] || { cp -l "$SPLIT/$p" "$W/late/data/$p.part" 2>/dev/null || cp "$SPLIT/$p" "$W/late/data/$p.part"; mv "$W/late/data/$p.part" "$W/late/data/$p"; late+=("$p"); status "$U" fetching "${late[@]}"; sleep 0.3; }; done
wait
L="$U/data/qconsole.log"
grep -a -q "Server spawned" "$L" && ok "${#late[@]} packs landed (status file only): mounted, and the held map $MAP2 started by itself" || bad "the held map did not start after its packs landed"
grep -a -q "could not find" "$L" && bad "fs_mountpack did not see a pack that had just landed" || ok "every landed pack was found at once"
missing "$L" > "$W/late.txt"; known "$L"
new="$(comm -23 "$W/late.txt" <(sort -u "$W/base-$MAP2.txt" "$W/base-$MAP1.txt"))"
[ -z "$new" ] && ok "late-mounted $MAP2: no missing-file line the whole set does not print" || { bad "late-mounted $MAP2: new missing-file lines:"; echo "$new" | head -20; }

# --- 3. joining a server whose map is still to come -------------------------
mkbase "$W/srv" "${ALL[@]}"
mkbase "$W/cl" "${FIRST[@]}"
PORT=$((27000 + RANDOM % 2000))
timeout $((SECS * 2 + 200)) env SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy "$ENGINE" -xonotic -dedicated -basedir "$W/srv" -userdir "$W/srv-u" -condebug \
	+exec xonotic.cfg +sv_public 0 +port "$PORT" +bot_number 2 +map "$MAP3" >/dev/null 2>&1 &
for i in $(seq 1 240); do grep -a -q "Server spawned\|Server listening" "$W/srv-u/data/qconsole.log" 2>/dev/null && break; sleep 0.5; done
U="$W/cl-u"; status "$U" fetching
mapfile -t NEED3 < <(names "$MAP3")
run "$W/cl" "$U" $((SECS + 150)) +cl_curl_enabled 0 +alias cl_hook_gamestart_all "defer $SECS quit" +defer 5 "connect 127.0.0.1:$PORT" &
req="$U/data/data/touch/data-request.txt"
for i in $(seq 1 300); do [ -s "$req" ] && break; sleep 0.5; done
[ "$(cut -d' ' -f1-2 "$req" 2>/dev/null)" = "want $MAP3" ] && ok "joining a server on $MAP3: request file says '$(cat "$req")'" || bad "join: no request for $MAP3 (got: $(cat "$req" 2>/dev/null))"
sleep 1
grep -a -q "cannot load map" "$U/data/qconsole.log" && bad "join: the cannot-load-map error showed" || ok "join: no cannot-load-map error"
grep -a -q "still downloading. Joining when it is here" "$U/data/qconsole.log" && ok "join: the player is told the map is downloading" || bad "join: no downloading message"
late=()
for p in "${NEED3[@]}"; do [ -e "$W/cl/data/$p" ] || { ln -s "$SPLIT/$p" "$W/cl/data/$p"; late+=("$p"); }; done
status "$U" fetching "${late[@]}"
est() { grep -a -c "Connection established" "$U/data/qconsole.log" 2>/dev/null || true; }
for i in $(seq 1 240); do [ "$(est)" -ge 2 ] && break; sleep 0.5; done
n="$(est)"
[ "$n" -ge 2 ] && ok "join: reconnected by itself after ${#late[@]} packs landed" || bad "join: no second connection (established $n times)"
sleep $((SECS / 2 + 10))
awk '/Connection established/{n++} n>=2' "$U/data/qconsole.log" > "$W/join-after.log"
grep -a -q "Connect: signon stage 4\|CL_SignonReply: 4" "$W/join-after.log" && ok "join: signed on to the game on $MAP3" || bad "join: did not finish signing on"
known "$W/join-after.log"
new="$(missing "$W/join-after.log" | comm -23 - <(sort -u "$W/base-$MAP1.txt" "$W/base-$MAP2.txt"))"
[ -z "$new" ] && ok "join: nothing missing after the reconnect (also what failed to load the first time)" || { bad "join: missing after the reconnect:"; echo "$new" | head -20; }
grep -a -q "cannot load map" "$U/data/qconsole.log" && bad "join: cannot-load-map after the packs landed" || ok "join: the world loaded after the packs landed"
kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null || true

[ "$fail" = 0 ] && echo "PASS: datafetch engine" || { echo "FAILED: datafetch engine (KEEP=1 keeps $W)"; exit 1; }
