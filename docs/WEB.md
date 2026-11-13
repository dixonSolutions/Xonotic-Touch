# Godnotic in the browser

The same engine and game code as the Android, Flatpak and Click builds,
compiled to WebAssembly with Emscripten and drawn with WebGL 2. Touch
controls and keyboard and mouse work; gamepads go through the browser's
Gamepad API (not tested yet). Local games against bots run entirely in the
page.

- Vercel: https://godnotic.vercel.app (page and game data), rebuilt by
  Vercel on every push to `main`
- GitHub Pages: https://dixonSolutions.github.io/Godnotic/web/, a shell
  like the apps: the page, engine and this fork's game code (about 15 MB) are
  on Pages, and the stock Xonotic data is fetched from the Vercel deployment
  on first launch
- Every GitHub release carries `godnotic-web-<version>.zip`: the page and
  engine without game data, for hosting elsewhere

## What the player gets

| | |
|---|---|
| First visit | About 1 GB of game data downloads once (965 MB, plus 111 MB of optional music), with percent, speed, time left, Pause/Resume and Cancel |
| Later visits | Starts from the browser's own storage: no data requests at all |
| Updates | A new release fetches only the files that changed; under 50 MB (new game code is about 3 MB) it does so without asking |
| Interrupted downloads | Carry on where they stopped, after Cancel, a reload or a lost connection |
| Saved | `config.cfg`, touch layouts and everything else the game writes, kept in IndexedDB |
| Display | Fills the window; Fullscreen on Play, or the game's own fullscreen setting |
| Mouse | Click to capture (pointer lock, raw motion where the browser offers it); Esc opens the menu |
| Touch | Detected from the browser's pointer type; same controls and profiles as on Android |
| Sound | Web Audio at the device's rate; pauses with the tab |
| Phones | Landscape lock in fullscreen, screen kept on while playing, on-screen keyboard for chat and console |
| Install | "Add to Home Screen" or "Install app" gives a fullscreen, landscape app (web manifest) |

Needs a browser with WebAssembly and WebGL 2: current Chrome, Edge, Firefox or
Safari (16.4+). Desktop GPUs get hardware-compressed textures (S3TC); phones
without S3TC decode them in software at reduced resolution, as the Android
build does.

Not in the browser build yet:

- **Online servers.** Browsers cannot send UDP, which Xonotic servers speak.
  Joining public servers needs a WebSocket or WebRTC relay; local games with
  bots work. The engine sends no master server or LAN queries, so the
  server list stays empty instead of failing a WebSocket per master.
- **XonStat / player IDs.** No `d0_blind_id` crypto in the WebAssembly build.
- **WebGPU.** DarkPlaces has an OpenGL renderer only; WebGL 2 is the
  hardware-accelerated path (ANGLE on Direct3D, Metal or Vulkan underneath).
  A WebGPU renderer would be a new backend for the whole engine.
- **Low-memory phones.** The game data is held in memory while playing
  (about 1 GB plus the engine's own). Phones with 4 GB of RAM or less may
  close the tab.

## How it fits together

```
web/index.html, shell.css     loader UI: download, Play, settings
web/shell.js                  the platform layer (below)
web/store.js                  the game data store (chunks, hashes, resume)
web/manifest.webmanifest      installable app: fullscreen, landscape
web/serve.py                  local dev server
vercel.json                   Vercel: build commands, main-only, headers
scripts/vercel-build.sh       Vercel's build (tools, SDK, engine, data)
engine/darkplaces/sys_wasm.c  engine side of the shim (Module.xt, xt_web_*)
scripts/build-web.sh          engine + QuakeC + page -> build/web
scripts/stage-web-data.sh     game data -> build/web/data + manifest.json
.github/workflows/web.yml     build, release asset, GitHub Pages shell
```

`shell.js` is what an OS would otherwise provide:

- **Game data.** See "Game data" below: hash-checked 32 MiB chunks in the
  origin private file system (OPFS), or the Cache API where OPFS is not
  writable, handed to the engine's in-memory filesystem at start-up.
- **User data.** The engine's userdir is IndexedDB-backed (IDBFS), synced a
  moment after each write and when the tab is hidden or closed. Settings and
  key bindings changed in the menu or console are written to `config.cfg`
  and synced within about 2 seconds (`xt_web_autosave`), so a reload or a
  killed tab keeps them; the save on closing alone often does not finish.
- **Rendering.** The WebGL 2 context is forced opaque (DarkPlaces does not keep
  alpha at 1) and asks for the high-performance GPU. The page reports when the
  browser renders without the GPU. The render resolution is the CSS size times
  a scale (screen pixels, capped at 1.25 on phones and 2 elsewhere; changeable
  under "Settings and storage").
- **Fullscreen.** `vid_fullscreen` is a request to the page: it fullscreens the
  page (not just the canvas), locks landscape on phones, and on Chromium locks
  the keyboard so Esc, Tab and Ctrl shortcuts reach the game (hold Esc to
  leave). Without a fresh click it waits for the next tap or key.
- **Mouse.** Pointer lock when the game wants mouselook; menus use the absolute
  pointer. A lock the browser drops (Esc without keyboard lock) opens the menu,
  as Esc would.
- **Keyboard on phones.** A hidden text field is focused while the game wants
  text; what changes in it goes to the engine as text and Backspace.
- **Lifecycle.** Audio suspends with the tab and resumes on return; the screen
  wake lock is held while playing; config is saved when the tab is hidden.

Engine changes for the web, all behind `__EMSCRIPTEN__`:

- libpng, libjpeg and Ogg Vorbis linked statically (no `dlopen` in WebAssembly).
- The clock from `performance.now()`: Emscripten's `CLOCK_MONOTONIC_RAW` returns garbage.
- No threads (no `-pthread` build): the task queue runs everything on the main
  thread. Before, it handed work to threads that never started and spun forever
  once a match produced enough tasks.
- `Sys_Sleep` returns at once: `nanosleep` would spin the page's only thread.
  Idle time goes to the browser's frame scheduling instead.
- `vid_vsync` defaults to 1: the main loop runs once per
  `requestAnimationFrame`, since a browser shows no more frames than that.
  It is a default, not a forced value: Vertical Synchronization off in
  Settings > Video is saved and uncaps the frame rate as before.
- Dynamic vertex data through `glBufferSubData`: WebGL has no buffer mapping,
  and Emscripten rejects the unsynchronized `glMapBufferRange` the Android build
  uses (an error per draw call).
- DDS textures whose sizes WebGL will not take as S3TC (not whole 4x4
  blocks: 1x1 tints, 365x365 team blobs, 1024x255 walls) decoded to RGBA in
  software at full size; desktop GL accepts them, WebGL rejects the upload.
- S3TC detected under WebGL's extension name, fatal errors shown on the page,
  and the display, mouse and keyboard hooks above.

The page itself also answers the engine's stdin with end-of-file: Emscripten's
default stdin in a browser is `window.prompt()`, a modal "Input" dialog that
the engine would open every frame.

## Game data

### On the server

`scripts/stage-web-data.sh` writes `data/`:

```
data/blobs/<sha256>.bin   every file cut into 32 MiB chunks, each named by its
                          hash; small files are one chunk         immutable
data/m/<id>.json          the manifest: every file's path, size, sha256,
                          group (core/music) and chunk hashes; <id> is the
                          start of the sha256 of its own text      immutable
data/stock.json           {"manifest": "m/<id>.json"}: the stock files alone,
                          for shells on other hosts                 no-cache
data/manifest.json        names the current manifest, and lists the files
                          in the format pages before this layout read
                          (url/parts, now pointing at the blobs)    no-cache
```

The page is built knowing its manifest: `index.html` carries it in its
`xt-config` block, next to the content-hashed engine and page files (see
"Start-up"). A page without one (served from `web/`, or the release zip) asks
`data/manifest.json`.

What goes in: this fork's pk3dirs as `zz-*.pk3` (they sort after the stock
pk3s, so they win), the compiled game code (`progs.dat`, `csprogs.dat`,
`menu.dat`) alone in `zz-progs.pk3`, the loose configs (`xonotic.cfg`, touch
profiles), and the stock pk3s from the pinned data release
(`scripts/vercel-build.sh`, `data-20261107`) except nexcompat. Music is
optional in the page.

Reproducible: the zips get fixed timestamps (`SOURCE_DATE_EPOCH`, else
1980-01-01), modes, order and compression level, so the same commit stages
the same bytes on every run and on both hosts (given the same zlib), and a
release that changes no data changes no file. The game code carries a
per-commit watermark, which is why it is a file of its own: a code push
changes `zz-progs.pk3` (about 3 MB) and nothing else.
`scripts/test-web-cache-repro.sh` stages twice from differently timestamped
copies and compares; the Pages job compares its files with Vercel's when
both are on the same commit.

### In the browser (`web/store.js`)

One layout under the name `xt-data-v1` (the OPFS directory and the Cache API
cache: never rename it, older pages' downloads live there):

```
c-<sha256>        a whole chunk, checked with crypto.subtle.digest before it
                  is written; a damaged download is retried, never stored
p-<sha256>-<n>    4 MiB pieces of a chunk still downloading, kept on slow
                  connections (a chunk taking over 1.5 s): after a reload the
                  chunk carries on with an HTTP Range request
m-<id>.json       manifests seen, so a start needs no network
r-<page path>     which manifests a page path uses, and when
```

OPFS where `createWritable()` exists (Chrome, Edge, Firefox, Safari 26+),
else the Cache API with keys that carry no data host
(`https://xt-store.invalid/c-…`). `?store=cache` forces the fallback.

- **Warm start** checks that each chunk exists with the right size (about
  20 ms for the 40 chunks); it does not hash. "Verify data" (Settings and
  storage) hashes everything and drops what is damaged.
- **Downloads** run two chunks at a time; Pause, Cancel, a reload or a lost
  connection keep every finished chunk and the pieces of the rest.
- **Updates.** What is missing after a release is fetched without a question
  when it is under 50 MB and not a first install (`?ask` forces the dialog);
  bigger downloads and the first install ask, saying how much, how much is
  already there, and how much room the browser offers.
- **Older layouts** (pages before this one kept whole files as
  `<rev>-<path>` in OPFS, or 32 MiB parts keyed by data URL in the Cache
  API): adopted at the first start, each chunk read, hash-checked and
  written, the old copy removed once its file is whole. Nothing is
  downloaded again; a move from the Cache API to OPFS (a browser update)
  works the same way.
- **Pruning** runs only after a start that had every file it needed. It
  keeps every chunk of every manifest any page path started with in the last
  60 days (two Pages paths share one origin), and stops if one of those
  manifests cannot be read.
- **Persistence.** After a download the page asks for persistent storage
  (`navigator.storage.persist()`). The ready screen says how much is stored,
  how much the browser allows the site, and, when storage is not persistent,
  that the browser may clear it under disk pressure (and that Safari clears
  it after 7 days without a visit), pointing at Install / Add to Home Screen,
  which keeps it.

Each origin has its own storage: `godnotic.vercel.app`, every Vercel preview
URL and `dixonsolutions.github.io` each download once.

### Start-up

`scripts/build-web.sh` copies the engine (`godnotic.js`, `godnotic.wasm`),
`shell.js`, `store.js`, `shell.css` and the banner to `a/<name>.<hash>.<ext>`
and writes their names into `index.html`, with `<link rel="preload">` for the
engine so it downloads and compiles while storage is checked. `a/` is served
immutable, `index.html` no-cache: a deploy that changes nothing the page
loads costs one revalidation.

Measured in headless Chrome (GPU, local server, desktop), navigation to the
game menu:

| | before | after |
|---|---|---|
| Warm start, no added latency | 2.35-2.7 s, 1 data request | 2.6 s, 0 data requests |
| Warm start, 150 ms round trips | 3.4 s | 2.7 s |
| First visit (1 GB from localhost) | 6.9 s | 8.7 s (hashing every chunk) |
| A rebuild with unchanged data | everything again (Vercel) | 0 data requests |
| A rebuild with new game code | everything again (Vercel) | the manifest and `zz-progs.pk3` |
| An existing visitor's first start | n/a | 13 MB downloaded (the fork's pk3s, reproducible from now on), 1 GB adopted in 3.4 s |

Of a warm start, checking storage takes about 20 ms, reading the data into
memory about 550 ms, and the engine's own start about 1.9 s.

The data is copied into the JS heap (about 1.08 GB) on every start. That is
the remaining cost, on phones mostly as memory: tabs on 4 GB phones get
killed. Reading pk3s lazily needs synchronous reads from the engine, which
the main thread cannot do from OPFS:

- **Engine in a worker** with OPFS `FileSystemSyncAccessHandle` and a small
  Emscripten FS backend doing `handle.read(buf, {at})`: sync access handles
  are in every current browser and need no COOP/COEP, but the engine would
  move off the main thread, and SDL2's Emscripten port only does that with
  pthreads (`OFFSCREENCANVAS_SUPPORT`, proxied input and audio), which need
  SharedArrayBuffer and so COOP/COEP headers. Vercel can send them; GitHub
  Pages cannot (only a service-worker shim can fake them).
- **WASMFS with its OPFS backend**: also pthread-based, same headers.
- **JSPI** (`-sJSPI`): the engine stays on the main thread and a read
  suspends wasm while the page reads `File.slice()` from OPFS. No headers.
  Chrome 137+ only today (Firefox behind a flag, no Safari), so the copy
  stays as the fallback, and a map load becomes thousands of awaited reads
  unless the JS side reads ahead in blocks.
- **Maps on demand** (works everywhere, no headers): `xonotic-*-maps.pk3` is
  626 MB of the 1 GB. Repacked as one pk3 per map (in the pinned data
  release), the page would mount the core data and fetch/mount a map's pk3
  when the engine asks for it. Engine side: in `sys_wasm.c`, an
  `xt_web_need_file(name)` hook the map command calls when `maps/<x>.bsp` is
  missing, which defers the command until the page has written the pk3 to
  MEMFS and run `fs_rescan`; the loading screen already shows (the hook sits
  where the map load starts, in upstream `sv_ccmds.c`, as a one-line
  `// Touch:` call).

Recommended order: maps on demand (memory and first-download size, every
browser), then JSPI reads for the rest on Chromium.

### The slim data set

`Godnotic-data` also has a slim set for phones (433 MB against about 1 GB:
ASTC textures, zstd pk3 entries, Opus music, smaller JPEGs, development
files left out). The web build reads zstd entries (`touch_assets_zstd.c` is
compiled in), but not the rest as built today:

- **Opus**: the engine decodes it with libopusfile, linked on Android and
  loaded with `dlopen` elsewhere; the WebAssembly build has neither, so Opus
  music would be silent.
- **ASTC**: the KTX loader looks for `GL_KHR_texture_compression_astc_ldr`;
  WebGL names it `WEBGL_compressed_texture_astc`, which phones' browsers
  have and most desktops do not. It needs the same extension-name mapping
  the S3TC path got.

So the web keeps the full set for now. The next step that needs no engine
change is a "web" profile of `scripts/repack-phone-assets.py`:
`--profile slim --textures keep --audio keep` (zstd, the JPEG re-encode and
the dropped files; DDS and Vorbis as they are), built once as a pinned
release asset, because the repack needs Python 3.14 and numpy and takes long
on Vercel's 2-core, 45-minute build. `scripts/vercel-build.sh` would then
fetch that asset instead of the full one. Changing the data changes every
stock chunk, so every visitor downloads once more when it lands. ASTC for
browsers that have it means a second manifest the page picks by
`WEBGL_compressed_texture_astc`, after the engine's extension mapping and an
Opus decoder (libopus built with Emscripten) are in.

## Build and run locally

```bash
git clone --depth 1 https://github.com/emscripten-core/emsdk.git ~/.cache/emsdk
~/.cache/emsdk/emsdk install 7.0.0 && ~/.cache/emsdk/emsdk activate 7.0.0
scripts/build-web.sh
```

Game data comes from the pinned Godnotic data release (the archive Vercel
uses):

```bash
curl -L -o data.zip https://github.com/dixonSolutions/Godnotic-data/releases/download/data-20261107/Godnotic-data-20261107.zip
scripts/stage-web-data.sh --zip data.zip
python3 web/serve.py build/web
```

`serve.py` sends the same cache headers as Vercel, answers Range requests,
and with `XT_SERVE_LOG=FILE` logs every request (for counting them).

Open http://localhost:8080. `localhost` counts as a secure origin, which the
storage, pointer lock and fullscreen APIs need; anything else needs https.

Debugging:

- `?debug` prints the engine console to the browser console.
- `?logsink` sends every console line to `serve.py`, which prints it. This
  survives a page that freezes, and is the easy way to read a phone's console.
- `?args=+map%20stormkeep` adds engine arguments; `?autoplay` skips the Play
  button (no fullscreen or audio until the first click).
- `?store=cache` keeps the game data in the Cache API instead of OPFS, the
  path browsers without writable OPFS take (older Safari, Firefox private
  windows). A visit without it goes back to OPFS and moves the Cache copy
  there.
- `?ask` always shows the download question, even for a small update;
  `?manifest=m/<id>.json`, `?data=URL` and `?stock=URL` pick other data.

Tests: `scripts/test-web-cache.sh` runs `tests/web-cache/cache.test.mjs` (the
store and the loader flow in node, against an in-memory OPFS, Cache API and
a data host with Range: first visit, warm start with zero requests, small and
large updates, pause and resume, reload mid-chunk, damaged chunks, adopting
the older layouts, prune safety) and `scripts/test-web-cache-repro.sh`. CI
runs both on every push and pull request that touches the web loader.
`tests/web-cache/e2e.sh PROFILE URL LABEL` drives a real headless GPU Chrome
through a visit (clicks Download, waits for the menu) and prints times and
every data request; run it twice on one profile for a reload.
- `xt.command('...')` in the devtools console runs a console command.
- `scripts/build-web.sh --debug` keeps function names and Emscripten
  assertions.

## Deploying

**Vercel** (page and game data) builds itself from Git. The project imports
this repository; `vercel.json` sets the commands, so no dashboard settings
are needed:

- `ignoreCommand`: `main` and `work/web*` (loader previews) are built;
  other branches and pull requests are skipped (each build stages 1.1 GB of
  data).
- `installCommand` (`scripts/vercel-build.sh --install`): build tools,
  Python 3.10+ and GCC 12+ from `dnf` where the build image lacks them, and
  the Emscripten SDK, kept in `.vercel/cache` between builds.
- `buildCommand` (`scripts/vercel-build.sh`): `scripts/build-web.sh`, then
  the stock data from the pinned Godnotic data release (`data-20261107`;
  move the pin to change the data), staged with `scripts/stage-web-data.sh`.
  About 3½ minutes on the Hobby build machine (2 cores); the SDK comes from
  the build cache.
- `headers`: everything is `no-cache` except the content-addressed paths
  (`a/`, `data/blobs/`, `data/m/`), which are cached for a year as
  immutable. `data/` is served with `Access-Control-Allow-Origin: *` (the
  Pages shell reads it from another origin) and exposes `Content-Range`.
  Blobs are sent as `application/x-godnotic-chunk`: Vercel Brotli-compresses
  `application/octet-stream` on the fly (measured on the old layout), which
  wastes its CPU on pk3 data and drops `Content-Length`; a type outside its
  compressible list should avoid that (not yet confirmed: preview
  deployments are behind Vercel Authentication). Range requests are answered
  either way (206, identity), which is what resuming relies on.

No tokens or repository secrets are involved. The project is `godnotic`
in the `dixonsolutions-projects` Vercel team. To set it up again, either
import `dixonSolutions/Godnotic` in the dashboard (Add New → Project,
every setting at its default), or with the CLI:

```bash
npx vercel login                 # device code, approved in a browser
npx vercel project create godnotic
npx vercel link --yes --project godnotic
npx vercel git connect https://github.com/dixonSolutions/Godnotic --yes
```

then push to `main`, or start a deploy of `main` from the dashboard.

To test the build without Vercel, run it in a container of the same base
image:

```bash
podman run --rm -v "$PWD:/vercel/path0" -w /vercel/path0 \
  -e VERCEL_GIT_COMMIT_REF=main docker.io/library/amazonlinux:2023 \
  bash -c 'bash scripts/vercel-build.sh --install && bash scripts/vercel-build.sh'
```

Bandwidth: each new player downloads about 1 GB. Vercel's Hobby plan includes
100 GB of transfer a month, about 100 new players, and is for non-commercial
use. Pro, or another host for the stock data (any static host that sends
`Access-Control-Allow-Origin: *`), raises that; point the Pages shell at it
with the repository variable `XT_WEB_DATA_URL`.

**GitHub Pages** (shell). `.github/workflows/web.yml` runs after every
successful "Build and publish" run (each push to `main`), or by hand from the
Actions tab. It builds the page and engine, stages only this fork's files
(`stage-web-data.sh --shell`), attaches `godnotic-web-<version>.zip` to
the release, then takes the site "Build and publish" deployed (the Flatpak
and Click remotes), adds `web/` and deploys it again. Pages caps a site at
1 GB, so the stock pk3s stay on Vercel: the shell's manifest names that host
(`"stock"`), and the page reads `stock.json` there at each start (a few
hundred bytes), so Vercel can move to newer data without the shell being
republished. Both hosts build the fork's files from the same commit with the
same watermark, so they are the same chunks. `?stock=URL` overrides the host
for one visit.

"Build and publish" deploys Pages without `web/`, so the shell is missing
for the few minutes between the two workflows.
