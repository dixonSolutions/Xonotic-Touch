# Web loader tests

Three layers, from fast to real:

| | What | Needs | Time |
|---|---|---|---|
| `scripts/test-web-cache.sh` | `cache.test.mjs`: the store (`web/store.js`) and the page's loader flow (`web/shell.js`) in node against an in-memory OPFS, Cache API and data host; `test-web-cache-repro.sh`: staging and the map split are reproducible | node 22, python3 | seconds (CI runs it) |
| `e2e.sh` | one visit in headless GPU Chromium over CDP: download, menu, bot matches, heap and renderer memory, throttling | a built page, Chromium | a minute |
| `matrix.sh` | every scenario in every engine this host can run (below) | a built page, Chromium, Firefox, Xvfb | 15-30 min per engine |

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
