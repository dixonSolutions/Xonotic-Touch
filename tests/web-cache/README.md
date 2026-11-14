# Web loader tests

Five layers, from fast to real:

| | What | Needs | Time |
|---|---|---|---|
| `scripts/test-web-cache.sh` | `cache.test.mjs`: the store (`web/store.js`) and the page's loader flow (`web/shell.js`) in node against an in-memory OPFS, Cache API and data host; `test-web-cache-repro.sh`: staging and the map split are reproducible, a split set is staged as it is; then `split.sh` where a browser is installed | node 22, python3 | seconds (CI runs it), plus `split.sh` |
| `split.sh` | the split set (progressive download) in headless Chromium and Firefox: the page's scripts as they are, a made-up split set of 1.6 MB (`make-split-set.py`) staged by `scripts/stage-web-data.sh`, a slow local server, and a stand-in engine (`stub-engine.js`). First visit (what was requested before the engine started), the rest arriving with `fs_mountpack` and reading back right, a kill mid-download, `map` and `dataWant` for a map that is not in yet, a browser holding the older layout | Chromium, Firefox | 1 min per engine |
| `browser.sh` | `storage.html`: the store against the browser's own OPFS, the browser killed and restarted between steps (Chromium, Firefox); with `--site`: the page in Chromium (persist switch, eviction, damage, new deploy, the app offer, an existing folder) | Chromium, Firefox; a built page for part 2 | 1 min, 5 with a page |
| `e2e.sh` | one visit in headless GPU Chromium over CDP: download, menu, bot matches, heap and renderer memory, throttling | a built page, Chromium | a minute |
| `matrix.sh` | every scenario in every engine this host can run (below) | a built page, Chromium, Firefox, Xvfb | 15-30 min per engine |

`split.sh` needs no built page and no game data: a made-up set could not
start the real engine, so `stub-engine.js` stands in for `godnotic.js`. It
gives the page the file system calls it makes (a lazy file's `read()` is
`web/lazyfs.js`'s own, so the synchronous reads from OPFS are real), keeps
a search path like the engine's (packs found at start in name order, the
last name first; `fs_mountpack` adds at the end and, like the engine, does
not see a file younger than its once-a-second directory listing), and
runs `map` through `Module.xt.needMap` before loading the map's `.bsp` and
every file that lists. `split-init.js` holds what the steps evaluate (and
a WebGL context for headless Firefox, which has none). The real engine on
the real split set is `e2e.sh`, below.

`lazyfs.html` checks the synchronous reads in place (`web/lazyfs.js`) in a
real browser: serve the repository root and evaluate `lazyfsTest()`.

## Setup

```bash
# Emscripten (see docs/WEB.md), then a page with data:
scripts/build-web.sh
curl -L -o data.zip https://github.com/dixonSolutions/Godnotic-data/releases/download/data-20261107/Godnotic-data-20261107.zip
curl -L -o slim.zip https://github.com/dixonSolutions/Godnotic-data/releases/download/data-slim/Godnotic-data.zip
scripts/stage-web-data.sh --zip data.zip --astc slim.zip
```

Browsers: Chromium from Playwright (`npx playwright install chromium`, or
set `CHROME=/path/to/chrome`), Firefox as the `org.mozilla.firefox`
Flatpak (or `FIREFOX=/path/to/firefox`), and `Xvfb`. No browser window ever
reaches the desktop: Chromium runs `--headless=new` with the GPU through
EGL; Firefox's headless mode has no WebGL, so it runs on a private Xvfb
display. Both scripts refuse to start while `WAYLAND_DISPLAY` or a desktop
`DISPLAY` is set; run them under `env -u`.

## Running

```bash
env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY tests/web-cache/split.sh                     # the split set, both engines
env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY tests/web-cache/browser.sh                   # the store, both engines
# the page too, and an existing folder: DIR holds stock pk3s under their
# stock names (xonotic-<date>-data.pk3, -maps.pk3, -music.pk3, ...)
env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY tests/web-cache/browser.sh --site build/web --install DIR
```

`step.mjs` is its driver: one navigation and evaluation in a browser
already running, over CDP (Chromium) or WebDriver BiDi (Firefox). Firefox
runs with `MOZ_HEADLESS` here (the store needs no WebGL). What it cannot
do: show the real folder picker or permission prompts (the folder test
answers `showDirectoryPicker()` with an OPFS directory handle it fills),
and run on Android.


```bash
env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY tests/web-cache/matrix.sh                    # both engines
env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY tests/web-cache/matrix.sh --engines firefox
env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY OUT=/tmp/m tests/web-cache/matrix.sh build/web
```

It serves the page with `matrix_server.py` (web/serve.py's vercel.json
headers, plus fault injection, a switchable root and request counters),
makes three more roots from the build (the same data with a new page; one
changed small file, as a game-code push; a GitHub Pages-style shell on a
second origin that takes the stock data from the first), and runs, per
engine:

| Scenario | Passes when |
|---|---|
| first | a fresh profile downloads and the game starts (no Play button) |
| reload, newtab, restart | the game starts with 0 data requests |
| deploy-same | a new page with the same data: 0 data requests |
| deploy-code | one changed file: at most 2 requests, no question |
| kill-start | the browser killed while the engine starts; next start: no download, the reason is shown |
| offline | expected to fail: there is no service worker, so the page itself cannot load offline |
| slow-cpu | Chromium at 4x CPU: warm start time to the menu |
| cleared | site data deleted: it downloads again and starts |
| shell, back | the shell on another origin downloads into its own storage; the main origin still needs nothing |
| resume | killed (SIGKILL) at three points of the download; the fourth visit finishes; total fetched about the data size |
| two-tabs | two tabs at once: both start, one download (Web Locks) |
| flaky | 30% of chunk requests answer 503, then 30% of bodies are cut off: it still finishes |
| quota | a 200 MB quota: a clear "Not enough space" message |
| private | a private window: plays, storage "memory" where the browser offers nothing |
| mobile | a phone (mobile UA, touch, small screen): plays |

`OUT` (default `/tmp/xt-matrix`) on the same filesystem as the build lets
the extra roots be hard links (otherwise they are 1.5 GB copies each); they
and the browser profiles are removed at exit.

Results: a table on stdout, `$OUT/results.tsv`, and each visit's JSON in
`$OUT/<scenario>-<engine>.json` (`matrix.mjs` prints what the page showed,
the store it used, the reason it gave for downloading, blob requests and
bytes).

WebKit: not on this host (no Epiphany or WebKitGTK MiniBrowser, and
Playwright's WebKit builds carry no published checksums). Run the same
steps by hand in Safari: first visit, reload, close and reopen, Storage
popover after each.
