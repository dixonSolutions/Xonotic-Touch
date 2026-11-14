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
# rig has no gfx/touch (the packages stage it), and a server that has the
# detail pack names LOD meshes the client does not have yet (its precache list
# carries them; the client's QuakeC uses a LOD only where fexists() finds it,
# so the full model stands in).
KNOWN='BGM track|gfx/touch|_lod[0-9]*\.iqm'
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

# --- 1b. sounds named .opus decode on the workers, as .ogg ones do ----------
if command -v ffmpeg >/dev/null 2>&1 && [ "$(ffmpeg -hide_banner -encoders 2>/dev/null | grep -c libopus)" -gt 0 ]; then
	mkbase "$W/snd" "${FIRST[@]}"
	mkdir -p "$W/snd/data/zzzz-sndtest.pk3dir/sound/xttest"
	for n in long:8 short:1; do
		ffmpeg -v error -f lavfi -i "sine=frequency=440:duration=${n#*:}" -ar 48000 -ac 1 -c:a libopus -b:a 32k "$W/snd/data/zzzz-sndtest.pk3dir/sound/xttest/${n%:*}.opus"
	done
	run "$W/snd" "$W/snd-u" 60 +developer 1 +defer 3 "play xttest/long.wav; play xttest/short.wav" +defer 6 quit
	L="$W/snd-u/data/qconsole.log"
	if grep -a -q "sound/xttest/long.opus (Opus) decodes on a worker" "$L" && ! grep -a -q "short.opus (Opus) decodes on a worker" "$L"; then
		ok "an 8 s sound named .opus decodes on a worker thread; a 1 s one is left for its first play"
	else
		bad "the .opus sound did not take the worker path: $(grep -a -i 'xttest\|snd_lazyload' "$L" | head -5)"
	fi
else
	echo "skip .opus sounds on the workers: no ffmpeg with libopus"
fi

# --- 2. a second map lands while the engine runs ---------------------------
mkbase "$W/late" "${FIRST[@]}"
U="$W/late-u"; status "$U" fetching
mapfile -t NEED2 < <(names "$MAP2")
# poll.cfg: commands the test hands the running engine (it execs the file twice a second)
: > "$U/data/poll.cfg"
run "$W/late" "$U" $((SECS * 2 + 150)) +alias cl_hook_gamestart_all "defer $SECS quit" +alias xt_poll "exec poll.cfg; defer 0.5 xt_poll" +defer 2 xt_poll +defer 6 "map $MAP2" &
req="$U/data/data/touch/data-request.txt"
for i in $(seq 1 240); do [ -s "$req" ] && break; sleep 0.5; done
if [ "$(cut -d' ' -f1-2 "$req" 2>/dev/null)" = "want $MAP2" ]; then ok "map $MAP2 while it is still to come: request file says '$(cat "$req")'"; else bad "no request for $MAP2 (got: $(cat "$req" 2>/dev/null))"; fi
sleep 2
grep -a -q "Server spawned" "$U/data/qconsole.log" && bad "the match started before the map was there" || ok "the match did not start before the packs landed"
poll() { # file to wait for, commands...
	local f="$U/data/$1"; shift
	printf '%s\n' "$@" > "$U/data/poll.cfg"
	for i in $(seq 1 60); do [ -s "$f" ] && break; sleep 0.5; done
	: > "$U/data/poll.cfg"
	[ -s "$f" ]
}
# the menu (UI bridge): the held map's dialog, the map rows' state, Cancel
poll hold1.json "uib_dump hold1.json" "uib_send {\"uib\":1,\"t\":\"window\",\"id\":91,\"source\":\"maps\",\"offset\":0,\"limit\":4,\"filter\":\"$MAP2\"}" "uib_send {\"uib\":1,\"t\":\"window\",\"id\":92,\"source\":\"maps\",\"offset\":0,\"limit\":4,\"filter\":\"$MAP1\"}" "screenshot hold1.png" || true
python3 - "$U/data/hold1.json" "$U/data/qconsole.log" "$MAP2" "$MAP1" <<'PY' && ok "menu: the held map's dialog (progress, Cancel) and the map rows' state ($MAP2 downloading, $MAP1 ready)" || bad "menu: no hold dialog or no map row state"
import json, re, sys
snap = json.load(open(sys.argv[1])); log = open(sys.argv[2], errors="replace").read()
nodes = {n["id"]: n for n in (snap.get("snapshot", snap)).get("nodes", [])}
app = next((n for n in nodes.values() if n.get("type") == "app"), {})
modal = app.get("props", {}).get("modal", [])
st = nodes.get("data.hold/status", {}).get("props", {})
ok = "data.hold" in modal and st.get("primary", {}).get("id") == "data.cancel" and sys.argv[3] in st.get("title", "")
rows = {}
for m in re.finditer(r'\{"key":"([^"]+)"[^{}]*"cells":\{([^{}]*)\}', log):
    rows[m.group(1)] = m.group(2)
ok = ok and '"state":"downloading"' in rows.get(sys.argv[3], "") and '"state":"ready"' in rows.get(sys.argv[4], "")
if not ok:
    print("     modal", modal, "status", st, "rows", {k: rows.get(k) for k in sys.argv[3:5]})
sys.exit(0 if ok else 1)
PY
poll hold2.json "uib_event {\"ev\":\"action\",\"node\":\"data.hold/status\",\"action\":\"data.cancel\"}" "defer 1 \"uib_dump hold2.json\"" || true
python3 - "$U/data/hold2.json" <<'PY' && ok "menu: Cancel drops the held map and closes its dialog" || bad "menu: Cancel left the dialog or the held map"
import json, sys
snap = json.load(open(sys.argv[1])); nodes = (snap.get("snapshot", snap)).get("nodes", [])
app = next((n for n in nodes if n.get("type") == "app"), {})
sys.exit(0 if "data.hold" not in app.get("props", {}).get("modal", []) else 1)
PY
grep -a -q "Server spawned" "$U/data/qconsole.log" && bad "the match started after Cancel" || true
poll hold3.json "map $MAP2" "defer 1 \"uib_dump hold3.json\"" || true
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
