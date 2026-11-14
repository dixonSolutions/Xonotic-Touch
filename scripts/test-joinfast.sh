#!/usr/bin/env bash
# Faster joins (docs/JOIN_SPEED.md), end to end on one machine: a stock
# Xonotic 0.8.6 dedicated server (the Flatpak's xonotic-dedicated on its own
# data) and this checkout's client, through scripts/bench-join.sh (a network
# namespace, a local HTTP server standing in for the csprogs release).
#
#   scripts/test-joinfast.sh                      # the logic only (CI)
#   BASEDIR=<client basedir> scripts/test-joinfast.sh [CASE...]
#
# Always first: tests/joinfast/csprogs_logic_test.c (reading the server's
# first message as it arrives, SHA-256) and scripts/csprogs-catalog.py on a
# made-up csprogs.dat; no engine needed. With BASEDIR, the cases below.
#
# CASE (default all):
#   timeline   a local start and a join to this checkout's server report every
#              phase (touch_jointime), the join without fetching anything
#   http       stock server, the release has its CSQC: fetched over HTTP from
#              the server's first fragment, the server is not asked
#   missing    the release does not have it (404): the server's download
#   corrupt    the release answers garbage under the right name: refused,
#              the server's download
#   slow       the release is too slow (touch_csprogs_timeout): the server's
#              download
#   off        touch_csprogs 0: the server's download, no HTTP request
#
# Needs unshare -rn and tc (bench-join.sh), python3, and the stock server
# (STOCK_SERVER / STOCKBASE as in bench-join.sh; SKIP when missing).
# OUT=<dir> keeps the logs.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LT="$(mktemp -d "${TMPDIR:-/tmp}/godnotic-joinfast-logic.XXXXXX")"
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -I"$ROOT/engine/darkplaces" "$ROOT/tests/joinfast/csprogs_logic_test.c" -o "$LT/logic" || { rm -rf "$LT"; exit 1; }
"$LT/logic" || { rm -rf "$LT"; exit 1; }
# the catalog: name, size, CRC (DarkPlaces' CRC-16 of "123456789" is 0x29b1), round trip
printf 123456789 > "$LT/csprogs.dat"
python3 "$ROOT/scripts/csprogs-catalog.py" "$LT/out" "test=$LT/csprogs.dat" > "$LT/line" && [ -f "$LT/out/csprogs.dat.9.10673.deflate" ] \
	&& python3 "$ROOT/scripts/csprogs-catalog.py" --check "$LT/out" > /dev/null && grep -q '{9, 10673, "15e2b0d3c33891ebb0f1ef609ec419420c20e320ce94c65fbc8c3312448eb225", "test"}' "$LT/line" \
	|| { echo "FAIL csprogs-catalog.py"; cat "$LT/line"; rm -rf "$LT"; exit 1; }
echo "ok   csprogs-catalog.py: name, size, CRC-16, SHA-256, raw deflate"
rm -rf "$LT"
if [ -z "${BASEDIR:-}" ]; then echo "test-joinfast: logic passed (no BASEDIR: engine cases not run)"; exit 0; fi
FLATPAK_XON=/var/lib/flatpak/app/org.xonotic.Xonotic/x86_64/stable/active/files
[ -d "$FLATPAK_XON" ] || FLATPAK_XON=/run/host$FLATPAK_XON
export STOCK_SERVER="${STOCK_SERVER:-$FLATPAK_XON/bin/xonotic-dedicated}"
export STOCKBASE="${STOCKBASE:-$FLATPAK_XON/share/xonotic}"
STOCKDATA=$(ls "$STOCKBASE"/data/xonotic-*-data.pk3 2> /dev/null | head -1)
if ! unshare -rn true 2> /dev/null || ! command -v tc > /dev/null; then echo "SKIP: needs unshare -rn and tc"; exit 77; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/godnotic-joinfast.XXXXXX")"
cleanup() {
	if [ -n "${OUT:-}" ]; then mkdir -p "$OUT" && cp -r "$WORK"/. "$OUT/" 2> /dev/null; fi
	rm -rf "$WORK"
}
trap cleanup EXIT
export PRIME_MAP=

fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }
has() { grep -aq -- "$2" "$1"; }

bench() { # out scenario [extra client args]
	bash "$ROOT/scripts/bench-join.sh" -n 1 -o "$WORK/$1" ${3:+-e "$3"} "$2" > "$WORK/$1.out" 2>&1
	ls "$WORK/$1"/*-1.log 2> /dev/null | grep -v '\.sv\.log$' | head -1
}

joined() { # log name
	if has "$1" "JOINTIME line" && has "$1" "first_frame="; then ok "$2: joined"; else bad "$2: did not reach the game (see $1)"; fi
}

# the release's files, a broken copy and an empty one
mkdir -p "$WORK/rel" "$WORK/corrupt" "$WORK/empty"
have_stock=0
if [ -x "$STOCK_SERVER" ] && [ -n "$STOCKDATA" ]; then
	have_stock=1
	python3 "$ROOT/scripts/csprogs-catalog.py" "$WORK/rel" "stock=$STOCKDATA" > "$WORK/catalog.txt" || bad "csprogs-catalog.py"
	python3 "$ROOT/scripts/csprogs-catalog.py" --check "$WORK/rel" > /dev/null || bad "csprogs-catalog.py --check"
	for f in "$WORK"/rel/*.deflate; do head -c 300000 /dev/urandom > "$WORK/corrupt/$(basename "$f")"; done
	# the build the engine knows must be the one the catalog made
	sha=$(cut -d'"' -f2 "$WORK/catalog.txt")
	if grep -qF "\"$sha\"" "$ROOT/engine/darkplaces/touch_csprogs.c"; then
		ok "touch_csprogs.c knows the stock build's SHA-256"
	else
		bad "touch_csprogs.c does not list the stock data's csprogs.dat: $(cat "$WORK/catalog.txt")"
	fi
fi

cases=("$@")
[ ${#cases[@]} -gt 0 ] || cases=(timeline http missing corrupt slow off)
for c in "${cases[@]}"; do
	if [ "$c" != timeline ] && [ $have_stock = 0 ]; then echo "SKIP $c: no stock server ($STOCK_SERVER)"; continue; fi
	case $c in
	timeline)
		log=$(bench local "local:stormkeep:0.5")
		joined "$log" "local start"
		line=$(grep -a "JOINTIME line" "$log" | tail -1)
		for e in tap spawn_begin spawn_end accept signon1 signon4 first_frame playable; do
			[[ "$line" == *" $e="* || "$line" == *"line $e="* ]] || bad "local start: no $e in the timeline ($line)"
		done
		[[ "$line" == *"by=tap"* ]] && ok "local start: timed from the tap" || bad "local start: not timed from the tap ($line)"
		log=$(bench gn "join:gn:40:csprogs=$WORK/rel")
		joined "$log" "join to a Godnotic server"
		line=$(grep -a "JOINTIME line" "$log" | tail -1)
		for e in challenge_tx challenge_rx connect_tx accept signon1 precache_end signon2 signon3 signon4 first_frame playable; do
			[[ "$line" == *" $e="* ]] || bad "join: no $e in the timeline ($line)"
		done
		[[ "$line" == *"csprogs=local"* ]] && ok "join: the bundled CSQC" || bad "join: CSQC not the bundled one ($line)"
		[ -s "$WORK/gn"/*.cshttp.log ] && grep -q GET "$WORK/gn"/*.cshttp.log && bad "join: asked the csprogs release for a Godnotic server's CSQC"
		;;
	http)
		log=$(bench http "join:stock:40:csprogs=$WORK/rel")
		joined "$log" "http"
		has "$log" "Fetched the server's game code" && ok "http: fetched over HTTP" || bad "http: no HTTP fetch"
		has "$log" "Downloading new CSQC" && bad "http: asked the server for its CSQC too" || ok "http: the server was not asked"
		has "$log" "csprogs=http" && ok "http: timeline says http" || bad "http: timeline does not say http"
		has "$log" "touch_csprogs: fetch started from the server's first fragment" || has "$log" "csprogs_begin=" || bad "http: no start mark"
		;;
	missing)
		log=$(bench missing "join:stock:0:csprogs=$WORK/empty")
		joined "$log" "missing"
		has "$log" "HTTP 404" && ok "missing: 404 noticed" || bad "missing: no 404"
		has "$log" "Downloading new CSQC" && ok "missing: the server's download" || bad "missing: no download from the server"
		;;
	corrupt)
		log=$(bench corrupt "join:stock:0:csprogs=$WORK/corrupt")
		joined "$log" "corrupt"
		has "$log" "not a deflated file\|checksum mismatch" && ok "corrupt: refused" || bad "corrupt: not refused"
		has "$log" "Downloading new CSQC" && ok "corrupt: the server's download" || bad "corrupt: no download from the server"
		has "$log" "program loaded (crc 18349" && ok "corrupt: the real CSQC runs" || bad "corrupt: CSQC 18349 not loaded"
		;;
	slow)
		log=$(bench slow "join:stock:0:csprogs=$WORK/rel,cskbps=4" "+set touch_csprogs_timeout 2")
		joined "$log" "slow"
		has "$log" "too slow" && ok "slow: gave up on HTTP" || bad "slow: did not give up"
		has "$log" "Downloading new CSQC" && ok "slow: the server's download" || bad "slow: no download from the server"
		;;
	off)
		log=$(bench off "join:stock:0:csprogs=$WORK/rel" "+set touch_csprogs 0")
		joined "$log" "off"
		has "$log" "Downloading new CSQC" && ok "off: the server's download" || bad "off: no download from the server"
		grep -q GET "$WORK/off"/*.cshttp.log 2> /dev/null && bad "off: asked HTTP anyway" || ok "off: no HTTP request"
		;;
	*) bad "unknown case $c" ;;
	esac
done
[ $fail = 0 ] && echo "test-joinfast: all passed" || echo "test-joinfast: FAILED"
exit $fail
