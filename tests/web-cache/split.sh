#!/bin/bash
# The split data set (progressive download) in real browsers: web/shell.js,
# web/store.js and web/lazyfs.js as they are, against a local server with a
# small made-up split set (tests/web-cache/make-split-set.py, staged by
# scripts/stage-web-data.sh) and a stand-in for the engine
# (tests/web-cache/stub-engine.js), which a made-up set could not start.
#
#   env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY tests/web-cache/split.sh [--engines chromium,firefox]
#
# What it checks (one line per step; exit 1 on any failure):
#   staged          the staged core has every .mapinfo and no .bsp, and the
#                   split made by scripts/split-web-maps.py was not used
#   dialog          the first-visit question names the first-play size and
#                   says the rest follows while playing
#   first-play      the engine starts when the first-play files are stored:
#                   exactly those were requested before it started
#   background      the rest arrives while the engine runs: each pack is
#                   announced with fs_mountpack, in priority order (a map's
#                   own pack after the packs it needs), every file of every
#                   pack reads back right through the engine's search path,
#                   and the engine hears "touch_data_status" at most about
#                   twice a second, ending with "state complete"
#   warm            a reload: no data request, every pack found at start
#   resume          the browser killed mid-download: the next visit starts
#                   with what was stored (no request before the start) and
#                   fetches only the rest
#   needmap         "map beta" while the background is busy with the music:
#                   beta's packs come first, are mounted with fs_mountpack
#                   (own pack last), the command runs again and the map
#                   loads; Module.xt.dataWant("gamma") fetches and mounts
#                   gamma's pack without a command
#   old-layout      a browser holding the layout from before the split set
#                   (official pack names, the stager's own map split; it
#                   goes through the same queue, with fs_rescan): the
#                   new deploy starts with no question and no request,
#                   every map still loads, the new set is stored in the
#                   background, and the next start runs it
#
# Browsers run headless and never reach the desktop; everything started here
# is killed at exit.
set -uo pipefail
if [ -n "${WAYLAND_DISPLAY:-}" ] || [ "${DISPLAY:-}" = ":0" ] || [ "${DISPLAY:-}" = ":1" ]; then
    echo "split.sh: WAYLAND_DISPLAY/DISPLAY point at a desktop session; run under env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY" >&2
    exit 2
fi
here="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$here/../.." && pwd)"
ENGINES="chromium,firefox"
while [ $# -gt 0 ]; do
    case "$1" in
        --engines) ENGINES="$2"; shift 2 ;;
        *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
done
OUT="${OUT:-$(mktemp -d "${TMPDIR:-/tmp}/xt-split.XXXXXX")}"
mkdir -p "$OUT"
CHROME="${CHROME:-$(ls -d "$HOME"/.cache/ms-playwright/chromium-*/chrome-linux64/chrome 2>/dev/null | sort -V | tail -1)}"
FIREFOX="${FIREFOX:-}"   # empty: the org.mozilla.firefox Flatpak
freeport() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }

# ---- the two sites: the split set, and the layout before it
python3 -I "$here/make-split-set.py" "$OUT/set" --old "$OUT/oldset" || exit 1
mkdir -p "$OUT/slim/xonotic-data.pk3dir"
for f in progs.dat csprogs.dat menu.dat default.cfg; do printf 'test %s\n' "$f" > "$OUT/slim/xonotic-data.pk3dir/$f"; done
printf '// test\n' > "$OUT/slim/xonotic.cfg"
site() { # DIR stage-web-data arguments...
    local dir="$1"
    shift
    mkdir -p "$dir"
    # (Everything the page loads: its scripts, the style sheet, the banner.)
    cp "$ROOT"/web/index.html "$ROOT"/web/*.js "$ROOT"/web/*.css "$ROOT"/web/manifest.webmanifest "$dir/"
    cp "$ROOT"/web/godnotic-banner.* "$dir/" 2>/dev/null || true
    cp "$here/stub-engine.js" "$dir/godnotic.js"
    cp "$OUT/set/expect.json" "$dir/"
    TMPDIR="$OUT" bash "$ROOT/scripts/stage-web-data.sh" "$@" --slim "$OUT/slim" --out "$dir" >"$dir.stage.log" 2>&1 || { cat "$dir.stage.log" >&2; exit 1; }
}
site "$OUT/new" --web "$OUT/set"
site "$OUT/old" --stock "$OUT/oldset"

SPORT="$(freeport)"
python3 "$here/matrix_server.py" "$OUT/new" --port "$SPORT" >"$OUT/serve.log" 2>&1 &
SPID=$!
BPID=""
PROFILE=""
stop_browser() { # [SIGNAL]: the browser and everything running on its profile
    [ -n "$PROFILE" ] || return 0
    local sig="${1:-KILL}"
    [ -z "$BPID" ] || kill -s "$sig" -- "-$BPID" 2>/dev/null || kill -s "$sig" "$BPID" 2>/dev/null
    for p in $(ps -eo pid=,args= | awk -v d="$PROFILE" 'index($0, d) {print $1}'); do kill -s "$sig" "$p" 2>/dev/null; done
    sleep 1
    for p in $(ps -eo pid=,args= | awk -v d="$PROFILE" 'index($0, d) {print $1}'); do kill -9 "$p" 2>/dev/null; done
    BPID=""
}
cleanup() { stop_browser; kill $SPID 2>/dev/null; [ -n "${KEEP:-}" ] || rm -rf "$OUT"; }
trap cleanup EXIT
start_browser() { # ENGINE PROFILE_DIR
    PROFILE="$2"
    BPORT="$(freeport)"
    mkdir -p "$PROFILE"
    if [ "$1" = chromium ]; then
        setsid "$CHROME" --headless=new --use-angle=gl-egl --enable-gpu --ignore-gpu-blocklist \
            --disable-breakpad --disable-crash-reporter --no-first-run --no-default-browser-check \
            --autoplay-policy=no-user-gesture-required --window-size=1280,720 \
            --user-data-dir="$PROFILE" --remote-debugging-port="$BPORT" about:blank >>"$PROFILE.log" 2>&1 &
    elif [ -n "$FIREFOX" ]; then
        setsid env MOZ_HEADLESS=1 "$FIREFOX" --headless --no-remote --profile "$PROFILE" \
            --remote-debugging-port "$BPORT" >>"$PROFILE.log" 2>&1 &
    else
        setsid env MOZ_HEADLESS=1 flatpak run --env=MOZ_HEADLESS=1 --unset-env=WAYLAND_DISPLAY --unset-env=DISPLAY \
            --nosocket=wayland --nosocket=x11 --filesystem="$PROFILE" \
            org.mozilla.firefox --headless --no-remote --profile "$PROFILE" --remote-debugging-port "$BPORT" >>"$PROFILE.log" 2>&1 &
    fi
    BPID=$!
    disown "$BPID" 2>/dev/null || true
}
FAILED=0
PASSED=0
result() { # ok|FAIL ENGINE LABEL TEXT
    if [ "$1" = ok ]; then PASSED=$((PASSED + 1)); else FAILED=$((FAILED + 1)); fi
    printf '%-4s %-9s %-24s %s\n' "$1" "$2" "$3" "$(printf '%s' "$4" | cut -c1-"${5:-260}")"
}
step() { # LABEL ENGINE step.mjs options...
    local label="$1" engine="$2"
    shift 2
    local line
    line="$(node "$here/step.mjs" --engine "$engine" --port "$BPORT" --label "$label" --init "$here/split-init.js" --timeout 90 "$@" 2>&1 | tail -1)"
    if printf '%s' "$line" | grep -q '"ok":true'; then result ok "$engine" "$label" "$line"; else result FAIL "$engine" "$label" "$line" 3000; fi
}
ctl() { curl -s "http://127.0.0.1:$SPORT/__ctl?$1" >/dev/null; }
SITE="http://127.0.0.1:$SPORT/"
DONE='r.verify.bad.length === 0 && r.verify.files === r.allFiles && r.errors.length === 0 && r.lazyErrors === 0 && r.badStatus.length === 0 && / state complete /.test(r.last)'

# ---- staged: no stand-in .bsp, the split set's own packs
if out="$(python3 -I - "$OUT/new/data" <<'PY' 2>&1
import io, json, os, sys, zipfile
d = sys.argv[1]
p = json.load(open(os.path.join(d, 'profiles.json')))
m = json.load(open(os.path.join(d, p['profiles']['web']['manifest'])))
assert m['format'] == 5 and m['set'] and m['firstPlay'] and m['maps'], 'a split-set manifest'
names = [f['name'] for f in m['files']]
assert not [n for n in names if '-0core' in n or '-0extra' in n], 'no pack of the map split made by split-web-maps.py'
core = [f for f in m['files'] if f.get('kind') == 'core']
assert len(core) == 1
z = zipfile.ZipFile(io.BytesIO(b''.join(open(os.path.join(d, 'blobs', h + '.bin'), 'rb').read() for h in core[0]['chunks'])))
bsp = [n for n in z.namelist() if n.endswith('.bsp')]
info = [n for n in z.namelist() if n.endswith('.mapinfo')]
assert not bsp, f'stand-in .bsp in the staged core: {bsp}'
assert len(info) == len(m['maps']) == 3, info
for name, need in m['maps'].items():
    own = [f for f in m['files'] if f['name'] == need[-1]][0]
    assert own.get('map') == name and own['group'] == 'map', (name, own)
print(f"core {core[0]['name']}: {len(info)} mapinfo, 0 bsp; {len(names)} files, first play {m['firstPlay']}")
PY
)"; then result ok stager staged "$out"; else result FAIL stager staged "$out" 3000; fi

for E in ${ENGINES//,/ }; do
    case "$E" in chromium|firefox) ;; *) continue ;; esac
    ctl "root=$OUT/new"

    # ---- a first visit, the background, a reload
    P="$OUT/p-first-$E"
    ctl rate=150000
    start_browser "$E" "$P"
    step dialog "$E" --url "$SITE" --until '!document.getElementById("xt-download").hidden' --eval 'splitTest.dialog()' \
        --expect 'r.title === "Download Godnotic" && /^\d+ MB to play\. Maps and music \(\d+ MB\) follow as you play\.$/.test(r.lead)'
    step first-play "$E" --no-navigate --until '(document.getElementById("xt-dl-go").click(), window.__engine && __engine.started)' --eval 'splitTest.started()' \
        --expect 'r.beforeIsFirstPlay && r.packsAreFirstPlay && r.lazy === true && r.store === "opfs" && r.coreBsp.length === 0 && r.coreMapinfo === 3 && / state fetching /.test(r.status0)'
    step background "$E" --no-navigate --until 'splitTest.complete()' --eval 'splitTest.now()' \
        --expect "r.restInOrder && r.reads > 0 && r.perSecond <= 4 && r.commands.length === 0 && $DONE"
    step warm "$E" --url "$SITE" --until 'window.__engine && __engine.started' --eval 'splitTest.started().then(async (s) => Object.assign(s, { now: await splitTest.now() }))' \
        --expect 'r.before.length === 0 && r.hasAll && !r.dialogShown && r.now.fetched.length === 0 && r.now.mountpacks.length === 0 && r.now.verify.bad.length === 0 && / state complete /.test(r.status0)'
    stop_browser KILL

    # ---- killed mid-download
    P="$OUT/p-resume-$E"
    ctl rate=100000
    start_browser "$E" "$P"
    step resume-kill "$E" --url "$SITE?autodownload" --until 'window.__engine && __engine.mountpacks.length >= 2 && !splitTest.complete()' --eval 'splitTest.now()' \
        --expect 'r.mountpacks.length >= 2 && r.mountpacks.length < r.rest.length'
    stop_browser KILL; start_browser "$E" "$P"
    step resume-start "$E" --url "$SITE" --until 'window.__engine && __engine.started' --eval 'splitTest.started()' \
        --expect 'r.before.length === 0 && !r.dialogShown && r.startPacks.includes("godnotic-t-music.pk3") && r.startPacks.includes("godnotic-t-shared-2.pk3") && !r.hasAll'
    step resume-rest "$E" --no-navigate --until 'splitTest.complete()' --eval 'splitTest.now()' \
        --expect "r.mountpacks.length >= 1 && r.mountpacks.length <= r.rest.length - 2 && !r.fetched.includes('godnotic-t-music.pk3') && !r.fetched.includes('godnotic-t-core.pk3') && $DONE"
    stop_browser KILL

    # ---- a map that is not here yet
    P="$OUT/p-needmap-$E"
    # (A slow line: the background is in the music, 600 KB, for a quarter
    # of a minute.)
    ctl rate=40000
    start_browser "$E" "$P"
    step needmap-start "$E" --url "$SITE?autodownload" --until 'window.__engine && __engine.started' \
        --eval '(xt.command("map \"beta\""), 1)' --expect 'r === 1'
    step needmap "$E" --no-navigate --until '__engine.loaded.includes("beta")' --eval 'splitTest.now()' \
        --expect 'r.errors.length === 0 && r.mountpacks.join() === "godnotic-t-shared-2.pk3,godnotic-t-map-beta.pk3" && r.commands.join("|") === "map \"beta\"|map \"beta\"" && r.wants.join() === "beta" && !/ want /.test(r.last) && !r.card'
    step datawant "$E" --no-navigate --until '(window.__w = window.__w || Module.xt.dataWant("gamma"), __engine.mountpacks.includes("godnotic-t-map-gamma.pk3"))' --eval 'splitTest.now()' \
        --expect 'r.errors.length === 0 && r.mountpacks.join() === "godnotic-t-shared-2.pk3,godnotic-t-map-beta.pk3,godnotic-t-map-gamma.pk3" && r.commands.length === 2 && r.wants.join() === "beta,gamma" && r.loaded.join() === "beta"'
    step needmap-rest "$E" --no-navigate --until '(window.__r = window.__r || fetch("/__ctl?rate=0"), splitTest.complete())' --eval '(xt.command("map \"gamma\""), new Promise((r) => setTimeout(r, 1500)).then(() => splitTest.now()))' \
        --expect "r.loaded.join() === 'beta,gamma' && r.mountpacks.length === r.rest.length && $DONE"
    stop_browser KILL

    # ---- a browser that holds the layout from before the split set
    P="$OUT/p-old-$E"
    ctl "root=$OUT/old"
    start_browser "$E" "$P"
    # (That layout starts on its core packs too; its maps and music follow,
    # and the engine sees them with the fs_rescan before the next map.)
    step old-first "$E" --url "$SITE?autodownload" --until 'splitTest.complete()' \
        --eval '(xt.command("map \"beta\""), new Promise((r) => setTimeout(r, 1500)).then(() => splitTest.started().then((s) => Object.assign(s, { loaded: __engine.loaded, errors: __engine.errors }))))' \
        --expect 'r.oldLayout && r.loaded.join() === "beta" && r.errors.length === 0 && / state fetching /.test(r.status0)'
    ctl "root=$OUT/new"
    step old-keeps-playing "$E" --url "$SITE?small=1000" --until 'window.__engine && __engine.started' \
        --eval '(xt.command("map \"alpha\""), new Promise((r) => setTimeout(r, 1500)).then(() => splitTest.started().then((s) => Object.assign(s, { loaded: __engine.loaded, errors: __engine.errors }))))' \
        --expect 'r.oldLayout && !r.dialogShown && r.before.length === 0 && r.loaded.join() === "alpha" && r.errors.length === 0 && / state fetching /.test(r.status0)'
    step old-new-arrives "$E" --no-navigate --until 'splitTest.complete()' --eval 'splitTest.now()' \
        --expect 'r.mountpacks.length === 0 && r.errors.length === 0 && r.fetched.includes("godnotic-t-core.pk3") && r.fetched.includes("godnotic-t-extras.pk3") && r.fetched.includes("godnotic-t-map-gamma.pk3")'
    step old-next-start "$E" --url "$SITE?small=1000" --until 'window.__engine && __engine.started' --eval 'splitTest.started().then(async (s) => Object.assign(s, { now: await splitTest.now() }))' \
        --expect 'r.before.length === 0 && r.hasAll && !r.oldLayout && !r.dialogShown && r.now.fetched.length === 0 && r.now.verify.bad.length === 0 && r.now.verify.files === r.now.allFiles'
    stop_browser KILL
done

echo "split set in browsers: $PASSED passed, $FAILED failed"
[ "$FAILED" = 0 ]
