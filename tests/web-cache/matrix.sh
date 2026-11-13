#!/bin/bash
# The web loader's storage, download and start-up path, scenario by
# scenario, in every browser engine this host can run. See README.md here.
#
#   tests/web-cache/matrix.sh [--engines chromium,firefox] [--only NAME,...] [BUILD_DIR]
#
# BUILD_DIR is a built and staged page (scripts/build-web.sh,
# scripts/stage-web-data.sh), default build/web. Prints a table (scenario x
# engine: pass/fail, blob requests, MB fetched) and writes every visit's
# JSON to $OUT (default /tmp/xt-matrix).
#
# Browsers never reach the desktop: Chromium runs headless (GPU through
# EGL), Firefox on a private Xvfb display (its headless mode has no WebGL).
# Refuses to run with a desktop session in the environment: run it as
#   env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY tests/web-cache/matrix.sh
set -uo pipefail
if [ -n "${WAYLAND_DISPLAY:-}" ] || [ "${DISPLAY:-}" = ":0" ] || [ "${DISPLAY:-}" = ":1" ]; then
    echo "matrix.sh: unset WAYLAND_DISPLAY/DISPLAY first (env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY ...)" >&2
    exit 2
fi
here="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$here/../.." && pwd)"
ENGINES="chromium,firefox"
ONLY=""
BUILD=""
while [ $# -gt 0 ]; do
    case "$1" in
        --engines) ENGINES="$2"; shift 2 ;;
        --only) ONLY="$2"; shift 2 ;;
        *) BUILD="$1"; shift ;;
    esac
done
BUILD="$(cd "${BUILD:-$ROOT/build/web}" && pwd)"
OUT="${OUT:-/tmp/xt-matrix}"
CHROME="${CHROME:-$(ls -d "$HOME"/.cache/ms-playwright/chromium-*/chrome-linux64/chrome 2>/dev/null | sort -V | tail -1)}"
FIREFOX="${FIREFOX:-}"   # empty: the org.mozilla.firefox Flatpak
SPORT=8791 EPORT=8792 XDISP=:79
rm -rf "$OUT"; mkdir -p "$OUT/roots"

want() { [ -z "$ONLY" ] || [[ ",$ONLY," == *",$1,"* ]]; }

# ---- deploys: the same data with a new page, one changed small file, and
# a GitHub Pages-style shell on another origin.
B="$OUT/roots/b"; C="$OUT/roots/c"; E="$OUT/roots/e"
mkdir -p "$B" "$C" "$E"
cp -a "$BUILD"/. "$B"/
sed -i 's/"build":"\([^"]*\)"/"build":"\1-redeploy"/' "$B/index.html"
cp -a "$BUILD"/. "$C"/
python3 -I - "$C" <<'PY'
# One small own file changed (as a game-code push changes zz-progs.pk3):
# a new blob, its manifest entry, a new manifest id in the page.
import hashlib, json, os, re, sys
root = sys.argv[1]
html = open(os.path.join(root, 'index.html')).read()
cfg = json.loads(re.search(r'<script id="xt-config" type="application/json">(.*?)</script>', html, re.S).group(1))
for name, prof in cfg['profiles']['profiles'].items():
    p = os.path.join(root, 'data', prof['manifest'])
    m = json.load(open(p))
    f = next(f for f in m['files'] if f['path'].endswith('.cfg') or f['path'].endswith('.txt'))
    data = b'// matrix: a changed file\n' + bytes(f['size'] % 7)
    h = hashlib.sha256(data).hexdigest()
    open(os.path.join(root, 'data', 'blobs', h + '.bin'), 'wb').write(data)
    f.update(size=len(data), sha256=h, rev=h[:16], chunks=[h])
    text = json.dumps(m, sort_keys=True, separators=(',', ':')).encode()
    mid = hashlib.sha256(text).hexdigest()[:20]
    open(os.path.join(root, 'data', 'm', mid + '.json'), 'wb').write(text)
    prof['manifest'] = f'm/{mid}.json'
html = re.sub(r'(<script id="xt-config" type="application/json">)(.*?)(</script>)', lambda x: x.group(1) + json.dumps(cfg, separators=(',', ':')) + x.group(3), html, flags=re.S)
open(os.path.join(root, 'index.html'), 'w').write(html)
PY
# The shell: the page and this fork's files only; stock data from the main host.
cp -a "$BUILD"/index.html "$BUILD"/a "$E"/
cp "$BUILD"/*.png "$BUILD"/manifest.webmanifest "$E"/ 2>/dev/null || true
XT_WEB_OUT="$E" bash "$ROOT/scripts/stage-web-data.sh" --shell "http://127.0.0.1:$SPORT/data/" --out "$E" >/dev/null

# ---- servers
python3 "$here/matrix_server.py" "$BUILD" --port $SPORT >"$OUT/server.log" 2>&1 &
SPID=$!
python3 "$here/matrix_server.py" "$E" --port $EPORT >"$OUT/server-e.log" 2>&1 &
EPID=$!
XPID=""
cleanup() { kill $SPID $EPID $XPID 2>/dev/null; stop_browser; }
trap cleanup EXIT
for _ in $(seq 50); do curl -sf "http://127.0.0.1:$SPORT/__stats" >/dev/null && curl -sf "http://127.0.0.1:$EPORT/__stats" >/dev/null && break; sleep 0.2; done
ctl() { curl -sf "http://127.0.0.1:$SPORT/__ctl?$1" >/dev/null; }
URL="http://127.0.0.1:$SPORT/"
TOTAL_MB=$(python3 -I - "$BUILD" <<'PY'
import json, os, re, sys
root = sys.argv[1]
html = open(os.path.join(root, 'index.html')).read()
cfg = json.loads(re.search(r'<script id="xt-config" type="application/json">(.*?)</script>', html, re.S).group(1))
sizes = []
for prof in cfg['profiles']['profiles'].values():
    m = json.load(open(os.path.join(root, 'data', prof['manifest'])))
    sizes.append(sum(f['size'] for f in m['files'] if f.get('hosted', True)))
print(int(min(sizes) / 1e6))
PY
)

# ---- browsers
BPID=""; PROFILE=""; BPORT=0
start_browser() { # ENGINE PROFILE_DIR [private] [quota_kb] [mobile]
    local engine="$1" prof="$2" private="${3:-}" quota="${4:-}" mobile="${5:-}"
    PROFILE="$prof"; mkdir -p "$prof"
    BPORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')"
    if [ "$engine" = chromium ]; then
        local extra=()
        [ -n "$private" ] && extra+=(--incognito)
        setsid "$CHROME" --headless=new --use-angle=gl-egl --enable-gpu --ignore-gpu-blocklist \
            --disable-breakpad --disable-crash-reporter --no-first-run --no-default-browser-check \
            --autoplay-policy=no-user-gesture-required --window-size=1280,720 \
            --user-data-dir="$prof" --remote-debugging-port="$BPORT" "${extra[@]}" about:blank \
            >>"$prof.log" 2>&1 &
        BPID=$!
    else
        if [ -z "$XPID" ]; then
            Xvfb $XDISP -screen 0 1280x800x24 -nolisten tcp >"$OUT/xvfb.log" 2>&1 &
            XPID=$!
            sleep 1
        fi
        {
            echo 'user_pref("webgl.force-enabled", true);'
            echo 'user_pref("browser.shell.checkDefaultBrowser", false);'
            echo 'user_pref("datareporting.policy.dataSubmissionEnabled", false);'
            echo 'user_pref("browser.aboutwelcome.enabled", false);'
            echo 'user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);'
            [ -n "$private" ] && echo 'user_pref("browser.privatebrowsing.autostart", true);'
            [ -n "$quota" ] && echo "user_pref(\"dom.quotaManager.temporaryStorage.fixedLimit\", $quota);"
            [ -n "$mobile" ] && echo 'user_pref("general.useragent.override", "Mozilla/5.0 (Android 16; Mobile; rv:146.0) Gecko/146.0 Firefox/146.0");' && echo 'user_pref("dom.w3c_touch_events.enabled", 1);'
        } > "$prof/user.js"
        if [ -n "$FIREFOX" ]; then
            setsid env DISPLAY=$XDISP MOZ_ENABLE_WAYLAND=0 "$FIREFOX" --no-remote --profile "$prof" \
                --remote-debugging-port "$BPORT" >>"$prof.log" 2>&1 &
        else
            setsid env DISPLAY=$XDISP MOZ_ENABLE_WAYLAND=0 flatpak run --env=DISPLAY=$XDISP --env=MOZ_ENABLE_WAYLAND=0 \
                --unset-env=WAYLAND_DISPLAY --nosocket=wayland --socket=x11 --filesystem="$prof" \
                org.mozilla.firefox --no-remote --profile "$prof" --remote-debugging-port "$BPORT" >>"$prof.log" 2>&1 &
        fi
        BPID=$!
        sleep 6
    fi
}
stop_browser() { # [KILL]
    [ -n "$BPID" ] || return 0
    local sig="${1:-TERM}"
    # The whole session (setsid): the browser and its child processes.
    kill -s "$sig" -- "-$BPID" 2>/dev/null || kill -s "$sig" "$BPID" 2>/dev/null
    for p in $(ps -eo pid=,args= | awk -v d="$PROFILE" 'index($0, d) {print $1}'); do kill -s "$sig" "$p" 2>/dev/null; done
    sleep 2
    for p in $(ps -eo pid=,args= | awk -v d="$PROFILE" 'index($0, d) {print $1}'); do kill -9 "$p" 2>/dev/null; done
    BPID=""
    sleep 1
}
visit() { # NAME ENGINE [matrix.mjs args...] -> $OUT/NAME-ENGINE.json, returns its exit code
    local name="$1" engine="$2"; shift 2
    node "$here/matrix.mjs" --engine "$engine" --port "$BPORT" --server "http://127.0.0.1:$SPORT" "$@" \
        > "$OUT/$name-$engine.json" 2>>"$OUT/$name-$engine.err"
}
field() { python3 -c "import json,sys; d=json.loads(open(sys.argv[1]).read().strip().splitlines()[-1]); print(d.get(sys.argv[2], ''))" "$1" "$2" 2>/dev/null; }
RESULTS="$OUT/results.tsv"
: > "$RESULTS"
record() { # NAME ENGINE PASS NOTE (the last visit's JSON gives requests and MB)
    local f="$OUT/$1-$2.json" req mb
    req="$(field "$f" blobRequests)"; mb="$(field "$f" blobBytes)"
    mb=$(( ${mb:-0} / 1000000 ))
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "${req:-?}" "$mb" "$4" >> "$RESULTS"
    printf '%-14s %-9s %-5s req=%-4s %5s MB  %s\n' "$1" "$2" "$3" "${req:-?}" "$mb" "$4"
}
started() { [ -n "$(field "$1" started)" ]; }

IFS=',' read -r -a engines <<< "$ENGINES"
for E_ in "${engines[@]}"; do
    P="$OUT/p-$E_"
    ctl "root=$BUILD&fault=none"
    # 1-4: first visit, reload, new tab, browser restart
    start_browser "$E_" "$P/main"
    visit first "$E_" --url "$URL" --click
    f="$OUT/first-$E_.json"; started "$f" && record first "$E_" pass "store=$(field "$f" store) profile=$(field "$f" profile) lazy=$(field "$f" lazy)" || record first "$E_" FAIL "$(field "$f" error)"
    visit reload "$E_" --url "$URL"
    f="$OUT/reload-$E_.json"; started "$f" && [ "$(field "$f" blobRequests)" = 0 ] && record reload "$E_" pass "" || record reload "$E_" FAIL "asked=$(field "$f" asked) $(field "$f" error)"
    visit newtab "$E_" --url "$URL" --new-tab
    f="$OUT/newtab-$E_.json"; started "$f" && [ "$(field "$f" blobRequests)" = 0 ] && record newtab "$E_" pass "" || record newtab "$E_" FAIL "$(field "$f" error)"
    stop_browser; start_browser "$E_" "$P/main"
    visit restart "$E_" --url "$URL"
    f="$OUT/restart-$E_.json"; started "$f" && [ "$(field "$f" blobRequests)" = 0 ] && record restart "$E_" pass "" || record restart "$E_" FAIL "$(field "$f" error)"
    # 8, 9: deploys
    ctl "root=$B"
    visit deploy-same "$E_" --url "$URL"
    f="$OUT/deploy-same-$E_.json"; started "$f" && [ "$(field "$f" blobRequests)" = 0 ] && record deploy-same "$E_" pass "new page, same data" || record deploy-same "$E_" FAIL "$(field "$f" error)"
    ctl "root=$C"
    visit deploy-code "$E_" --url "$URL"
    f="$OUT/deploy-code-$E_.json"; started "$f" && [ "$(field "$f" blobRequests)" -le 2 ] && record deploy-code "$E_" pass "one changed file fetched, no question" || record deploy-code "$E_" FAIL "$(field "$f" error) shown=$(field "$f" shown)"
    ctl "root=$BUILD"
    # 6: killed while the engine starts, then a normal start
    visit kill-start "$E_" --url "$URL" --kill-at-start
    stop_browser KILL; start_browser "$E_" "$P/main"
    visit kill-start "$E_" --url "$URL"
    f="$OUT/kill-start-$E_.json"; started "$f" && [ "$(field "$f" blobRequests)" = 0 ] && record kill-start "$E_" pass "$(field "$f" problem | cut -c1-70)" || record kill-start "$E_" FAIL "$(field "$f" error)"
    # 7: offline start (no service worker: the page itself cannot load)
    ctl "fault=offline"
    visit offline "$E_" --url "$URL" --timeout 20
    ctl "fault=none"
    f="$OUT/offline-$E_.json"; started "$f" && record offline "$E_" pass "" || record offline "$E_" FAIL "expected: no service worker, so no page offline"
    # 16: a slow device (Chromium only: CPU throttling)
    if [ "$E_" = chromium ]; then
        visit slow-cpu "$E_" --url "$URL" --cpu 4
        f="$OUT/slow-cpu-$E_.json"; started "$f" && record slow-cpu "$E_" pass "warm start to menu $(field "$f" started) ms at 4x CPU" || record slow-cpu "$E_" FAIL "$(field "$f" error)"
    fi
    # 11: storage cleared by the user
    stop_browser
    if [ "$E_" = chromium ]; then rm -rf "$P/main/Default/File System" "$P/main/Default/Service Worker" "$P/main/Default/Local Storage"
    else rm -rf "$P/main/storage/default/http+++127.0.0.1+$SPORT"; fi
    start_browser "$E_" "$P/main"
    visit cleared "$E_" --url "$URL" --click
    f="$OUT/cleared-$E_.json"; started "$f" && record cleared "$E_" pass "downloaded again: $(field "$f" reason | cut -c1-60)" || record cleared "$E_" FAIL "$(field "$f" error)"
    # 18: the Pages-style shell on another origin, then back
    visit shell "$E_" --url "http://127.0.0.1:$EPORT/" --click
    f="$OUT/shell-$E_.json"; started "$f" && record shell "$E_" pass "own storage; stock data from the main host" || record shell "$E_" FAIL "$(field "$f" error)"
    visit back "$E_" --url "$URL"
    f="$OUT/back-$E_.json"; started "$f" && [ "$(field "$f" blobRequests)" = 0 ] && record back "$E_" pass "" || record back "$E_" FAIL "$(field "$f" error)"
    stop_browser
    # 5: killed mid-download three times, then resumed
    start_browser "$E_" "$P/kills"
    sum=0
    for frac in 15 40 70; do
        visit "kill-$frac" "$E_" --url "$URL" --click --kill-at $(( TOTAL_MB * 1000000 * frac / 100 / 3 ))
        sum=$(( sum + $(field "$OUT/kill-$frac-$E_.json" blobBytes || echo 0) ))
        stop_browser KILL; start_browser "$E_" "$P/kills"
    done
    visit resume "$E_" --url "$URL" --click
    f="$OUT/resume-$E_.json"; sum=$(( sum + $(field "$f" blobBytes) ))
    started "$f" && record resume "$E_" pass "3 kills; $(( sum / 1000000 )) MB fetched in all for $TOTAL_MB MB; $(field "$f" reason | cut -c1-50)" || record resume "$E_" FAIL "$(field "$f" error)"
    stop_browser
    # 14: two tabs at once
    start_browser "$E_" "$P/two"
    visit two-tabs "$E_" --url "$URL" --click --two-tabs
    f="$OUT/two-tabs-$E_.json"; started "$f" && record two-tabs "$E_" pass "one download for both" || record two-tabs "$E_" FAIL "$(field "$f" error)"
    stop_browser
    # 15: a flaky network
    start_browser "$E_" "$P/flaky"
    ctl "fault=5xx:0.3"
    visit flaky-5xx "$E_" --url "$URL" --click --kill-at $(( TOTAL_MB * 1000000 / 2 ))
    ctl "fault=drop:0.3"
    visit flaky-drop "$E_" --url "$URL" --click
    ctl "fault=none"
    f="$OUT/flaky-drop-$E_.json"; started "$f" && record flaky "$E_" pass "503s, then cut-off bodies" || record flaky "$E_" FAIL "$(field "$f" error)"
    stop_browser
    # 12: quota too small
    start_browser "$E_" "$P/quota" "" 200000
    if [ "$E_" = chromium ]; then visit quota "$E_" --url "$URL" --click --quota 200000000; else visit quota "$E_" --url "$URL" --click; fi
    f="$OUT/quota-$E_.json"; e="$(field "$f" error)$(field "$f" problem)"
    [[ "$e" == *"space"* ]] && record quota "$E_" pass "says: $(echo "$e" | cut -c1-70)" || record quota "$E_" FAIL "$e"
    stop_browser
    # 13: a private window
    start_browser "$E_" "$P/private" private
    visit private "$E_" --url "$URL" --click
    f="$OUT/private-$E_.json"; started "$f" && record private "$E_" pass "store=$(field "$f" store)" || record private "$E_" FAIL "$(field "$f" error)"
    stop_browser
    # 17: a phone (mobile UA, touch, small screen)
    start_browser "$E_" "$P/mobile" "" "" mobile
    visit mobile "$E_" --url "$URL" --click --mobile
    f="$OUT/mobile-$E_.json"; started "$f" && record mobile "$E_" pass "profile=$(field "$f" profile)" || record mobile "$E_" FAIL "$(field "$f" error)"
    stop_browser
    rm -rf "$P"
done

echo
echo "Results ($RESULTS): scenario, engine, result, blob requests, MB fetched, note"
column -t -s $'\t' "$RESULTS"
! grep -q $'\tFAIL\t' <(grep -v '^offline' "$RESULTS")
