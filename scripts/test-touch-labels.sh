#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The labels on the engine's own keys (touch_ui.c) are drawn from a glyph map
# near their own size, so they stay sharp: on the web on a phone (1144x515
# pixels for 600 virtual units of height, 1.25 pixels per CSS pixel) the chat font's
# one 9-pixel size was stretched to twice that, blurred and misshapen. Runs the
# built engine offscreen at that size and at a desktop's, and asks
# `touch_ui_labelfont` what a label of each height draws with.
#
#   BASEDIR=<basedir with the stock pk3s and this clone's pk3dir> \
#   [ENGINE=build/bin/xonotic] scripts/test-touch-labels.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${ENGINE:-$ROOT/build/bin/xonotic}"
: "${BASEDIR:?set BASEDIR to a basedir with game data (run-suites.sh makes <clone>/.suite-base)}"
[ -x "$ENGINE" ] || { echo "no engine at $ENGINE (build it first)"; exit 1; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/touchlabels.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
fail=0

run() { # name width height conheight -> $tmp/<name>.log
	local name=$1 w=$2 h=$3 ch=$4 ud="$tmp/$1"
	mkdir -p "$ud/data"
	cat > "$ud/data/labels.cfg" <<EOF
set touch_profile_done 1
defer 4 "vid_conheight $ch"
defer 6 "echo LABELSCALE \$vid_width \$vid_height \$vid_conwidth \$vid_conheight; touch_ui_labelfont 14; touch_ui_labelfont 22; touch_ui_labelfont 30"
defer 7 "quit"
EOF
	SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy timeout 120 "$ENGINE" -xonotic -basedir "$BASEDIR" -userdir "$ud" \
		+vid_fullscreen 0 +vid_width "$w" +vid_height "$h" +exec labels.cfg > "$tmp/$name.log" 2>&1
}

check() { # name
	local name=$1 lines
	lines=$(grep "^touch_ui_labelfont " "$tmp/$name.log")
	echo "     $name: $(grep -m1 '^LABELSCALE' "$tmp/$name.log")"
	if [ "$(echo "$lines" | grep -c .)" != 3 ]; then
		echo "FAIL $name: touch_ui_labelfont printed nothing ($(tail -n 3 "$tmp/$name.log" | tr '\n' '|'))"
		fail=1
		return
	fi
	while read -r _ font _ want _ _ map _; do
		# the glyph map within a fifth of the pixels the label covers
		if awk -v w="$want" -v m="$map" 'BEGIN { exit !(m >= w * 0.8 && m <= w * 1.25) }'; then
			echo "ok   $name: a $want px label draws from a $map px map ($font)"
		else
			echo "FAIL $name: a $want px label draws from a $map px map ($font): stretched or shrunk"
			fail=1
		fi
	done <<< "$lines"
}

run phone-web 1144 515 600
check phone-web
run desktop 1280 720 600
check desktop
[ $fail = 0 ] && echo "touch-labels: all passed"
exit $fail
