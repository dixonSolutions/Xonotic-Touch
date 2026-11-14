#!/bin/bash
# The progressive data download end to end (docs/DATA_FETCH.md): the loader's
# downloader (loader/crates/core, examples/datafetch) against a local stand-in
# for the release storage that serves a real split set, and a headless engine
# that plays while the rest comes in.
#
#   scripts/test-datafetch.sh SPLITDIR [PHONEDIR]
#   TIMINGS="5 20 100" RUNS=3 scripts/test-datafetch.sh SPLITDIR   also: time to first play per link speed
#
# SPLITDIR: godnotic-data.json, the pk3s and Godnotic-data-<profile>.zip
# (scripts/split-data-packs.py). PHONEDIR: the unsplit packs (for the old
# install); without it that step uses stand-in files.
# ENGINE (default build/bin/xonotic) runs offscreen; without one the engine
# steps are skipped. Nothing is copied: the data dirs hold what the downloader
# fetched (one full set at a time, about 400 MB for astc) and are removed.
set -euo pipefail
export LC_ALL=C
root="$(cd "$(dirname "$0")/.." && pwd)"
SPLIT="$(realpath "${1:?split directory}")"
PHONE="${2:-}"
ENGINE="${ENGINE:-$root/build/bin/xonotic}"
W="$(mktemp -d "${TMPDIR:-/tmp}/datafetch.XXXXXX")"
PORT=$((28000 + RANDOM % 2000))
SRV=""
cleanup() { [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; kill $(jobs -p) 2>/dev/null || true; [ -n "${KEEP:-}" ] || rm -rf "$W"; }
trap cleanup EXIT
fail=0
ok()  { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

DF="$root/loader/target/release/examples/datafetch"
[ -x "$DF" ] || (cd "$root/loader" && cargo build --offline --release -p godnotic-loader-core --features http-ureq --example datafetch >/dev/null 2>&1)
[ -x "$DF" ] || { echo "cannot build the datafetch example" >&2; exit 2; }

ZIP="$(ls "$SPLIT"/Godnotic-data-*.zip | head -1)"
PROFILE="$(basename "$ZIP" .zip)"; PROFILE="${PROFILE#Godnotic-data-}"
mkdir -p "$W/www/slim" "$W/www/r1"
ln -s "$ZIP" "$W/www/r1/$(basename "$ZIP")"
manifest() { # set-id suffix -> the published manifest (archive.url filled in)
	python3 - "$SPLIT/godnotic-data.json" "http://127.0.0.1:$PORT/r1/$(basename "$ZIP")" "$1" > "$W/www/slim/godnotic-data-$PROFILE.json.tmp" <<'PY'
import json, sys
m = json.load(open(sys.argv[1])); m["archive"]["url"] = sys.argv[2]; m["set"] += sys.argv[3]
json.dump(m, sys.stdout)
PY
	mv "$W/www/slim/godnotic-data-$PROFILE.json.tmp" "$W/www/slim/godnotic-data-$PROFILE.json"
}
manifest ""
q() { python3 - "$SPLIT/godnotic-data.json" "$@" <<'PY'
import json, sys
m = json.load(open(sys.argv[1])); P = {p["name"]: p for p in m["packs"]}
w = sys.argv[2]
if w == "first": print(sum(P[n]["size"] for n in m["first_play"]))
elif w == "total": print(sum(p["size"] for p in m["packs"]))
elif w == "count": print(len(m["packs"]))
elif w == "offset": print(P[sys.argv[3]]["offset"] + 1000)
elif w == "size": print(P[sys.argv[3]]["size"])
elif w == "needs": print("\n".join(P[n]["install_as"] for n in m["maps"][sys.argv[3]]["needs"]))
elif w == "sums": print("\n".join("%s  %s" % (p["sha256"], p["install_as"]) for p in m["packs"]))
PY
}
FIRST="$(q first)"; TOTAL="$(q total)"; COUNT="$(q count)"
URL="http://127.0.0.1:$PORT/slim/Godnotic-data.zip"
[ "$PROFILE" = dxt ] && FLAG=--dxt || FLAG=--astc

serve() { # extra linksrv args
	[ -n "$SRV" ] && { kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null || true; }
	rm -f "$W/stats"
	python3 "$root/tests/datafetch/linksrv.py" "$W/www" "$PORT" --stats "$W/stats" "$@" & SRV=$!
	for i in $(seq 1 50); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break; sleep 0.1; done
}
served() { awk -v p="r1/$(basename "$ZIP")" '$1==p{print $3; f=1} END{if(!f)print 0}' "$W/stats" 2>/dev/null || echo 0; }
requests() { awk -v p="r1/$(basename "$ZIP")" '$1==p{print $2; f=1} END{if(!f)print 0}' "$W/stats" 2>/dev/null || echo 0; }
packs() { ls "$1" 2>/dev/null | grep -c '^godnotic-.*\.pk3$' || true; }
mb() { echo "$(( ($1 + 524288) / 1048576 )) MB"; }

# --- 1. fresh install: playable after the first_play bytes -------------------
serve
D="$W/d1"; E="$W/e1"; mkdir -p "$D" "$E"
out="$("$DF" --url "$URL" --dir "$D" --engine-dir "$E" $FLAG --first-play-only)"
sleep 0.3
s="$(served)"
[ "$s" = "$FIRST" ] && ok "fresh install: the game may start after exactly the first_play bytes ($(mb "$s") of $(mb "$TOTAL")); ${out#first_play }" || bad "first play fetched $s bytes, first_play is $FIRST ($out)"
[ -e "$D/.assets-ready" ] && [ -s "$D/.godnotic-data.json" ] && ! ls "$D" | grep -q '\.part' && ok "the ready marker and the manifest are stored, no part left" || bad "after first play: $(ls -a "$D" | tr '\n' ' ')"

# --- 2. the rest in the background, a kill in the middle, a damaged pack -----
serve --mbit 400 --corrupt-once "$(q offset music)"
"$DF" --url "$URL" --dir "$D" --engine-dir "$E" $FLAG > "$W/run2.log" 2>&1 & pid=$!
for i in $(seq 1 600); do [ "$(served)" -gt $((TOTAL / 3)) ] && break; sleep 0.1; done
kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null || true
sleep 0.3
before="$(served)"; had="$(packs "$D")"
ls "$D" | grep -q '\.part$' && ok "killed with SIGKILL in the middle of a pack ($(mb "$before") served, $had packs installed, parts on disk)" || bad "no part on disk after the kill"
grep -q '^state fetching' "$E/touch/data-status.txt" && ok "the status file said fetching: $(sed -n '4,5p' "$E/touch/data-status.txt" | tr '\n' ' ')" || bad "status file: $(head -5 "$E/touch/data-status.txt" 2>/dev/null)"
"$DF" --url "$URL" --dir "$D" --engine-dir "$E" $FLAG > "$W/run3.log" 2>&1 || bad "the resumed run failed: $(cat "$W/run3.log")"
sleep 0.3
after="$(served)"; music="$(q size music)"
extra=$((FIRST + after - TOTAL - music))
[ "$(packs "$D")" = "$COUNT" ] && ! ls "$D" | grep -q '\.part' && ok "resumed to the whole set: $COUNT packs, no part left" || bad "after the resume: $(packs "$D") of $COUNT packs"
(cd "$D" && q sums | sha256sum -c --quiet) && ok "every pack has the manifest's sha256 (one was served damaged once and fetched again)" || bad "a pack differs from the manifest"
# what was fetched twice: the damaged music pack, and what the kill lost (unsaved chunk progress, socket buffers)
[ "$extra" -ge 0 ] && [ "$extra" -lt $((24 << 20)) ] && ok "fetched twice besides the damaged pack: $(mb "$extra") (lost to the kill)" || bad "resume fetched $extra bytes more than the set"
grep -q '^state complete' "$E/touch/data-status.txt" && ok "the status file ends with state complete" || bad "status: $(head -3 "$E/touch/data-status.txt")"
r="$(requests)"
"$DF" --url "$URL" --dir "$D" --engine-dir "$E" $FLAG >/dev/null 2>&1; sleep 0.2
[ "$(requests)" = "$r" ] && ok "a whole install asks the archive for nothing" || bad "a whole install made $(( $(requests) - r )) archive requests"

# --- 3. a new set id with the same packs: nothing fetched again --------------
manifest "-b"
"$DF" --url "$URL" --dir "$D" --engine-dir "$E" $FLAG >/dev/null 2>&1; sleep 0.2
[ "$(requests)" = "$r" ] && grep -q -- '-b$' <(head -1 "$D/.godnotic-set") && ok "a new set id whose packs are unchanged: adopted, nothing fetched" || bad "set change: $(( $(requests) - r )) requests, $(head -1 "$D/.godnotic-set")"
manifest ""
rm -rf "$D" "$E"

# --- 4. an old full install is left alone ------------------------------------
D="$W/d4"; E="$W/e4"; mkdir -p "$D" "$E"
for role in data maps music xoncompat; do
	src="$PHONE/xonotic-20261107-$role-phone.pk3"
	if [ -n "$PHONE" ] && [ -e "$src" ]; then ln -s "$src" "$D/xonotic-20261107-$role.pk3"; else echo stand-in > "$D/xonotic-20261107-$role.pk3"; fi
done
serve
"$DF" --url "$URL" --dir "$D" --engine-dir "$E" $FLAG >/dev/null 2>&1 || bad "the run over an old install failed"
sleep 0.2
[ "$(requests)" = 0 ] && [ "$(packs "$D")" = 0 ] && [ "$(ls "$D" | grep -c '^xonotic-')" = 4 ] && ok "a complete old install: no archive request, its packs untouched, $(cat "$D/.godnotic-set")" || bad "old install: $(requests) requests, $(ls -a "$D" | tr '\n' ' ')"
grep -q '^state legacy' "$E/touch/data-status.txt" && ok "the game is told state legacy" || bad "status: $(head -3 "$E/touch/data-status.txt")"
rm -rf "$D" "$E"

# --- 5. the engine plays while the rest comes in ----------------------------
if [ -x "$ENGINE" ]; then
	B="$W/base"; U="$W/user"; D="$B/data"; mkdir -p "$D" "$U/data/data"
	ln -s "$root/engine/data/xonotic-data.pk3dir" "$D/zzz-xonotic-data.pk3dir"
	ln -s "$root/engine/data/xonotic.cfg" "$D/xonotic.cfg"
	for f in "$root"/engine/data/font-*.pk3dir; do [ -e "$f" ] && ln -s "$f" "$D/$(basename "$f")"; done
	MAP2="${MAP2:-xoylent}"   # late in the queue
	serve --mbit "${PLAY_MBIT:-60}"
	"$DF" --url "$URL" --dir "$D" --engine-dir "$U/data/data" $FLAG > "$W/run5.log" 2>&1 & dl=$!
	for i in $(seq 1 600); do grep -q '^first_play' "$W/run5.log" && break; sleep 0.1; done
	at_start="$(packs "$D")"
	# no touch/ link here: the downloader keeps its music marker in <data>/touch
	timeout 300 env SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy "$ENGINE" -xonotic -basedir "$B" -userdir "$U" -condebug \
		+exec xonotic.cfg +vid_fullscreen 0 +vid_width 640 +vid_height 360 +set _touch_assets_ready 1 +sv_public 0 +bot_number 4 \
		+alias cl_hook_gamestart_all "defer 20 quit" +defer 4 "map $MAP2" >/dev/null 2>&1 &
	eng=$!
	L="$U/data/qconsole.log"
	for i in $(seq 1 400); do grep -a -q "still downloading" "$L" 2>/dev/null && break; sleep 0.25; done
	grep -a -q "map $MAP2 is still downloading" "$L" && ok "engine started on $at_start packs; map $MAP2 was held: '$(cat "$U/data/data/touch/data-request.txt" 2>/dev/null)'" || bad "the engine did not hold map $MAP2"
	for i in $(seq 1 600); do grep -a -q "Server spawned" "$L" 2>/dev/null && break; kill -0 "$eng" 2>/dev/null || break; sleep 0.25; done
	now="$(packs "$D")"
	grep -a -q "Server spawned" "$L" && ok "the match on $MAP2 started by itself with $now of $COUNT packs installed (the downloader put its needs first)" || bad "the match on $MAP2 never started"
	[ "$now" -lt "$COUNT" ] || echo "     note: the whole set was in before the match started; raise the work or lower PLAY_MBIT to see the jump"
	wait "$eng" 2>/dev/null || true
	wait "$dl" 2>/dev/null || bad "the background download failed: $(tail -2 "$W/run5.log")"
	n="$(grep -a -c '^.*fs_mountpack: godnotic-' "$L" || true)"
	grep -a -q "could not find" "$L" && bad "fs_mountpack missed a pack" || ok "$n packs were mounted into the running engine, none missed"
	miss="$(grep -a -i -E 'not found|couldn.t load|could not load|failed to load' "$L" | grep -a -v -E 'config\.cfg|autoexec|cdtracks|crypto|libcurl|d0_blind|\.so|server\.cfg|touch\.layout|userkey|BGM track|gfx/touch|_lod[0-9]*\.iqm|gfx/hud/default/progressbar' | sort -u || true)"
	[ -z "$miss" ] && ok "no missing-file line in the match on the late map" || { echo "     note: lines to look at (compare scripts/test-datafetch-engine.sh, which diffs against the whole set):"; echo "$miss" | head -8 | sed 's/^/       /'; }
	rm -rf "$B" "$U"
else
	echo "skip the engine steps: no engine at $ENGINE"
fi

# --- 6. time to first play per link speed ------------------------------------
for mbit in ${TIMINGS:-}; do
	t=()
	for run in $(seq 1 "${RUNS:-3}"); do
		D="$W/t"; rm -rf "$D"; mkdir -p "$D"
		serve --mbit "$mbit"
		out="$("$DF" --url "$URL" --dir "$D" $FLAG --first-play-only ${CONNECTIONS:+--connections $CONNECTIONS})"
		t+=("$(echo "$out" | awk '{print $2}')")
		rm -rf "$D"
	done
	med="$(printf '%s\n' "${t[@]}" | sort -n | sed -n "$(( (${#t[@]} + 1) / 2 ))p")"
	echo "time  $mbit Mbit/s: first play ($(mb "$FIRST")) after ${med} s (runs: ${t[*]}); the link alone needs $(python3 -c "print('%.1f' % ($FIRST * 8 / ($mbit * 1e6)))") s"
done

[ "$fail" = 0 ] && echo "PASS: datafetch" || { echo "FAILED: datafetch (KEEP=1 keeps $W)"; exit 1; }
