#!/usr/bin/env bash
# Join and local-start timing, phase by phase (docs/JOIN_SPEED.md): each run
# starts the client on a fresh userdir, waits for its main menu, idles, then
# does what a tap does (`fe play` or `fe connect`) and waits until the engine
# reports the join playable (touch_jointime 1, engine/darkplaces/touch_jointime.c).
# The summary gives, per event, the median and the spread (min-max) in ms
# from the tap.
#
#   scripts/bench-join.sh [-n RUNS] [-o OUTDIR] [-e "extra client +args"] SCENARIO...
#
# SCENARIO:
#   local:MAP[:IDLE]            tap Play (fe play dm MAP 4 bots) after IDLE s
#                               in the menu (default 3: the menu's warm-up ran)
#   join:SERVER:RTT[:OPT,...]   tap a server: SERVER is gn (this checkout's
#                               dedicated server, the client's data) or stock
#                               (STOCK_SERVER on STOCKBASE: a stock Xonotic
#                               0.8.6 server); RTT in ms (tc netem on loopback
#                               in a user+network namespace, half each way)
#     OPT cached       the server's CSQC is in dlcache already (a returning
#                      player): copied from a first, unmeasured join
#         pack=MB      the server needs a MB map pack over HTTP
#                      (sv_curl_defaulturl), served at HTTP_KBPS per connection
#         csprogs=DIR  serve DIR over HTTP as the client's touch_csprogs_url
#                      (known CSQC builds, touch_csprogs.c)
#         cskbps=N     ... at N KB/s
#
# Environment:
#   BASEDIR       client data (and the gn server's): data/*.pk3 + the pk3dir
#   ENGINE        client (default build/bin/xonotic)
#   SV_ENGINE     gn server (default engine/darkplaces/darkplaces-dedicated)
#   STOCK_SERVER  stock server binary (default: the Xonotic Flatpak's 0.8.6
#                 xonotic-dedicated), STOCKBASE its basedir
#   LIBS          extra LD_LIBRARY_PATH (libjpeg.so.62)
#   HTTP_KBPS     per-connection cap of the pack server (default 2048)
#   CPUS          taskset CPU list for both processes (default 4-9)
#   W, H          window size (default 1280x720, offscreen SDL)
#   JOIN_PRE      console commands run with the tap on a server (tests)
#   PRIME_MAP     the template userdir plays one game here first, as a
#                 returning player has (default stormkeep; empty = none)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUNS=5
OUT=""
EXTRA=""
while getopts "n:o:e:h" opt; do
	case $opt in
		n) RUNS=$OPTARG ;;
		o) OUT=$OPTARG ;;
		e) EXTRA=$OPTARG ;;
		*) sed -n '2,38p' "$0"; exit 1 ;;
	esac
done
shift $((OPTIND - 1))
[ $# -gt 0 ] || { sed -n '2,38p' "$0"; exit 1; }
: "${BASEDIR:?set BASEDIR to the client game data dir (holding data/)}"
ENGINE="${ENGINE:-$ROOT/build/bin/xonotic}"
SV_ENGINE="${SV_ENGINE:-$ROOT/engine/darkplaces/darkplaces-dedicated}"
FLATPAK_XON=/var/lib/flatpak/app/org.xonotic.Xonotic/x86_64/stable/active/files
[ -d "$FLATPAK_XON" ] || FLATPAK_XON=/run/host$FLATPAK_XON
STOCK_SERVER="${STOCK_SERVER:-$FLATPAK_XON/bin/xonotic-dedicated}"
STOCKBASE="${STOCKBASE:-$FLATPAK_XON/share/xonotic}"
HTTP_KBPS="${HTTP_KBPS:-2048}"
CPUS="${CPUS:-4-9}"
W="${W:-1280}"; H="${H:-720}"
OUT="${OUT:-$(mktemp -d "${TMPDIR:-/tmp}/bench-join.XXXXXX")}"
mkdir -p "$OUT"
export LD_LIBRARY_PATH="${LIBS:+$LIBS:}${LD_LIBRARY_PATH:-}"
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy

client_args() { # userdir
	echo -xonotic -basedir "$BASEDIR" -userdir "$1" +exec xonotic.cfg +vid_fullscreen 0 \
		+vid_width "$W" +vid_height "$H" +set _touch_assets_ready 1 +set touch_jointime 1 +set touch_loadtime 1 $EXTRA
}

TEMPLATE="$OUT/userdir-template"
make_template() {
	[ -d "$TEMPLATE" ] && return
	mkdir -p "$TEMPLATE/data/touch"
	echo "echo startup.cfg" > "$TEMPLATE/data/touch/startup.cfg"
	cat > "$TEMPLATE/data/bench-quit.cfg" <<'EOF'
alias _bq_0 "defer 0.05 _bq_poll"
alias _bq_1 "defer 0.5 quit"
alias _bq_poll "_bq_$_touch_loadtime_menu"
_bq_poll
EOF
	# shellcheck disable=SC2046
	timeout 120 taskset -c "$CPUS" "$ENGINE" $(client_args "$TEMPLATE") +exec bench-quit.cfg > "$OUT/template.log" 2>&1 || true
	[ -f "$TEMPLATE/data/config.cfg" ] || { echo "template run made no config.cfg (see $OUT/template.log)" >&2; exit 1; }
	printf 'seta touch_profile_done 1\nseta _cl_name "Bench"\nseta cl_allow_uid2name 0\n' >> "$TEMPLATE/data/config.cfg"
	# a returning player: one game on PRIME_MAP played, so its preload list
	# and the menu's warm-up target exist (PRIME_MAP= for a first-time player)
	if [ -n "${PRIME_MAP-stormkeep}" ]; then
		write_job "$TEMPLATE" 1 "fe play dm ${PRIME_MAP-stormkeep} 4"
		# shellcheck disable=SC2046
		timeout 180 taskset -c "$CPUS" "$ENGINE" $(client_args "$TEMPLATE") +exec bench-job.cfg > "$OUT/template-prime.log" 2>&1 || true
		grep -q "JOINTIME line" "$OUT/template-prime.log" || { echo "priming game did not finish (see $OUT/template-prime.log)" >&2; exit 1; }
		rm -f "$TEMPLATE/data/bench-job.cfg"
	fi
}

fresh_userdir() { # name [dlcache dir]
	local u="$OUT/u-$1"
	rm -rf "$u"; cp -a "$TEMPLATE" "$u"
	if [ -n "${2:-}" ] && [ -d "$2" ]; then cp -a "$2" "$u/data/dlcache"; fi
	echo "$u"
}

# wait for the menu, idle, tap, wait for the timeline, quit
write_job() { # userdir idle command
	cat > "$1/data/bench-job.cfg" <<EOF
alias _bj_w0 "defer 0.05 _bj_wait"
alias _bj_w1 "defer $2 _bj_go"
alias _bj_wait "_bj_w\$_touch_loadtime_menu"
alias _bj_go "$3; _bj_gwait"
alias _bj_g0 "defer 0.05 _bj_gwait"
alias _bj_g1 "defer 0.3 quit"
alias _bj_gwait "_bj_g\$_touch_jointime_serial"
_bj_wait
EOF
}

run_local() { # map idle run
	local u log
	u=$(fresh_userdir "local-$1-$3"); log="$OUT/local-$1-$2-$3.log"
	write_job "$u" "$2" "fe play dm $1 4"
	# shellcheck disable=SC2046
	timeout 180 taskset -c "$CPUS" "$ENGINE" $(client_args "$u") +exec bench-job.cfg > "$log" 2>&1 || true
	echo "$log"
}

make_pack() { # MB dir: a valid pk3 holding MB of incompressible data
	mkdir -p "$2"
	[ -f "$2/zzz-benchpack-$1.pk3" ] && return
	python3 - "$1" "$2/zzz-benchpack-$1.pk3" <<'EOF'
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[2], "w", zipfile.ZIP_STORED) as z:
    z.writestr("benchpack-%s.dat" % sys.argv[1], os.urandom(int(float(sys.argv[1]) * 1048576)))
EOF
}

run_join() { # server rtt opts run [dlcache]
	local sv=$1 rtt=$2 opts=$3 r=$4 dl=${5:-} u log slog svbin svbase svu port=26123 svx="" clx="" pack="" csdir="" cskbps=0 o
	u=$(fresh_userdir "join-$sv-$rtt-$r" "$dl"); log="$OUT/join-$sv-$rtt-${opts//[,=\/]/_}-$r.log"; slog="${log%.log}.sv.log"
	svu="$OUT/svu-$r"; rm -rf "$svu"; mkdir -p "$svu/data"
	if [ "$sv" = stock ]; then svbin=$STOCK_SERVER; svbase=$STOCKBASE; else svbin=$SV_ENGINE; svbase=$BASEDIR; fi
	IFS=, read -ra o <<< "$opts"
	for x in "${o[@]}"; do
		case $x in
			pack=*) pack=${x#pack=}; make_pack "$pack" "$OUT/packs"; ln -sf "$OUT/packs/zzz-benchpack-$pack.pk3" "$svu/data/"
				svx="$svx +set sv_curl_defaulturl http://127.0.0.1:26180/ +set sv_curl_serverpackages benchpack-$pack.dat" ;;
			csprogs=*) csdir=${x#csprogs=}; clx="$clx +set touch_csprogs_url http://127.0.0.1:26181/" ;;
			cskbps=*) cskbps=${x#cskbps=} ;;
		esac
	done
	write_job "$u" 1 "${JOIN_PRE:+$JOIN_PRE; }fe connect 127.0.0.1:$port"
	# shellcheck disable=SC2016,SC2046
	unshare -rn bash -c '
		ip link set lo up
		[ "$1" != 0 ] && tc qdisc add dev lo root netem delay "$(( $1 / 2 ))ms"
		shift
		cpus=$1 svbin=$2 svbase=$3 svu=$4 slog=$5 port=$6 svx=$7 cl=$8 log=$9 pack=${10} packdir=${11} kbps=${12} csdir=${13} root=${14} cskbps=${15}; shift 15
		pids=""
		if [ -n "$pack" ]; then python3 "$root/tests/net/range_http.py" "$packdir" 26180 "$kbps" 2> "${slog%.sv.log}.http.log" & pids="$pids $!"; fi
		if [ -n "$csdir" ]; then python3 "$root/tests/net/range_http.py" "$csdir" 26181 "$cskbps" 2> "${slog%.sv.log}.cshttp.log" & pids="$pids $!"; fi
		# shellcheck disable=SC2086
		taskset -c "$cpus" "$svbin" -dedicated -xonotic -basedir "$svbase" -userdir "$svu" +set port "$port" +set sv_public 0 $svx +map stormkeep > "$slog" 2>&1 &
		svpid=$!
		for i in $(seq 1 300); do grep -aq "Server spawned\|SpawnServer\|Server listening" "$slog" 2>/dev/null && break; sleep 0.05; done
		sleep 1
		timeout 300 taskset -c "$cpus" "$cl" "$@" > "$log" 2>&1 || true
		kill $svpid $pids 2>/dev/null; wait 2>/dev/null || true
	' bash "$rtt" "$CPUS" "$svbin" "$svbase" "$svu" "$slog" "$port" "$svx" "$ENGINE" "$log" "$pack" "$OUT/packs" "$HTTP_KBPS" "$csdir" "$ROOT" "$cskbps" $(client_args "$u") $clx +exec bench-job.cfg
	rm -rf "$svu"
	echo "$log"
}

summarise() { # label logs...
	python3 - "$@" <<'EOF'
import re, sys, statistics
label, logs = sys.argv[1], sys.argv[2:]
order = ["tap", "spawn_begin", "spawn_end", "challenge_tx", "challenge_rx", "connect_tx", "accept", "signon1",
         "curl_begin", "curl_end", "csprogs_begin", "csprogs_end", "precache_end", "signon2", "signon3",
         "csqc_begin", "csqc_end", "signon4", "first_frame", "playable"]
rows, srcs, missing = {}, [], 0
for path in logs:
    line = None
    for l in open(path, errors="replace"):
        if "JOINTIME line " in l:
            line = l.split("JOINTIME line ", 1)[1].strip()
    if not line:
        missing += 1
        continue
    kv = dict(x.split("=", 1) for x in line.split() if "=" in x)
    srcs.append(kv.get("csprogs", "?"))
    for k in order:
        if k in kv:
            rows.setdefault(k, []).append(float(kv[k]))
print("== %s: %d runs%s, csprogs from %s" % (label, len(logs) - missing, " (%d without a timeline)" % missing if missing else "", ",".join(sorted(set(srcs))) or "-"))
for k in order:
    v = rows.get(k)
    if v:
        print("  %-14s %8.0f ms  (%.0f-%.0f)%s" % (k, statistics.median(v), min(v), max(v), "" if len(v) == len(logs) - missing else "  [%d runs]" % len(v)))
EOF
}

make_template
for sc in "$@"; do
	IFS=: read -r kind a b c <<< "$sc"
	logs=()
	case $kind in
		local)
			for r in $(seq 1 "$RUNS"); do logs+=("$(run_local "$a" "${b:-3}" "$r")"); done ;;
		join)
			dl=""
			if [[ ",${c:-}," == *,cached,* ]]; then
				first=$(run_join "$a" "$b" "${c:-}" prime)
				dl="$OUT/dlcache-$a"; rm -rf "$dl"
				[ -d "$OUT/u-join-$a-$b-prime/data/dlcache" ] && cp -a "$OUT/u-join-$a-$b-prime/data/dlcache" "$dl"
				# a cached join keeps the CSQC only; packs come again
				find "$dl" -name "*.pk3" -delete 2>/dev/null || true
				echo "primed dlcache from $first: $(ls "$dl" 2>/dev/null | tr '\n' ' ')" >&2
			fi
			for r in $(seq 1 "$RUNS"); do logs+=("$(run_join "$a" "$b" "${c:-}" "$r" "$dl")"); done ;;
		*) echo "unknown scenario $sc" >&2; exit 1 ;;
	esac
	summarise "$sc" "${logs[@]}" | tee -a "$OUT/summary.txt"
	rm -rf "$OUT"/u-local-* "$OUT"/u-join-*
done
echo "logs and summary.txt in $OUT"
