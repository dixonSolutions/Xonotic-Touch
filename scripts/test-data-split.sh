#!/bin/bash
# Checks a split data set (scripts/split-data-packs.py) and, given an engine,
# loads every map from the core and that map's own packs only.
#
#   scripts/test-data-split.sh                      self-test on a made-up set (tests.yml)
#   scripts/test-data-split.sh SPLITDIR PHONEDIR    the static checks below on a real set
#   ENGINE=path scripts/test-data-split.sh SPLITDIR PHONEDIR   ... and the engine runs
#   ENGINE=path scripts/test-data-split.sh --trace OUTDIR PHONEDIR
#                                                   record usage traces from a full set and
#                                                   write OUTDIR/usage.tsv for --usage
#
# Static checks: the manifest's sizes, sha256 and crc32 match the files; the
# archive (if any) holds each pack at the offset the manifest gives; every
# entry of the phone packs is in exactly one split pack, byte for byte (same
# name, method, CRC and payload), so the union is the unsplit set and no byte
# is duplicated; no name is in two packs; a symlink's target is in its pack or
# the core; every map's needs list exists and holds its bsp.
#
# Engine runs (ENGINE, offscreen and silent): for each map, a basedir with the
# core, the map's needs and nothing else; a short bot match with fs_opentrace.
# Every entry the same match opened on the whole set must be opened here too,
# and the console must not show a missing file the whole set does not.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
PY=${PYTHON:-python3}
SECONDS_PER_MAP=${SECONDS_PER_MAP:-20}
JOBS=${JOBS:-6}

static_checks() { # SPLITDIR PHONEDIR
	"$PY" -I - "$1" "$2" <<'EOF'
import hashlib, json, os, sys, zipfile, zlib, struct
split, phone = sys.argv[1:3]
man = json.load(open(os.path.join(split, "godnotic-data.json")))
pm = json.load(open(os.path.join(phone, "phone-manifest.json")))
err = []
by = {}
for r in man["packs"]:
    p = os.path.join(split, r["file"])
    d = open(p, "rb").read()
    if len(d) != r["size"]: err.append("size " + r["file"])
    if hashlib.sha256(d).hexdigest() != r["sha256"]: err.append("sha256 " + r["file"])
    if "%08x" % zlib.crc32(d) != r["crc32"]: err.append("crc32 " + r["file"])
    by[r["name"]] = r
if "archive" in man:
    a = os.path.join(split, man["archive"]["file"])
    with open(a, "rb") as f:
        if os.path.getsize(a) != man["archive"]["size"]: err.append("archive size")
        for r in man["packs"]:
            f.seek(r["offset"])
            if f.read(r["size"]) != open(os.path.join(split, r["file"]), "rb").read():
                err.append("archive range " + r["file"])
    with zipfile.ZipFile(a) as z:
        names = [i.filename for i in z.infolist()]
        if names != [man["archive"]["folder"] + r["install_as"] for r in man["packs"]]: err.append("archive names")
        if any(i.compress_type for i in z.infolist()): err.append("archive entries must be stored")
def raw(path, info):
    with open(path, "rb") as f:
        f.seek(info.header_offset); h = f.read(30)
        n, e = struct.unpack_from("<HH", h, 26); f.seek(n + e, 1)
        return f.read(info.compress_size)
src, seen = {}, set()
for r in sorted(pm["packs"], key=lambda r: r["install_as"]):  # mount order: a later name shadows
    if "nexcompat" in r["install_as"]: continue
    p = os.path.join(phone, r["file"])
    for i in zipfile.ZipFile(p).infolist():
        if i.is_dir(): continue
        for k in [k for k in src if k[0] == i.filename] if i.filename in seen else []:
            del src[k]
        seen.add(i.filename)
        src[i.filename, i.CRC, i.compress_type] = 1
out, where, links = {}, {}, []
for r in man["packs"]:
    p = os.path.join(split, r["file"])
    z = zipfile.ZipFile(p)
    if z.testzip() is not None: err.append("bad entry in " + r["file"])
    for i in z.infolist():
        k = (i.filename, i.CRC, i.compress_type)
        out[k] = out.get(k, 0) + 1
        if i.filename in where: err.append("%s in %s and %s" % (i.filename, where[i.filename], r["name"]))
        where[i.filename] = r["name"]
        if (i.external_attr >> 16) & 0o170000 == 0o120000:
            t = os.path.normpath(os.path.join(os.path.dirname(i.filename), z.read(i).decode())).replace(os.sep, "/")
            links.append((i.filename, t, r["name"]))
if out != src:
    miss = [k for k in src if src[k] != out.get(k, 0)]; extra = [k for k in out if out[k] != src.get(k, 0)]
    err.append("union differs from the phone set: %d missing/changed, %d extra, e.g. %s" % (len(miss), len(extra), (miss + extra)[:3]))
for l, t, p in links:
    if t in where and where[t] not in (p, "core"):
        err.append("link %s (%s) -> %s (%s)" % (l, p, t, where[t]))
for m, rec in man["maps"].items():
    for n in rec["needs"]:
        if n not in by: err.append("map %s needs missing pack %s" % (m, n))
    if where.get("maps/%s.bsp" % m) not in rec["needs"]: err.append("map %s: bsp not in its needs" % m)
for n in man["first_play"]:
    if n not in by: err.append("first_play pack %s missing" % n)
size = sum(r["size"] for r in man["packs"])
print("%d packs, %.1f MB, %d entries, first play %.1f MB" % (len(by), size / 1e6, sum(out.values()),
      sum(by[n]["size"] for n in man["first_play"]) / 1e6))
if err:
    print("\n".join("FAIL: " + e for e in err[:40])); sys.exit(1)
print("ok: manifest, archive ranges, union = phone set, no duplicates, links, map needs")
EOF
}

engine_run() { # BASEDIR NAME MAP GAMETYPE_CVAR OUTDIR
	local B=$1 N=$2 M=$3 G=$4 OUT=$5 U
	U=$OUT/u-$N
	rm -rf "$U"; mkdir -p "$U/data/touch"; : > "$U/data/touch/startup.cfg"
	timeout $((SECONDS_PER_MAP + 120)) env SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy "$ENGINE" -xonotic \
		-basedir "$B" -userdir "$U" -condebug +exec xonotic.cfg +vid_fullscreen 0 +vid_width 640 +vid_height 360 \
		+set _touch_assets_ready 1 +set fs_opentrace opened.tsv +sv_cheats 1 +g_dm 0 +"$G" 1 +bot_number 5 \
		+minplayers 6 +skill 6 +g_warmup 0 +vid_touchscreen 0 \
		+alias cl_hook_gamestart_all "defer $SECONDS_PER_MAP quit" ${M:++map $M} ${M:-+defer 12 quit} >/dev/null 2>&1 || true
	cp "$U/data/opened.tsv" "$OUT/$N.tsv" 2>/dev/null || : > "$OUT/$N.tsv"
	cp "$U/data/qconsole.log" "$OUT/$N.log" 2>/dev/null || : > "$OUT/$N.log"
	rm -rf "$U"
}

gametype_cvar() { # first gametype of a map's mapinfo -> its cvar
	case $1 in
	ka) echo g_keepaway ;; dom) echo g_domination ;; ft) echo g_freezetag ;; kh) echo g_keyhunt ;;
	rc) echo g_race ;; surv) echo g_survival ;; nb) echo g_nexball ;; ons) echo g_onslaught ;;
	as) echo g_assault ;; inv) echo g_invasion ;; *) echo "g_$1" ;;
	esac
}

# basedir: the packs given, plus the game code and configs of this checkout
make_base() { # DIR packs...
	local D=$1; shift
	rm -rf "$D"; mkdir -p "$D/data"
	for p in "$@"; do ln -s "$(realpath "$p")" "$D/data/$(basename "$p")"; done
	local root; root=$(cd "$here/.." && pwd)
	ln -s "$root/engine/data/xonotic-data.pk3dir" "$D/data/zzz-xonotic-data.pk3dir"
	ln -s "$root/engine/data/xonotic.cfg" "$D/data/xonotic.cfg"
	ln -s "$root/touch" "$D/data/touch"
	for f in "$root"/engine/data/font-*.pk3dir; do [ -e "$f" ] && ln -s "$f" "$D/data/$(basename "$f")"; done
	return 0
}

maps_of() { # PHONEDIR -> "map gametype" lines
	"$PY" -I - "$1" <<'EOF'
import json, os, sys, zipfile
d = sys.argv[1]
for r in json.load(open(os.path.join(d, "phone-manifest.json")))["packs"]:
    if r["install_as"].endswith("-maps.pk3"):
        z = zipfile.ZipFile(os.path.join(d, r["file"]))
        for n in sorted(z.namelist()):
            if n.startswith("maps/") and n.endswith(".mapinfo") and n.count("/") == 1:
                gt = next((l.split()[1] for l in z.read(n).decode("utf-8", "replace").splitlines()
                           if l.startswith("gametype ") and len(l.split()) > 1), "dm")
                print(n[5:-8], gt)
EOF
}

phone_packs() { # PHONEDIR -> pk3 paths named as installed
	"$PY" -I - "$1" "$2" <<'EOF'
import json, os, sys
d, out = sys.argv[1:3]
os.makedirs(out, exist_ok=True)
for r in json.load(open(os.path.join(d, "phone-manifest.json")))["packs"]:
    if "nexcompat" in r["install_as"]: continue
    l = os.path.join(out, r["install_as"])
    if not os.path.islink(l): os.symlink(os.path.realpath(os.path.join(d, r["file"])), l)
    print(l)
EOF
}

self_test() {
	tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
	"$PY" -I - "$tmp" <<'EOF'
import json, os, sys, zipfile
t = sys.argv[1]
os.makedirs(t + "/phone")
def pk3(name, files, links=()):
    with zipfile.ZipFile(t + "/phone/" + name, "w", zipfile.ZIP_DEFLATED) as z:
        for n, d in files.items(): z.writestr(n, d)
        for n, target in links:
            i = zipfile.ZipInfo(n); i.external_attr = 0o120777 << 16; i.create_system = 3; z.writestr(i, target)
pk3("xonotic-20261107-data-phone.pk3", {"models/a.md3": b"a" * 300, "models/player/x_lod1.iqm": b"l" * 300,
    "gfx/menu.jpg": b"m" * 300, "sound/unused.ogg": b"u" * 300, "dds/textures/core.ktx": b"c" * 300,
    "gfx/hud/luma/progressbar.tga": b"p" * 50, "gfx/hud/default/progressbar.jpg": b"P" * 50,
    "gfx/hud/luma/ammo.tga": b"a" * 50, "gfx/hud/default/ammo.tga": b"A" * 50})
pk3("xonotic-20261107-maps-phone.pk3", {
    "maps/one.bsp": b"1" * 500, "maps/two.bsp": b"2" * 500, "maps/three.bsp": b"3" * 500,
    "maps/one.mapinfo": b"gametype dm", "maps/two.mapinfo": b"gametype dm", "maps/three.mapinfo": b"gametype dm",
    "maps/one.jpg": b"j", "maps/one/lm_0000.jpg": b"L" * 400, "scripts/one.shader": b"s",
    "dds/textures/shared.ktx": b"S" * 400, "dds/textures/own1.ktx": b"o" * 400, "dds/textures/target.ktx": b"T" * 50,
    "dds/textures/nobody.ktx": b"n" * 400, "sound/map_two/x.ogg": b"x" * 100},
    links=[("dds/textures/link.ktx", "target.ktx")])
pk3("xonotic-20261107-music-phone.pk3", {"sound/cdtracks/a.ogg": b"music" * 50})
recs = [{"file": f, "install_as": f.replace("-phone", "")} for f in sorted(os.listdir(t + "/phone"))]
json.dump({"format": 1, "packs": recs}, open(t + "/phone/phone-manifest.json", "w"))
U = [("data", "models/a.md3", "one,two,three"), ("data", "models/player/x_lod1.iqm", "one"),
     ("data", "gfx/menu.jpg", "menu"), ("data", "dds/textures/core.ktx", "one,two,three"),
     ("maps", "maps/one.bsp", "one"), ("maps", "maps/two.bsp", "two"), ("maps", "maps/three.bsp", "three"),
     ("maps", "maps/one/lm_0000.jpg", "one"), ("maps", "dds/textures/shared.ktx", "one,two"),
     ("maps", "dds/textures/own1.ktx", "one"), ("maps", "dds/textures/link.ktx", "two"),
     ("maps", "dds/textures/target.ktx", "two"), ("maps", "maps/one.mapinfo", "menu")]
with open(t + "/usage.tsv", "w") as f:
    for p, n, u in U: f.write("xonotic-20261107-%s.pk3\t%s\t%s\n" % (p, n, u))
EOF
	"$PY" -I "$here/split-data-packs.py" --usage "$tmp/usage.tsv" --default-map one --archive \
		-o "$tmp/split" "$tmp/phone" 2>"$tmp/log" || { cat "$tmp/log"; exit 1; }
	static_checks "$tmp/split" "$tmp/phone"
	"$PY" -I - "$tmp/split" <<'EOF'
import json, sys, zipfile, os
d = sys.argv[1]; m = json.load(open(d + "/godnotic-data.json"))
w = {}
for r in m["packs"]:
    for n in zipfile.ZipFile(os.path.join(d, r["file"])).namelist(): w[n] = r["name"]
exp = {"models/a.md3": "core", "models/player/x_lod1.iqm": "detail", "gfx/menu.jpg": "core",
       "sound/unused.ogg": "extras", "maps/one.bsp": "map-one", "maps/one/lm_0000.jpg": "map-one",
       "maps/two.mapinfo": "core", "scripts/one.shader": "core", "dds/textures/own1.ktx": "map-one",
       "dds/textures/nobody.ktx": "extras", "sound/map_two/x.ogg": "map-two",
       "dds/textures/link.ktx": "map-two", "dds/textures/target.ktx": "map-two",
       "sound/cdtracks/a.ogg": "music",
       # hud.qc draws gfx/hud/default/progressbar by that path: core even where the skin has its own
       "gfx/hud/default/progressbar.jpg": "core", "gfx/hud/default/ammo.tga": "extras", "gfx/hud/luma/ammo.tga": "core"}
bad = {k: (w.get(k), v) for k, v in exp.items() if w.get(k) != v}
assert not bad, bad
s = w["dds/textures/shared.ktx"]
assert s.startswith("shared-"), s
assert s in m["maps"]["one"]["needs"] and s in m["maps"]["two"]["needs"] and s not in m["maps"]["three"]["needs"], m["maps"]
assert m["first_play"][0] == "core" and m["first_play"][-1] == "map-one", m["first_play"]
names = [r["name"] for r in m["packs"]]
assert names.index("music") < names.index("map-two") < names.index("extras"), names
print("ok: assignment of core, map, shared, detail, music and extras entries; order")
EOF
}

if [ $# -eq 0 ]; then
	self_test
	exit 0
fi

if [ "$1" = --trace ]; then
	[ -n "${ENGINE:-}" ] || { echo "ENGINE=path to an engine with fs_opentrace" >&2; exit 2; }
	OUT=$(realpath -m "$2"); PHONE=$3
	mkdir -p "$OUT/traces"
	make_base "$OUT/base" $(phone_packs "$PHONE" "$OUT/packs")
	engine_run "$OUT/base" menu "" g_dm "$OUT/traces"
	export -f engine_run; export ENGINE SECONDS_PER_MAP
	maps_of "$PHONE" | while read -r m gt; do echo "$m $gt"; done |
		xargs -P "$JOBS" -L 1 bash -c 'engine_run "'"$OUT"'/base" "$0--$1" "$0" "$('"$(declare -f gametype_cvar)"'; gametype_cvar $1)" "'"$OUT"'/traces"'
	"$PY" -I - "$here/split-data-packs.py" "$OUT" <<'EOF'
import glob, importlib.util, sys
spec = importlib.util.spec_from_file_location("s", sys.argv[1]); s = importlib.util.module_from_spec(spec)
spec.loader.exec_module(s)
lines = s.usage_from_traces(sorted(glob.glob(sys.argv[2] + "/traces/*.tsv")))
open(sys.argv[2] + "/usage.tsv", "w").writelines(lines)
print("%d entries in %s/usage.tsv" % (len(lines), sys.argv[2]))
EOF
	exit 0
fi

SPLIT=$(realpath "$1"); PHONE=$(realpath "$2")
static_checks "$SPLIT" "$PHONE"
[ -n "${ENGINE:-}" ] || exit 0

W=${WORKDIR:-$(mktemp -d "${TMPDIR:-/tmp}/data-split.XXXXXX")}
mkdir -p "$W"
[ -n "${WORKDIR:-}" ] || trap 'rm -rf "$W"' EXIT
make_base "$W/full" $(phone_packs "$PHONE" "$W/packs")
"$PY" -I - "$SPLIT" > "$W/plan" <<'EOF'
import json, os, sys
d = sys.argv[1]; m = json.load(open(os.path.join(d, "godnotic-data.json")))
files = {r["name"]: os.path.join(d, r["file"]) for r in m["packs"]}
for mp, rec in sorted(m["maps"].items()):
    print(mp, " ".join(files[n] for n in rec["needs"]))
EOF
export -f engine_run; export ENGINE SECONDS_PER_MAP
maps_of "$PHONE" > "$W/maps"
cp "$W/maps" "$W/maps.keep"
while read -r m packs; do
	make_base "$W/b-$m" $packs
	"$PY" -I -c 'import sys, zipfile; [print(n) for p in sys.argv[1:] for n in zipfile.ZipFile(p).namelist()]' $packs > "$W/names-$m"
done < "$W/plan"
gtc=$(declare -f gametype_cvar)
while read -r m gt; do
	echo "full $m $gt"; echo "b-$m $m $gt"
done < "$W/maps" | xargs -P "$JOBS" -L 1 bash -c "$gtc"'; b=$0; m=$1; g=$(gametype_cvar $2); engine_run "'"$W"'/$b" "$b--$m" "$m" "$g" "'"$W"'/runs"' 2>/dev/null || true
fail=0
while read -r m gt; do
	full="$W/runs/full--$m"; part="$W/runs/b-$m--$m"
	# everything the whole set's match opened must be in this map's packs (names
	# only: which pack holds an entry differs by construction); a run opening
	# a little more or less on its own is noise, a name the packs lack is not
	# music is fetched after the first map and never blocks a match: not counted
	comm -23 <(cut -f2 "$full.tsv" | grep -v '^sound/cdtracks/' | LC_ALL=C sort -u) <(LC_ALL=C sort -u "$W/names-$m") \
		> "$W/missing-$m"
	pat='not found|couldn.t load|Missing|could not load|failed to load|missing'
	comm -13 <(grep -aiE "$pat" "$full.log" | grep -v 'BGM track' | sed 's/[0-9.]\+//g' | LC_ALL=C sort -u) \
		<(grep -aiE "$pat" "$part.log" | grep -v 'BGM track' | sed 's/[0-9.]\+//g' | LC_ALL=C sort -u) > "$W/extra-$m"
	missing=$(head -5 "$W/missing-$m"); extra=$(head -5 "$W/extra-$m")
	[ -z "${REPORT:-}" ] || { sed "s/^/$m missing /" "$W/missing-$m"; sed "s/^/$m console /" "$W/extra-$m"; } >> "$REPORT"
	opened=$(cut -f2 "$part.tsv" | sort -u | wc -l)
	if [ ! -s "$full.tsv" ] || [ ! -s "$part.tsv" ]; then
		echo "FAIL $m: no trace (engine did not run?)"; fail=1
	elif [ -n "$missing" ] || [ -n "$extra" ]; then
		echo "FAIL $m: core + own packs lack what the whole set gives:"; echo "$missing" "$extra" | sed 's/^/    /'; fail=1
	else
		echo "ok $m: $opened entries, same files and console as the whole set"
	fi
done < "$W/maps.keep"
exit $fail
