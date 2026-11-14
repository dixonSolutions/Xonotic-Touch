#!/bin/bash
# The game data store in real browsers, with the browser restarted (killed)
# between steps: what is written must still be there, whole, afterwards.
#
#   env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY tests/web-cache/browser.sh \
#       [--engines chromium,firefox] [--site BUILD_DIR [--install DIR]]
#
# Part 1 (every engine; no built page needed): tests/web-cache/storage.html,
# which runs web/store.js against the browser's own origin private file
# system: a download cut off and resumed, a restart, the worker writer
# (createSyncAccessHandle, the path of browsers whose OPFS has no
# createWritable), an evicted chunk, a damaged chunk, persist(); and
# lazyfs.html (reads in place).
#
# Part 2 (Chromium, with --site: a built page with data, see README.md): the
# page itself. First visit into OPFS without persist(), a restart with no
# data requests, a new deploy with the same packs, one chunk evicted, one
# damaged, the persist switch (denied, then granted through CDP), the
# installed-app offer (a mocked getInstalledRelatedApps), the worker writer,
# and, with --install DIR (a folder holding stock pk3s under their stock
# names, as an installed Godnotic keeps them), "Use an existing Godnotic
# folder" with showDirectoryPicker() answered by a directory handle the test
# fills (an OPFS directory: the same interface, no picker window).
#
# Browsers run headless and never reach the desktop; everything started here
# is killed at exit. Results: one line per step, then a summary; exit 1 on
# any failure.
set -uo pipefail
if [ -n "${WAYLAND_DISPLAY:-}" ] || [ "${DISPLAY:-}" = ":0" ] || [ "${DISPLAY:-}" = ":1" ]; then
    echo "browser.sh: WAYLAND_DISPLAY/DISPLAY point at a desktop session; run under env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY" >&2
    exit 2
fi
here="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$here/../.." && pwd)"
ENGINES="chromium,firefox"
SITE=""
INSTALL=""
while [ $# -gt 0 ]; do
    case "$1" in
        --engines) ENGINES="$2"; shift 2 ;;
        --site) SITE="$(cd "$2" && pwd)"; shift 2 ;;
        --install) INSTALL="$(cd "$2" && pwd)"; shift 2 ;;
        *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
done
OUT="${OUT:-$(mktemp -d "${TMPDIR:-/tmp}/xt-browser.XXXXXX")}"
mkdir -p "$OUT"
CHROME="${CHROME:-$(ls -d "$HOME"/.cache/ms-playwright/chromium-*/chrome-linux64/chrome 2>/dev/null | sort -V | tail -1)}"
FIREFOX="${FIREFOX:-}"   # empty: the org.mozilla.firefox Flatpak
freeport() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }

SPORT="$(freeport)"
python3 "$here/matrix_server.py" "$ROOT" --port "$SPORT" >"$OUT/serve.log" 2>&1 &
SPID=$!
MPID=""
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
cleanup() { stop_browser; kill $SPID $MPID 2>/dev/null; [ -n "${KEEP:-}" ] || rm -rf "$OUT"/p-* "$OUT/site2"; }
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
step() { # LABEL ENGINE step.mjs options...
    local label="$1" engine="$2"
    shift 2
    local line
    line="$(node "$here/step.mjs" --engine "$engine" --port "$BPORT" --label "$label" "$@" 2>&1 | tail -1)"
    if printf '%s' "$line" | grep -q '"ok":true'; then
        PASSED=$((PASSED + 1)); printf 'ok   %-9s %-28s %s\n' "$engine" "$label" "$(printf '%s' "$line" | cut -c1-230)"
    else
        FAILED=$((FAILED + 1)); printf 'FAIL %-9s %-28s %s\n' "$engine" "$label" "$line"
    fi
}

# ------------------------------------------------------------ part 1: store
STORE="http://127.0.0.1:$SPORT/tests/web-cache/storage.html"
for E in ${ENGINES//,/ }; do
    case "$E" in chromium|firefox) ;; *) continue ;; esac   # --engines none: part 2 only
    P="$OUT/p-store-$E"
    start_browser "$E" "$P"
    step lazy-reads "$E" --url "http://127.0.0.1:$SPORT/tests/web-cache/lazyfs.html" \
        --eval 'lazyfsTest()' --expect 'r.ok && r.eio && r.checked > 1e6'
    # A download into OPFS, cut off 5 MiB into its first chunk and resumed.
    step write-opfs "$E" --url "$STORE" --eval 'storeTest.write(1)' \
        --expect 'r.kind === "opfs" && r.writer === "main" && r.aborted === "AbortError" && r.resumable === 4194304 && r.resumedFrom === 4194304 && r.stored && r.piecesLeft === 0 && r.persisted === false'
    stop_browser KILL; start_browser "$E" "$P"
    step restart-has-it "$E" --url "$STORE" --eval 'storeTest.check(1)' \
        --expect 'r.kind === "opfs" && r.missing.length === 0 && r.bad === 0 && r.checked === 3 && r.lazy === true && r.persisted === false'
    # The same through a worker (createSyncAccessHandle).
    step write-worker "$E" --url "$STORE" --eval 'storeTest.write(2, "worker")' \
        --expect 'r.kind === "opfs" && r.writer === "worker" && r.resumedFrom === 4194304 && r.stored && r.piecesLeft === 0'
    stop_browser KILL; start_browser "$E" "$P"
    step restart-has-worker "$E" --url "$STORE" --eval 'storeTest.check(2)' \
        --expect 'r.missing.length === 0 && r.bad === 0 && r.checked === 3'
    # One chunk evicted, one damaged: only those are fetched again.
    step evict "$E" --url "$STORE" --eval 'storeTest.evict(1, 1)' --expect 'r.removed'
    step corrupt "$E" --url "$STORE" --eval 'storeTest.corrupt(1, 2)' --expect 'r.sameSize'
    step check-finds-both "$E" --url "$STORE" --eval 'storeTest.check(1)' \
        --expect 'JSON.stringify(r.missing) === "[1]" && r.bad === 1'
    step repair-only-those "$E" --url "$STORE" --eval 'storeTest.repair(1)' \
        --expect 'JSON.stringify(r.fetched) === "[1,2]" && r.left === 0'
    stop_browser KILL; start_browser "$E" "$P"
    step restart-after-repair "$E" --url "$STORE" --eval 'storeTest.check(1)' \
        --expect 'r.missing.length === 0 && r.bad === 0'
    # persist(): whatever the browser answers, persisted() must agree.
    step persist-answer "$E" --url "$STORE" --eval 'storeTest.persist()' \
        --expect 'r.before === false && (r.granted === true) === (r.after === true)'
    stop_browser KILL
done

# ------------------------------------------------------------- part 2: page
if [ -n "$SITE" ]; then
    E=chromium
    MPORT="$(freeport)"
    python3 "$here/matrix_server.py" "$SITE" --port "$MPORT" >"$OUT/matrix-server.log" 2>&1 &
    MPID=$!
    M="http://127.0.0.1:$MPORT"
    URL="$M/?autodownload"
    reset() { curl -sf "$M/__ctl?reset=1" >/dev/null; }
    blobs() { curl -sf "$M/__stats" | python3 -c 'import json,sys; print(json.load(sys.stdin)["blobRequests"])'; }
    expect_blobs() { # LABEL TEST...: a check on the number of blob requests since reset
        local label="$1" n; n="$(blobs)"; shift
        if [ "$n" "$@" ]; then PASSED=$((PASSED + 1)); printf 'ok   %-9s %-28s %s blob requests\n' "$E" "$label" "$n"
        else FAILED=$((FAILED + 1)); printf 'FAIL %-9s %-28s %s blob requests (wanted %s)\n' "$E" "$label" "$n" "$*"; fi
    }
    # persist() calls are counted by a script that runs before the page's.
    cat >"$OUT/count-persist.js" <<'JS'
window.__persistCalls = 0;
{ const real = navigator.storage.persist.bind(navigator.storage); navigator.storage.persist = () => { window.__persistCalls++; return real(); }; }
JS
    STARTED='(document.getElementById("xt-error").hidden === false && !document.getElementById("xt-overlay").classList.contains("xt-gone")) ? "FAIL " + document.getElementById("xt-error-text").textContent : (xt.state.started && xt.state.timing["first-frame"] && xt.state.logLines > 20)'
    STATE='(async () => ({store: xt.state.store, profile: xt.state.profile, problem: xt.state.problem || "", persistCalls: window.__persistCalls, persisted: await navigator.storage.persisted(), panels: ["xt-download","xt-progress"].filter((p) => !document.getElementById(p).hidden), lazy: xt.state.lazy, folder: xt.state.folder ? xt.state.folder.files.map((f) => f.name) : null, mounts: xt.mounts.size}))()'
    OPFS_DIR='(await (await navigator.storage.getDirectory()).getDirectoryHandle("xt-data-v1"))'
    # The first chunk of a stored map pack: something big enough to matter.
    VICTIM='(xt.state.files.find((f) => f.group === "map").chunks[0])'

    P="$OUT/p-page"
    start_browser "$E" "$P"; reset
    step first-visit "$E" --init "$OUT/count-persist.js" --url "$URL" --until "$STARTED" --eval "$STATE" --timeout 900 \
        --expect 'r.store === "opfs" && r.persistCalls === 0 && r.persisted === false && r.lazy === true'
    expect_blobs first-visit-downloads -gt 10
    # A setting changed in the game is in IndexedDB a few seconds later.
    step set-a-setting "$E" --no-navigate --eval '(async () => { xt.command("seta _cl_name WebTestName"); await new Promise((r) => setTimeout(r, 6000)); return true; })()' --expect 'r === true'
    stop_browser KILL; start_browser "$E" "$P"; reset
    step restart "$E" --init "$OUT/count-persist.js" --url "$M/" --until "$STARTED" --eval "$STATE" \
        --expect 'r.store === "opfs" && r.persistCalls === 0 && r.panels.length === 0'
    expect_blobs restart-no-requests -eq 0
    step setting-kept "$E" --no-navigate --eval '(async () => { xt.command("_cl_name"); await new Promise((r) => setTimeout(r, 1500)); return xt.log().some((l) => /WebTestName/.test(l)); })()' --expect 'r === true'
    # A new deploy: other page files, the same data.
    rm -rf "$OUT/site2"; mkdir -p "$OUT/site2"
    for f in "$SITE"/* "$SITE"/.well-known; do [ -e "$f" ] && ln -s "$f" "$OUT/site2/"; done
    rm -f "$OUT/site2/index.html"
    sed 's|</title>|</title><!-- another build -->|; s|"build":"[^"]*"|"build":"another-build"|' "$SITE/index.html" >"$OUT/site2/index.html"
    curl -sf "$M/__ctl?root=$OUT/site2" >/dev/null; reset
    step new-deploy "$E" --url "$M/" --until "$STARTED" --eval "$STATE" --expect 'r.panels.length === 0'
    expect_blobs new-deploy-no-requests -eq 0
    curl -sf "$M/__ctl?root=$SITE" >/dev/null
    # One chunk evicted: it comes back by itself, alone.
    step evict-one "$E" --no-navigate --eval "(async () => { await $OPFS_DIR.removeEntry('c-' + $VICTIM); return true; })()" --expect 'r === true'
    reset
    step evicted-restored "$E" --url "$M/" --until "$STARTED" --eval "$STATE" \
        --expect '/is gone from this browser: the browser removed it/.test(r.problem) && !r.panels.includes("xt-download")'
    expect_blobs evicted-one-request -eq 1
    # One chunk damaged on disk: Verify finds and removes it; the next start fetches it.
    step damage-one "$E" --no-navigate --eval "(async () => { const h = await $OPFS_DIR.getFileHandle('c-' + $VICTIM); const size = (await h.getFile()).size; const w = await h.createWritable({ keepExistingData: true }); await w.write({ type: 'write', position: 12345, data: new Uint8Array(64).fill(0xa5) }); await w.close(); return (await h.getFile()).size === size; })()" --expect 'r === true'
    step verify-finds-it "$E" --no-navigate --eval '(async () => { document.getElementById("xt-verify").click(); for (let i = 0; i < 600 && !/removed|intact|failed/.test(document.getElementById("xt-verify-text").textContent); i++) await new Promise((r) => setTimeout(r, 100)); return document.getElementById("xt-verify-text").textContent; })()' \
        --expect '/^1 damaged piece was removed/.test(r)' --timeout 120
    reset
    step damaged-restored "$E" --url "$M/" --until "$STARTED" --eval "$STATE" --expect '!r.panels.includes("xt-download")'
    expect_blobs damaged-one-request -eq 1
    # The switch: this browser says no (no engagement), and the page shows that.
    FLIP='(async () => { document.getElementById("xt-menu").click(); const sw = document.getElementById("xt-opt-persist"); if (!sw.checked) sw.click(); for (let i = 0; i < 50 && (sw.disabled || !xt.state.persistAnswer || xt.state.persistAnswer === "pending"); i++) await new Promise((r) => setTimeout(r, 100)); for (let i = 0; i < 50 && !/used of/.test(document.getElementById("xt-st-quota").textContent); i++) await new Promise((r) => setTimeout(r, 100)); return { checked: sw.checked, answer: xt.state.persistAnswer, text: document.getElementById("xt-persist-text").textContent, persisted: await navigator.storage.persisted(), calls: window.__persistCalls, rowHidden: document.getElementById("xt-opt-persist-row").hidden, quota: document.getElementById("xt-st-quota").textContent }; })()'
    step persist-denied "$E" --init "$OUT/count-persist.js" --url "$M/" --until "$STARTED" --eval "$FLIP" \
        --expect 'r.calls === 1 && r.checked === r.persisted && r.answer === (r.persisted ? "granted" : "denied") && !r.rowHidden && /used of/.test(r.quota)'
    # Granted (the permission given through CDP, as an installed or often
    # used site gets it; with it Chromium reports persisted() at once, so
    # the switch is either found on or turns on with one call).
    step persist-granted "$E" --init "$OUT/count-persist.js" --grant durableStorage --url "$M/" --until "$STARTED" --eval "$FLIP" \
        --expect 'r.calls <= 1 && r.checked === true && r.persisted === true && /keeps the game data until you delete it/.test(r.text)'
    # A later visit shows the answer without asking again. (The grant is
    # CDP's and lasts for its session, so this is a reload, not a restart:
    # keeping the persisted flag across restarts is the browser's part.)
    reset
    step persisted-shown-later "$E" --init "$OUT/count-persist.js" --grant durableStorage --url "$M/" --until "$STARTED" \
        --eval '(async () => { document.getElementById("xt-menu").click(); for (let i = 0; i < 50 && !/used of/.test(document.getElementById("xt-st-quota").textContent); i++) await new Promise((r) => setTimeout(r, 100)); return { checked: document.getElementById("xt-opt-persist").checked, persisted: await navigator.storage.persisted(), calls: window.__persistCalls }; })()' \
        --expect 'r.checked === true && r.persisted === true && r.calls === 0'
    expect_blobs persisted-no-requests -eq 0
    stop_browser KILL

    # The installed app (Chromium on Android reports it): offered on the first visit.
    cat >"$OUT/app.js" <<'JS'
navigator.getInstalledRelatedApps = async () => [{ platform: 'play', id: 'io.github.dixonsolutions.godnotic' }];
JS
    P="$OUT/p-app"
    start_browser "$E" "$P"
    step installed-app-offer "$E" --init "$OUT/app.js" --url "$M/" --until '!document.getElementById("xt-download").hidden' \
        --eval '({ app: !document.getElementById("xt-app").hidden, says: !document.getElementById("xt-app-text").hidden, label: document.getElementById("xt-app-open").textContent, href: document.getElementById("xt-app-open").getAttribute("href"), started: xt.state.started })' \
        --expect 'r.app && r.says && r.label === "Open the Godnotic app" && /^(godnotic:\/\/open|intent:\/\/open#Intent;scheme=godnotic;package=io\.github\.dixonsolutions\.godnotic;)/.test(r.href) && !r.started'
    stop_browser KILL

    # The worker writer through the whole page.
    P="$OUT/p-worker"
    start_browser "$E" "$P"; reset
    step worker-first-visit "$E" --url "$M/?autodownload&store=opfs-worker" --until "$STARTED" --timeout 900 \
        --eval '({ store: xt.state.store, lazy: xt.state.lazy })' --expect 'r.store === "opfs" && r.lazy === true'
    stop_browser KILL; start_browser "$E" "$P"; reset
    step worker-restart "$E" --url "$M/" --until "$STARTED" --eval "$STATE" --expect 'r.panels.length === 0'
    expect_blobs worker-restart-no-requests -eq 0
    stop_browser KILL

    if [ -n "$INSTALL" ]; then
        # showDirectoryPicker() answered with a directory the test fills from
        # the server (/__install/): an installed Godnotic's data folder, two
        # levels down, as under ~/.var/app/<id>.
        rm -f "$OUT/site2"/__install; mkdir -p "$OUT/site2"; ln -sfn "$INSTALL" "$OUT/site2/__install"
        curl -sf "$M/__ctl?root=$OUT/site2" >/dev/null
        cat >"$OUT/picker.js" <<'JS'
window.showDirectoryPicker = async () => (await navigator.storage.getDirectory()).getDirectoryHandle('picked-install');
JS
        NAMES="$(cd "$INSTALL" && ls *.pk3 | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read().split()))')"
        P="$OUT/p-folder"
        start_browser "$E" "$P"
        step folder-fill "$E" --init "$OUT/picker.js" --url "$M/" --until '!document.getElementById("xt-download").hidden' --timeout 600 \
            --eval "(async () => { const root = await (await navigator.storage.getDirectory()).getDirectoryHandle('picked-install', { create: true }); const dir = await (await (await root.getDirectoryHandle('data', { create: true })).getDirectoryHandle('godnotic', { create: true })).getDirectoryHandle('data', { create: true }); for (const n of $NAMES) { const w = await (await dir.getFileHandle(n, { create: true })).createWritable(); await (await fetch('/__install/' + n)).body.pipeTo(w); } const w = await (await dir.getFileHandle('config.cfg', { create: true })).createWritable(); await w.write('seta _cl_name \"FromTheFolder\"\\n'); await w.close(); return true; })()" \
            --expect 'r === true'
        reset
        # Pick it in the Storage popover: the page checks it, restarts, fetches
        # only its own files and starts.
        step folder-pick "$E" --init "$OUT/picker.js" --no-navigate \
            --eval '(async () => { document.getElementById("xt-menu").click(); await new Promise((r) => setTimeout(r, 300)); const b = document.getElementById("xt-folder"); if (b.hidden) return "FAIL the folder button is hidden"; b.click(); return true; })()' --expect 'r === true'
        step folder-start "$E" --init "$OUT/picker.js" --no-navigate --until "$STARTED" --timeout 600 \
            --eval "(async () => Object.assign(await $STATE, { where: (document.getElementById('xt-menu').click(), await new Promise((r) => setTimeout(r, 500)), document.getElementById('xt-st-backend').textContent), name: xt.log().some((l) => /FromTheFolder/.test(l)) || (xt.command('_cl_name'), await new Promise((r) => setTimeout(r, 1500)), xt.log().some((l) => /FromTheFolder/.test(l))) }))()" \
            --expect 'r.folder && r.folder.length >= 3 && r.lazy === true && /^Folder “picked-install” \(read only/.test(r.where) && r.name === true'
        expect_blobs folder-fetches-only-fork -lt 30
        stop_browser KILL; start_browser "$E" "$P"; reset
        step folder-restart "$E" --init "$OUT/picker.js" --url "$M/" --until "$STARTED" --eval "$STATE" \
            --expect 'r.folder && r.folder.length >= 3 && r.panels.length === 0'
        expect_blobs folder-restart-no-requests -eq 0
        # A match on the folder's maps pack.
        step folder-map "$E" --no-navigate --timeout 300 \
            --eval '(async () => { const n0 = xt.log().length; xt.command("bot_number 2; map stormkeep"); for (let i = 0; i < 1000; i++) { await new Promise((r) => setTimeout(r, 250)); if (xt.log().slice(n0).some((l) => /client: program loaded|CSQC.*loaded/i.test(l))) return { loaded: true, errors: (xt.lazyStats() || {}).errors || 0 }; } return { loaded: false, tail: xt.log().slice(-5) }; })()' \
            --expect 'r.loaded === true && r.errors === 0'
        # Stop using it: the page goes back to its own storage and asks for the download.
        step folder-stop "$E" --no-navigate --eval '(async () => { document.getElementById("xt-menu").click(); await new Promise((r) => setTimeout(r, 300)); document.getElementById("xt-folder-stop").click(); return true; })()' --expect 'r === true'
        step folder-stopped "$E" --no-navigate --until '!document.getElementById("xt-download").hidden && !xt.state.folder' \
            --eval '({ folder: xt.state.folder || null, size: document.getElementById("xt-dl-size").textContent, reason: document.getElementById("xt-dl-reason").textContent })' --expect 'r.folder === null && /folder is no longer used/.test(r.reason)'
        stop_browser KILL
    fi
fi

echo "browser tests: $PASSED passed, $FAILED failed"
[ "$FAILED" -eq 0 ]
