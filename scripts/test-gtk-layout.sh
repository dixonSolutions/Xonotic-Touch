#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The GTK front end (loader/crates/gtk) on fixtures, on a private Xvfb: sizes
# and positions read back with the UI script's `measure` command.
#
#   Home at 400x860       the three cards in one row inside the window (their
#                         minimum widths added up to 620 px)
#   Map previews          loader/scripts/drive.sh and shot.sh hand the fixture
#                         player real pk3s, so map pictures arrive (they passed
#                         an empty GODNOTIC_DATA_DIRS, which read nothing)
#   Singleplayer 915x480  a phone on its side: the map picker sits whole above
#                         the action bar with room for its tiles (it was a 36 px
#                         sliver under a strip of tall mode cards)
#
#   scripts/test-gtk-layout.sh   (needs loader/target/release/godnotic-loader:
#                                 loader/build.sh, Xvfb and org.gnome.Platform//48)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${GODNOTIC_LOADER_BIN:-$ROOT/loader/target/release/godnotic-loader}"
if [ ! -x "$BIN" ] || ! command -v Xvfb > /dev/null || ! flatpak info org.gnome.Platform//48 > /dev/null 2>&1; then
	echo "SKIP: needs $BIN (loader/build.sh), Xvfb and org.gnome.Platform//48"
	exit 0
fi
# under home: the Flatpak sandbox does not see the host's /tmp
WORK="$(mktemp -d "$HOME/.cache/godnotic-gtk-layout.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

measure() { # fixture WxH node... -> $WORK/<fixture>-<WxH>.txt
	local f=$1 size=$2 out="$WORK/$1-$2"
	shift 2
	mkdir -p "$out"
	printf 'wait 2000\nmeasure %s/m.txt %s\nsnapshot %s/shot.png\nquit\n' "$out" "$*" "$out" > "$out/run.script"
	FIXTURE="$f" timeout 90 "$ROOT/loader/scripts/drive.sh" "$out/run.script" "$out" "$size" dark > "$out/log.txt" 2>&1
	cp "$out/m.txt" "$WORK/$f-$size.txt" 2> /dev/null || : > "$WORK/$f-$size.txt"
}
field() { # file key n -> the n-th field after the key
	awk -v k="$2" -v n="$3" '$1 == k { print $(n + 1); exit }' "$1"
}

measure home 400x860 home/tiles/sp home/tiles/campaign home/tiles/mp
m="$WORK/home-400x860.txt"
min=$(field "$m" window 4); y1=$(field "$m" home/tiles/sp 2); y2=$(field "$m" home/tiles/campaign 2); y3=$(field "$m" home/tiles/mp 2)
x3=$(field "$m" home/tiles/mp 1); w3=$(field "$m" home/tiles/mp 3)
if [ -n "$min" ] && [ "$min" -le 400 ] && [ -n "$y1" ] && [ "$y1" = "$y2" ] && [ "$y2" = "$y3" ] && [ $((x3 + w3)) -le 400 ]; then
	ok "Home at 400x860: the content needs $min px, the three cards side by side end at $((x3 + w3)) px"
else
	bad "Home at 400x860: wider than the window or not one row ($(tr '\n' '|' < "$m"))"
fi

measure singleplayer 915x480 sp/maps/list sp:play
m="$WORK/singleplayer-915x480.txt"
pics=$(field "$m" map-pictures 1)
. "$ROOT/loader/scripts/data-dirs.sh"
if [ -n "$pics" ] && [ "$pics" -gt 0 ]; then
	ok "Map previews: $pics map pictures arrived from ${GN_DATA_DIRS:-nowhere}"
elif ! for d in ${GN_DATA_DIRS//:/ }; do ls "$d" "/run/host$d"; done 2> /dev/null | grep -qi 'maps.*\.pk3$\|\.jpg$'; then
	echo "SKIP map previews: no game data found (set GODNOTIC_DATA_DIRS to a folder of pk3s)"
else
	bad "Map previews: none arrived although $GN_DATA_DIRS has data ($(tr '\n' '|' < "$m"))"
fi
ly=$(field "$m" sp/maps/list 2); lh=$(field "$m" sp/maps/list 4); py=$(field "$m" sp:play 2)
# the action bar starts about 10 px above its Play button
if [ -n "$ly" ] && [ -n "$py" ] && [ $((ly + lh)) -le $((py - 8)) ] && [ "$lh" -ge 140 ]; then
	ok "Singleplayer at 915x480: the map picker is ${lh} px tall and ends at $((ly + lh)), above Play at $py"
else
	bad "Singleplayer at 915x480: the map picker is cut or cramped ($(tr '\n' '|' < "$m"))"
fi

[ $fail = 0 ] && echo "gtk-layout: all passed"
exit $fail
