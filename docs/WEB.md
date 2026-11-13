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
| First visit | One button, one size, everything (every map, the music): 405 MB on GPUs with ASTC (phones, tablets, Apple, Intel), 1.07 GB elsewhere until the web slim set is published (878 MB then); percent, speed, time left, Pause/Resume and Cancel. The game starts by itself when it is done |
| Later visits | Straight from a splash into the game: no download screen, no Play button, no data requests |
| Why it downloads | Whenever the page has to download again it says why in one line (cleared by the browser, closed by the browser mid-download, not enough space, private window, an update) |
| Memory | Game data is read from the browser's storage as the engine needs it, not copied into memory: about 30 MB of JS heap at the menu instead of 1.08 GB |
| Returning visitors | Keep the full download they already have; nothing is downloaded again |
| Updates | A new release fetches only the files that changed; under 50 MB (new game code is about 3 MB) it does so without asking |
| Interrupted downloads | Carry on where they stopped, after Cancel, a reload, a lost connection or the browser killing the page |
| Storage | The origin private file system (OPFS) of the browser: it survives reloads, closing the tab, restarting the browser and new releases of the page. The page never asks for persistent storage by itself, so by default the browser may clear it when the device runs short of space; what is gone is downloaded again at the next start, only that, without a question |
| Storage popover | The header bar's menu button (or `web_storage` in the console, over the game): where the data is kept, how much, the site's quota, the switch "Keep game data on this device" (persistent storage, with the browser's real answer), the last problem; Verify data, Clear data, Install app, Use an existing Godnotic folder (desktop Chromium), Import settings; render resolution and fullscreen |
| Already have the app | On Android in Chrome, a first visit says when the Godnotic app is installed and offers "Open the Godnotic app" instead of the download; other browsers on Android and Linux get the same hand-over as a plain link. See "An existing install" |
| Saved | `config.cfg`, touch layouts and everything else the game writes, kept in IndexedDB |
| Display | Fills the window; fullscreen at the first tap or key (Storage popover), or the game's own fullscreen setting |
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
- **The engine's own memory.** The game data no longer sits in memory, but
  the engine's WebAssembly memory grows with what a map loads (textures,
  models, sounds) and never shrinks: the page's renderer process measured
  0.8-1.1 GB in a match, most of it wasm memory and WebGL's copies. Phones
  with 3 GB of RAM or less may still close the tab on big maps.

## How it fits together

```
web/index.html, shell.css     loader UI: download, Play, settings
web/shell.js                  the platform layer (below)
web/store.js                  the game data store (chunks, hashes, resume)
web/lazyfs.js                 pk3s read from that store on demand (no copy)
web/manifest.webmanifest      installable app: fullscreen, landscape; names the Android app
web/.well-known/assetlinks.json  Android App Links: the app and its signing certificate
web/serve.py                  local dev server
vercel.json                   Vercel: build commands, main-only, headers
scripts/vercel-build.sh       Vercel's build (tools, SDK, engine, data)
engine/darkplaces/sys_wasm.c  engine side of the shim (Module.xt, xt_web_*)
scripts/build-web.sh          engine + QuakeC + page -> build/web
scripts/stage-web-data.sh     game data -> build/web/data, one manifest per profile
scripts/split-web-maps.py     a maps pk3 -> core pack + one pack per map
.github/workflows/web.yml     build, release asset, GitHub Pages shell
```

`shell.js` is what an OS would otherwise provide:

- **Game data.** See "Game data" below: hash-checked 32 MiB chunks in the
  origin private file system (OPFS), or the Cache API where OPFS is not
  writable, which the engine reads in place (lazy reads); maps fetched when
  the engine asks for them.
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

- libpng, libjpeg, Ogg Vorbis, libopus and opusfile linked statically (no
  `dlopen` in WebAssembly; Opus is the slim set's music).
- `map` and `changelevel` ask the page first (maps on demand, see "Game
  data"): wrapped from `sys_wasm.c` at start-up, no upstream file changed.
  `web_maps pause|resume|status` controls the background map download.
- ASTC textures (the slim set's KTX files) under WebGL's extension name.
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

### One download, picked by GPU

There is one download and nothing to choose: the page picks the data set by
the GPU at the first visit and keeps it:

| Set | What | For | The download |
|---|---|---|---|
| `astc` | the slim phone set (`Godnotic-data` release `data-slim`): ASTC textures in KTX, zstd entries, Opus music, re-encoded JPEGs, no editor files | GPUs with `WEBGL_compressed_texture_astc`: phones and tablets (Mali, Adreno, PowerVR, Apple), Apple Silicon, Intel's integrated GPUs | 405 MB |
| `web` | the stock pk3s (DXT); with the web slim set published, that set instead | every other GPU: desktops with NVIDIA or AMD (S3TC), the rare phone with neither (DXT decoded in software at reduced resolution) | 1.07 GB stock, 878 MB web slim |
| `full` | the stock pk3s as whole files, as pages before this layout stored them | only browsers that already hold all of it | nothing |

The slim set's textures are ASTC except where DXT came out smaller, so a GPU
without ASTC cannot use it; S3TC is on nearly every desktop GPU and on
almost no phone GPU (a Mali-G68 exposes ASTC and ETC2, no S3TC), so one set
cannot serve both. ETC2 (mandatory in OpenGL ES 3 and so on every phone,
but emulated with a decode on most desktop GPUs) would be the only
universal format, at about twice ASTC's size for the same quality: not
worth a third set.

`full` is how visitors from before keep what they have: its maps pk3 is no
longer hosted (`"hosted": false`), so the page keeps that set only while
every chunk of it is in the browser (older layouts adopted first). They
download nothing but new game code, and get the reads in place below.

The web slim set: `repack-phone-assets.py --profile slim --textures keep`
on the stock pk3s (zstd entries, re-encoded JPEGs, Opus music, editor files
dropped, DXT kept) measured 878 MB for everything (data pk3 281 MB, maps
530 MB, music 47 MB). It needs Python 3.14, numpy, Pillow and ffmpeg and
takes long on Vercel's 2 cores, so it is a release asset:
`scripts/vercel-build.sh` takes it from `XT_WEB_DATA_ARCHIVE` (with
`XT_WEB_DATA_SHA256`) and stages it as `web`.

`?profile=web` (or `astc`, `full`) forces a set for a visit.

### Maps on demand

`scripts/split-web-maps.py` splits each profile's maps pk3 (626 MB stock,
230 MB slim) into:

| | stock | slim |
|---|---|---|
| `<maps>-0core.pk3`: textures two or more maps share, whatever the game itself names, untraced small files, and every map's stand-ins | 100 MB | 31 MB |
| `<maps>-<map>.pk3`, one per map (29): the .bsp, lightmaps, real .mapinfo, waypoints, radar image, and what only that map uses | 3-22 MB, 298 MB in all | 0.6-18 MB, 144 MB |
| `<maps>-0extra.pk3`: textures, models and sounds no map and nothing in the game uses (texture sets for mappers) | 222 MB | 57 MB |

What a map uses is traced from its .bsp (shader names, entity paths, noise
keys relative to `sound/`, worldspawn's `sky` with its six sides), the shaders by name and every path in them
(skyparms' six sides), md3/iqm models and their .skin files, and a
texture's `_norm`/`_gloss`/`_glow`/`_pants`/`_shirt`/`_reflect` companions;
whatever the data pk3's QuakeC, configs, shaders and models name stays in
the core. Entries are copied byte for byte (no recompression, zstd entries
too) into zips with fixed timestamps: the same input gives the same packs.
Putting the 98 MB of shared textures into every map that uses them instead
would have added 339 MB; grouping them by the set of maps using them makes
254 packs. Neither was worth it.

The menu lists every map whether or not its pack is mounted: the core pack
has, for each map, the level shot, the .mapinfo and an empty
`maps/<map>.bsp` (the menu enumerates `maps/*.bsp`). The map pack sorts
after the core pack, so once it is mounted its real .bsp wins.

The engine asks the page before every `map` and `changelevel` (the menu,
the console, map rotation, votes and QuakeC's changelevel builtin all go
through those two commands): `sys_wasm.c` wraps both at start-up, through
the command list, so no upstream file changes. When the map's pack is not
mounted, the page fetches it with a progress card over the game (Cancel
keeps what arrived), mounts it, and runs `fs_rescan` and the same command
again. The engine never waits on the network, so nothing needed Asyncify
(which instruments every function that can be on the stack under the call
site: the whole main loop here, typically +30-50% code size and slower
calls) or JSPI: the cost is one frame and a rescan of the pk3 list.

The first download is everything, so the per-map packs are not about
downloading any more but about memory: where the page cannot read in place
(below) it copies everything but the map packs into memory, and the one
map being played; a new map's pack replaces the last one. The fetch-when-played path
stays as the fallback for a pack that is missing (removed by the browser,
or an interrupted update): a card over the game shows its download.
`web_maps` in the console says how much of the data is stored.

### Memory: lazy reads

Pages before this one copied every stored pk3 into the JS heap at each
start (MEMFS, about 1.08 GB), which phone browsers kill tabs for. Now
`web/lazyfs.js` gives the engine files whose `read()` fetches the bytes
from the stored chunks themselves (OPFS files or Cache API blobs, which
stay on disk):

- a synchronous `XMLHttpRequest` for a `blob:` URL of just the bytes
  wanted (`chunk.slice(off, off + len)`: a slice refers to the stored
  chunk, nothing is copied). Main-thread synchronous XHR is still allowed;
  with the `x-user-defined` charset its text is the bytes. Measured with
  ranged requests on one URL per chunk, which this replaced: Chrome about
  10 ms per MiB, Firefox about 5 ms.
- 1 MiB blocks, the most recent kept (48 MiB); reads of a block or more go
  straight into the engine's memory.
- read-ahead: synchronous reads cost ten times what `Blob.slice()
  .arrayBuffer()` does, so the page remembers which blocks start-up and map
  loads read (per profile, in localStorage), reads those, the map's pack
  and the rest of the core (up to 384 MiB, 160 MiB on phones) into the cache
  before the engine starts or loads a map, four runs at a time, and shrinks
  the cache to 16 MiB 15 s after the load.
- byte order marks: text decoding sniffs FF FE, FE FF and EF BB BF at the
  start of a response before it looks at the charset, so a read that
  happened to start with one came back as UTF-16 or UTF-8 (half the
  length, wrong bytes). Every read's blob therefore starts with three zero
  bytes of its own (`new Blob([zeros, chunk.slice(...)])`), so no response
  ever starts with data. (The first fix asked a ranged read again from one
  byte earlier; a run of alternating FE/FF bytes before a block start
  defeated that.) `tests/web-cache/lazyfs.html` plants marks and such runs
  at chunk and block starts and checks every byte.
- a read the browser's storage fails (a chunk removed under the page, a
  picked folder's file changed on disk) reaches the engine as `EIO`, not
  as an exception that stops the game; `xt.lazyStats().errors` counts them.
- a probe at start-up checks a 16-byte read against `Blob.slice()`;
  where it fails (status, length or bytes), the page copies the files into
  memory as before, and then mounts everything but the map packs, and the
  one map being played (a new map's pack replaces the last one).

The other options, measured or checked:

| | Works on | Cost | Verdict |
|---|---|---|---|
| Mount only the packs a session needs | everywhere | still copies the core (156 MB slim, 440 MB stock) | done, as the fallback |
| Synchronous XHR on blob: URLs, read-ahead in the background | Chrome, Firefox (both checked); Safari unverified | about 10 ms/MiB, hidden by read-ahead | done |
| Engine in a worker, OPFS `createSyncAccessHandle` | everywhere | SDL2's Emscripten port only runs off the main thread with pthreads (OFFSCREENCANVAS_SUPPORT, proxied input and audio), which need SharedArrayBuffer, so COOP/COEP headers. Pages cannot send them; a service-worker shim (coi-serviceworker) can fake them, but needs a reload on the first visit, breaks when the worker is not in control (hard reload, private windows in some browsers), and makes every cross-origin resource need CORP/CORS (the Pages shell's data on Vercel has it). The page's touch, keyboard and pointer-lock code would have to proxy to the worker. | not done |
| JSPI (`-sJSPI`) | Chrome/Edge 137+, Firefox from 153 (intent to ship), Safari unconfirmed | a second engine build for other browsers, and a map load becomes thousands of awaited reads | not done |
| Free each ArrayBuffer after indexing, re-read on demand | needs a synchronous read: one of the above | | the lazy reads are this |

### Why downloads restarted (Firefox for Android)

What the owner saw on a Samsung Tab Active5 in Firefox, "it keeps
reinstalling", from before the store redesign, follows from the pages of
that time; desktop Firefox here reproduces the parts a desktop can:

1. **The first store kept whole files.** A pk3 was written with one
   `createWritable()` stream and committed only at `close()`: the 626 MB
   maps pk3 only existed once all of it had arrived. Firefox writes a
   writable stream to a swap file first, so finishing that file needed
   twice its size in quota. A page killed during it (Fenix discards
   background tabs and kills content processes under memory pressure)
   lost the whole file, and the next start began it again.
2. **Every start copied 1 GB into the JS heap** (`get()` read a whole file
   into one ArrayBuffer, then MEMFS). Measured in desktop Firefox on
   production: 7.7 s of mount with 1.06 GB of heap. On a tablet that is
   where the content process dies; the next load finds the data, copies it
   again, dies again, and from the player's side it never gets anywhere.
3. **`persist()` was awaited.** Firefox answers it only when the player
   answers its permission prompt; the redesign waited for it after the
   download, so a finished download sat at 100 % (reproduced on production
   in desktop Firefox: 1.06 GB downloaded, never reached Play). Reloading
   then started fine, which looks like "it downloaded again".
4. **Best-effort quota.** Without persistent storage Firefox gives a site a
   group limit derived from free disk (here `estimate()` reported 3.35 GB);
   on a tablet with little free space 1.07 GB plus a swap file may not fit,
   and best-effort data is the first evicted under pressure. `vercel.app`
   and `github.io` are on the Public Suffix List, so each site is its own
   group.

What changed: 32 MiB chunks committed one by one with their hash (a kill
loses at most the chunks in flight; on slow connections 4 MiB pieces), the
data read in place instead of copied (heap at the menu: 30 MB, not 1 GB),
`persist()` never called on the start path (now: never by the page at all,
only by the switch in the Storage popover), the slim set
(405 MB) on ASTC GPUs such as the tablet's Mali-G68, a memory-only store
with a stated reason where nothing can be stored (Firefox private windows
throw `SecurityError` from `getDirectory()` and from the Cache API), and
breadcrumbs that say when the browser killed the page.

What Firefox for Android has, by the compatibility data (not run here):
`navigator.storage.getDirectory()`, `createWritable()` and, in workers,
`createSyncAccessHandle()` since Firefox 111, the same as desktop. The page
uses `createWritable()` there. Desktop Firefox 160, headless, passes
`tests/web-cache/browser.sh`: a download into OPFS cut off and resumed, the
browser killed (SIGKILL) and started again with every chunk there and
intact, the same through the worker writer, an evicted and a damaged chunk
fetched again alone.

What desktop Gecko cannot show: Fenix's memory limits and tab discarding,
its quota on that device's free space, how its permission prompt for
`persist()` looks (or whether it auto-denies), whether "Delete browsing
data on quit" is on, and its GPU's exact WebGL extensions. Those are on the
checklist under "Checking on a tablet by hand".

### Checking on a tablet by hand

Nothing here was run on a phone or tablet. On the Samsung Tab Active5
(Mali-G68), in Chrome and then in Firefox, on Wi-Fi, with the preview or
production URL:

1. Open the page. It shows "Download Godnotic", 405 MB (1.07 GB would mean
   the tablet got the DXT set), a line that the browser may not keep the
   data, and under the size either "Godnotic is installed on this
   device…" with "Open the Godnotic app" (Chrome, once the app with
   `asset_statements` is installed) or the link "Have the Godnotic app?
   Open it" (Firefox; Chrome with an older app).
2. Tap the app button or link. With the app installed (a build with the
   `godnotic` intent filter) it opens. Without it, Chrome comes back to
   the page with "The Godnotic app is not installed on this device."; in
   Firefox note what happens (a prompt, nothing, an error page).
3. Back in the browser, tap the menu button: "Stored in: Browser files
   (OPFS)", "Space: … used of N GB" (N well over 0.5), the switch "Keep
   game data on this device" off. Leave it off.
4. Tap Download and keep the tablet awake until the game starts by itself.
   No permission prompt may appear (the page no longer asks for persistent
   storage). Play a bot match on stormkeep and one more map; note if the
   browser closes the page (memory: desktop Firefox under a 2 GB limit was
   killed loading stormkeep, with software GL overstating it).
5. Menu button: "Game data: 405 MB of 405 MB · ASTC set", "Last problem:
   None".
6. Close the tab, swipe the browser away, open the page again: the game
   starts with no download page. Then restart the tablet and repeat. If a
   download page or a progress page shows instead, its yellow line and the
   popover's "Last problem" say why: note both, and whether the browser is
   set to delete browsing data on quit (Firefox: Settings, Delete browsing
   data on quit).
7. Turn the switch on. Chrome answers at once (most likely no on a fresh
   profile: the line under it says so and the switch goes back; after
   "Install app" or some days of use it says yes). Firefox shows its
   prompt: allow it, and the switch stays on; the game must not have
   stopped meanwhile. Reload: the switch shows the same answer.
8. During a first download (clear the site's data first), switch to
   another app for a few minutes and come back. If the browser reloaded
   the page, it must say "The browser closed this page while downloading
   (what had arrived is kept; it carries on from there)" and continue from
   there, not from 0 %.
9. Chrome only, with the app installed from a release signed with the
   repository keystore: tap a link to https://godnotic.vercel.app/ in
   another app (a chat message, a note). After the app's first start
   Android verifies the link (Settings, Apps, Godnotic, Open by default:
   "1 verified link"); the tap then opens the app instead of the browser.

### On the server

`scripts/stage-web-data.sh` writes `data/`:

```
data/blobs/<sha256>.bin   every hosted file cut into 32 MiB chunks, each
                          named by its hash; small files are one chunk  immutable
data/m/<id>.json          one manifest per profile (format 4): every file's
                          path, size, sha256, group, chunk hashes, "map" for
                          a map pack, "hosted": false for the full set's maps
                          pk3; <id> is the start of the sha256 of its text immutable
data/profiles.json        {default, profiles: {web, astc, full: {manifest,
                          gpu?, keep?}}}; the built page carries a copy  no-cache
data/stock.json           the same for the stock files alone, for the
                          Pages shell; "manifest" names the full set's, for
                          shells built before profiles                  no-cache
data/manifest.json        the web profile in the format pages before
                          content addressing read (url/parts)          no-cache
```

Groups: `core` (needed before the menu: this fork's files, the stock data,
xoncompat and font pk3s, the maps core pack), `map` (one map's pack),
`extra`, `music`. All of them are one download; the groups only say what
the in-memory fallback mounts when. The page is built knowing its
profiles: `index.html` carries them in its `xt-config` block, next to the
content-hashed engine and page files (see "Start-up"); a page without them
(served from `web/`, or the release zip) asks `data/profiles.json`.

What goes in: this fork's pk3dirs as `zz-*.pk3` (they sort after the stock
pk3s, so they win), the compiled game code (`progs.dat`, `csprogs.dat`,
`menu.dat`) alone in `zz-progs.pk3`, the loose configs (`xonotic.cfg`, touch
profiles), and the stock pk3s from the pinned data releases
(`scripts/vercel-build.sh`: `data-20261107` and `data-slim`) except
nexcompat. All profiles share this fork's files (same chunks), and `web`
and `full` share every stock chunk but the maps pk3's.

Reproducible: the zips get fixed timestamps (`SOURCE_DATE_EPOCH`, else
1980-01-01), modes, order and compression level, so the same commit stages
the same bytes on every run and on both hosts (given the same zlib), and a
release that changes no data changes no file. The game code carries a
per-commit watermark, which is why it is a file of its own: a code push
changes `zz-progs.pk3` (about 3 MB) and nothing else.
`scripts/test-web-cache-repro.sh` stages twice from differently timestamped
copies and compares, and splits a made-up maps pk3 twice and checks where
each file went; the Pages job compares its files with Vercel's when both
are on the same commit.

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

Where: the origin private file system (`navigator.storage.getDirectory()`).
Chunks are written with `createWritable()` on the main thread where it
exists (Chrome and Edge 86+, Chrome for Android 109+, Firefox 111+ on
desktop and Android, Safari 26+): the browser writes a swap file and
replaces the entry on `close()`, so a reader never sees half a chunk.
Where it does not (Safari 15.2 to 18), a worker made from text inside
`store.js` writes with `createSyncAccessHandle()` (truncate, write, flush):
a write cut short leaves a file of the wrong size, which counts as absent.
`?store=opfs-worker` forces that path (tested in Chromium and Firefox;
Safari itself is unverified). Where OPFS cannot be written at all, the
Cache API with keys that carry no data host
(`https://xt-store.invalid/c-…`; `?store=cache` forces it), and where
nothing can be stored (Firefox private windows) memory, for that visit.

What the store does, against what was asked of it:

| | |
|---|---|
| Streamed writes | A chunk (32 MiB at most) is received into one reused buffer, hashed, then written in one piece; on slow connections its 4 MiB pieces are written as they arrive. Memory while downloading: two 32 MiB buffers, whatever the size of the data |
| Resumable | Per chunk, and inside a chunk from its last 4 MiB piece with an HTTP `Range` request |
| Integrity | SHA-256 of every chunk before it is stored; a chunk that fails twice is an error, never stored |
| Read in place | Yes (`web/lazyfs.js`, above): nothing is copied into WebAssembly memory but the blocks the engine reads |
| Survives | Reloads, closing the tab, a browser restart (tested with SIGKILL in Chromium and Firefox), and new releases: chunks are named by their SHA-256, never by a build or a manifest, so a release that changes no data fetches nothing, and one that changes a file fetches that file's chunks |
| Deleted by the page | A chunk whose size is wrong when it is mounted, or whose hash is wrong when "Verify data" runs; "Clear data"; and pruning (below), which only touches chunks no release used in the last 60 days lists any more (replaced game code, mostly). Nothing else, ever |

- **Warm start** checks that each chunk exists with the right size (about
  20 ms for the 40 chunks); it does not hash. "Verify data" (Settings and
  storage) hashes everything and drops what is damaged.
- **Downloads** run two chunks at a time; Pause, Cancel, a reload or a lost
  connection keep every finished chunk and the pieces of the rest.
- **Updates.** What is missing after a release is fetched without a question
  when it is under 50 MB or under a quarter of the whole, and not a first
  install (`?ask` forces the dialog); bigger downloads and the first install
  ask, saying how much, how much is already there, and how much room the
  browser offers.
- **Data the browser removed.** Each start notes which release (manifest
  ids) it started with. When a later start of the same release finds
  chunks missing, they were removed (evicted, or site data deleted): the
  page says so in one line and downloads exactly those again with the
  progress page, without a question and without an error. A browser that
  evicts takes the whole origin at once (OPFS, IndexedDB and localStorage
  together), so after a real eviction the page cannot know it was ever
  here: that visit looks like a first one, with the Download button, and
  the settings are gone with it. "Clear data" in the popover is the
  player's own doing and also leads to the first-visit question.
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
- **Persistence.** The page never calls `navigator.storage.persist()` by
  itself. Without it the storage is "best effort": it stays between
  visits, but the browser may delete the whole site's data when the device
  runs short of space (least recently used sites first), Safari deletes
  script-written data of sites not visited for seven days of use, and a
  browser set to delete site data when it closes deletes it every time.
  The first-visit page says so. The switch "Keep game data on this device"
  in the Storage popover calls `persist()` inside that click and then shows
  what `persisted()` says: Chrome and Safari answer by themselves, without
  a prompt (Chrome says yes to sites that are installed, bookmarked or used
  often; a fresh profile gets no), Firefox asks the player. A yes cannot be
  taken back from the page (there is no API for it), only in the browser's
  site settings, and the switch says that too. The popover also shows
  `estimate()`: used and quota.

Each origin has its own storage: `godnotic.vercel.app`, every Vercel preview
URL and `dixonsolutions.github.io` each download once.

### Start-up

`scripts/build-web.sh` copies the engine (`godnotic.js`, `godnotic.wasm`),
`shell.js`, `store.js`, `shell.css` and the banner to `a/<name>.<hash>.<ext>`
and writes their names into `index.html`, with `<link rel="preload">` for the
engine so it downloads and compiles while storage is checked. `a/` is served
immutable, `index.html` no-cache: a deploy that changes nothing the page
loads costs one revalidation.

Measured 2026-11-14 in headless Chromium 1248 (Playwright) on an Intel
RPL-S iGPU through ANGLE/EGL, against `web/serve.py` (vercel.json's
headers), network throttled with CDP, "before" being main before maps on
demand (the full 1.06-1.08 GB set copied into memory) and "after" this
layout with the ASTC set this GPU gets. The host was busy with other builds
(load average 20-33 on 24 cores), so times vary by up to half; before and
after runs were interleaved.

| | before | after |
|---|---|---|
| Downloaded before playing | 1.076 GB | 405 MB (ASTC set; 1.07 GB stock DXT, 878 MB web slim) |
| First visit, 20 Mbit/s: to the menu / a bot match | 442 s / 462 s | 176 s / 183 s |
| First visit, 5 Mbit/s: to the menu | about 29 min (1.076 GB at 625 KB/s) | about 11 min (405 MB) |
| First visit, local: to the menu | 13-54 s | 5-12 s |
| Warm start: mount phase | 1.0-1.6 s (copying 1.08 GB) | 0.04-0.3 s |
| Warm start: navigation to the engine's first frame | 5.7-6.8 s (to the menu) | 2.2-2.3 s |
| Warm start at 4x CPU throttling (to the menu) | 19-25 s | 4.7 s |
| JS heap incl. ArrayBuffers at the menu | 1079 MB | 30-45 MB, 160-220 MB for 15 s while read-ahead holds the core |
| Same, a minute into a match, after GC | 1079 MB | 17-20 MB |
| Renderer process private memory in a match | 1.84-1.90 GB | 0.89-1.02 GB |
| Chromium limited to 2 GB (cgroup), warm start and two bot matches | killed | plays |
| Chromium limited to 1.5 GB | killed | killed |
| Map load, warm (stormkeep, then boil) | 15.8 s, 3.2 s | 4-6 s, 2-4 s |
| Production in Firefox (desktop), first visit | 1.06 GB downloaded, then stuck at 100 % (persist() prompt) | starts by itself |

What a warm start spends now (Chromium, warm, unthrottled): the page and
its config 0.2-0.6 s (the engine's 5 MB wasm compiles meanwhile, from the
HTTP cache), the storage check 15-30 ms (parallel existence and size, no
hashing, no network), the mount 40-300 ms (one `getFile()` per chunk,
kept from the check), the read-ahead of the blocks earlier starts read
(about 94 MB on the ASTC set) 170-360 ms, then the engine's own start
(configs, menu QuakeC, fonts, textures, sound) 1.2-1.7 s. In copy mode
(`?lazy=0`) the first frame comes 0.8 s sooner and the 156 MB core stays
in memory. Before the read-ahead list recorded every block read (cache hits
and map packs too), a warm start made 55-87 synchronous reads and was
slower than the old copy (26 s at 4x CPU); it now makes none.


### The slim data set on the web

The `astc` profile is the phone set as the apps stream it. Two things made
it usable in the browser:

- **Opus.** `touch_assets_opus.c` decodes Opus with libopusfile, linked on
  Android and loaded with `dlopen` elsewhere; WebAssembly has no `dlopen`.
  `scripts/build-web.sh` now builds libopus 1.5.2 and opusfile 0.12 with
  emcc (pinned tarballs, sha256-checked, kept in `XT_WEB_DEPS`, on Vercel
  in `.vercel/cache`) and links them statically: +134 KB of wasm (5,045,951
  bytes before, 5,179,824 after, which includes the maps hook). Decoding in
  the browser's `AudioDecoder`/`decodeAudioData` instead would be
  asynchronous behind a synchronous sound loader, and WebCodecs Opus is
  missing in older Safari; the library is small and decodes the same way as
  on the phones.
- **ASTC.** The KTX loader (`touch_assets_ktx.h`) also takes WebGL's
  `WEBGL_compressed_texture_astc`, which Emscripten lists as
  `GL_WEBGL_compressed_texture_astc` and enables with every other
  extension. ETC2 is not needed: every GPU with ASTC gets the slim set,
  desktops have S3TC for the DXT textures, and the rest (old phones with
  neither) decode DXT in software at reduced resolution, as before.

The page checks `WEBGL_compressed_texture_astc` in its GPU probe. The slim
set's textures are ASTC only (DXT was kept only where ASTC came out
bigger), so a GPU without ASTC must not get it: the profile is offered only
where the extension is there.

## An existing install

The wish: a device that already has Godnotic (the Android app, the Flatpak)
should not download the game data a second time for the browser, and
nobody should have to pick a folder. What the web platform allows, checked
against the documents linked here on 2026-11-14:

| Route | Possible? | What was done |
|---|---|---|
| The page finds the install's files by itself | **No.** A page cannot list or read the device's files, or another app's storage. The only way to files outside its own storage is `showDirectoryPicker()`, which "requires transient user activation" (a click) and always shows the browser's folder picker ([MDN](https://developer.mozilla.org/en-US/docs/Web/API/Window/showDirectoryPicker)). It exists in Chrome and Edge 86+ on desktop and Chrome for Android 132+, not in Firefox or Safari ([compat data](https://github.com/mdn/browser-compat-data/blob/main/api/Window.json)) | nothing to do |
| Reading the Android app's data folder with the picker | **No.** The app keeps its packs in its private storage, which Android gives to no other app, and the system picker that Chrome for Android opens does not show app-private directories. (No Chrome or Android document says this in one sentence; it follows from Android's app sandbox, and is not tested on a device here.) | nothing to do |
| The browser tells the page the app is installed | **Chrome on Android only**: `navigator.getInstalledRelatedApps()` (Chrome for Android 80+), when the web manifest lists the app in `related_applications` and the app names the site in an `asset_statements` entry of its manifest ([Chrome docs](https://developer.chrome.com/docs/capabilities/get-installed-related-apps)). Nothing there requires the app to come from Google Play, but sideloaded installs are not verified here | done: web side (`manifest.webmanifest`, `shell.js`), app side (`AndroidManifest.xml`, `res/values/asset_statements.xml`) |
| Handing over to the app (no pick, no download) | **Yes, with one tap.** A link to a URL the app handles opens it. Chrome for Android takes `intent://…#Intent;scheme=…;package=…;S.browser_fallback_url=…;end` from a tap, not from a typed URL or a script without a gesture ([Chrome docs](https://developer.chrome.com/docs/android/intents)); other browsers and Linux take the custom scheme, `godnotic://open` | done: the first-visit page offers "Open the Godnotic app" (see below); the app declares the `godnotic` scheme; the Flatpak's desktop file already registered `x-scheme-handler/godnotic` |
| Links to the web game open the app (Android App Links) | **Yes, on one host.** The app's intent filter with `autoVerify` plus `/.well-known/assetlinks.json` at the root of the host, naming the package and its signing certificate, served as JSON without redirects ([Android docs](https://developer.android.com/training/app-links/configure-assetlinks)). That works for `godnotic.vercel.app`; it cannot for the GitHub Pages copy, whose host root (`dixonsolutions.github.io/.well-known/`) belongs to another repository. A verified link opens the app from other apps and from links tapped on other sites; a URL typed into the browser stays in the browser | done: `web/.well-known/assetlinks.json` (copied by `scripts/build-web.sh`), the intent filter for `https://godnotic.vercel.app/` |
| Reusing a desktop install's packs after one pick | **Yes, in desktop Chromium.** One `showDirectoryPicker()`, the handle kept in IndexedDB; on later visits the browser asks again or, from Chrome 122, offers "Allow on every visit" ([Chrome blog](https://developer.chrome.com/blog/persistent-permissions-for-the-file-system-access-api)). `requestPermission()` needs a click | done: "Use an existing Godnotic folder" in the Storage popover (below) |
| The running app serves its packs to the page on loopback | Technically yes, not worth it (below) | not done |

**The signing certificate** in `assetlinks.json` is the one the release APKs
carry: SHA-256 `DB:CE:85:DC:DE:04:16:1C:E5:77:08:6B:B2:2E:03:82:AC:20:3B:D2:0E:47:45:84:C1:2D:DC:06:B0:31:5A:5B`
(subject `CN=dixonSolutions, OU=Android, O=dixonSolutions, C=US`), read from
the APK signing block of `Godnotic-1.2.189-arm64-v8a.apk`, `-x86_64.apk`,
the live build of 1.2.189 for arm64 and `Godnotic-1.2.175-arm64-v8a.apk`
(all the same). It is the
keystore in the repository secret `ANDROID_KEYSTORE_BASE64`; a build signed
with the throwaway key (`scripts/android-build.sh` without a keystore) is
not covered. A new keystore means a new fingerprint in that file.

### What the page does

A returning visitor with the data in the browser never sees any of this:
the game starts. On a first visit, the download page:

- asks `getInstalledRelatedApps()` where it exists. When the Godnotic app
  is reported: "Godnotic is installed on this device, with the game data
  and your settings: no download needed.", the button "Open the Godnotic
  app" as the suggested action, and Download next to it.
- on Android and Linux where the browser cannot tell (Firefox, Chrome
  before the app update with `asset_statements`, every Linux browser): a
  plain link "Have the Godnotic app? Open it". The page does not claim the
  app is there. On Chrome for Android the link is an intent URL naming the
  package, with this page plus `?noapp=1` as its fallback, so a device
  without the app comes back and reads "The Godnotic app is not installed
  on this device."; a `godnotic:` link with no handler does nothing
  visible in most browsers.
- elsewhere (Windows, macOS, iOS): nothing, there is no app.

The app opens as from its launcher (the link carries nothing yet); the
Linux launcher already understands `godnotic://join/HOST:PORT`.

### An existing folder (desktop Chromium)

Storage popover, "Use an existing Godnotic folder" (shown where
`showDirectoryPicker` exists). The player picks a folder once; the page
looks up to four levels below it for the shallowest folder with a
`xonotic-…-data.pk3` and a `xonotic-…-maps.pk3`, and takes from it the
stock packs it knows (data, maps, music, xoncompat, the fonts; the newest
of each), nothing else. It refuses, with the reason, a folder without
them, a pack that is not a whole zip (still downloading in the app), the
phone set (ASTC textures) on a GPU without ASTC, and a browser that cannot
read files in place. Then:

- those packs are read in place from the folder (`web/lazyfs.js`, the same
  reads as from OPFS), read only; the handle is asked for with
  `mode: 'read'`;
- this build's own files (game code, configs, about 15 MB) still come
  from the site into OPFS, with no question: the page's engine needs its
  own game code, not the app's;
- a `config.cfg` next to the packs is taken over as the browser game's
  settings (asked first when the browser game already has settings).
  "Import settings" does the same for a `config.cfg` picked by itself, in
  every browser;
- on later visits the handle comes from IndexedDB. Permission still
  granted: straight into the game, no data requests. Not granted: when the
  browser's own storage has everything, that is used; otherwise the
  download page offers "Use the folder “…”" (one click, the browser's
  permission prompt) next to Download;
- "Stop using the folder" forgets the handle.

Where the packs are on Linux (from `packaging/start.sh` and the Flatpak
manifest):

| Install | Packs | Settings | Pickable in Chromium? |
|---|---|---|---|
| Flatpak | `~/.var/app/io.github.dixonSolutions.Godnotic/data/godnotic/data/` | `~/.xonotic/data/config.cfg` | Yes: `~/.var` is not on Chromium's block list. It is hidden, so the path has to be typed (Ctrl+L in the GTK picker) |
| Local package (no Flatpak) | `~/.local/share/godnotic/data/` | `~/.xonotic/data/config.cfg` | **No**: Chromium refuses `~/.local/share` and everything below it, like `~/.config`, `~/.ssh` and the home folder itself ([source](https://github.com/chromium/chromium/blob/main/chrome/browser/file_system_access/chrome_file_system_access_permission_context.cc), `kBlockPaths`). A copy or a symlinked folder elsewhere works |
| Stock Xonotic | its `data/` folder (`~/.var/app/org.xonotic.Xonotic/…`, `/usr/share/xonotic/data`, an unpacked zip) | `~/.xonotic/data/config.cfg` | as its location allows; `/usr/share` works |
| Ubuntu Touch, Android | app-confined storage | | no picker reaches it |

The settings live in another tree than the Flatpak's packs, and the home
folder itself cannot be picked, so one pick cannot cover both: "Import
settings" is the way for `~/.xonotic/data/config.cfg`.

Limits, plainly: this is a convenience for desktop Chromium, not the
"find it without asking" that was wished for, which no browser offers.
The picker cannot be driven in a headless browser, so the automated test
answers `showDirectoryPicker()` with a directory handle it fills itself
(an OPFS directory: the same interface); the picker window, Chromium's
permission prompts and "Allow on every visit" are checked by hand only.
A pack that the app replaces while the page is open fails its reads
(`EIO` to the engine) until a reload.

### The app serving its packs on loopback: evaluated, not done

The Android or Linux app could answer `http://127.0.0.1:<port>/` with its
packs, and the page could fetch them without any pick.

- Mixed content does not forbid it: loopback addresses are "potentially
  trustworthy" ([Secure Contexts](https://w3c.github.io/webappsec-secure-contexts/)),
  and Chrome and Firefox allow `fetch('http://127.0.0.1…')` from an https
  page (Safari's behaviour is not confirmed).
- Chrome 142 put it behind a permission prompt of its own, Local Network
  Access, for every request from a public site to a local or loopback
  address ([Chrome blog](https://developer.chrome.com/blog/local-network-access)):
  so it is not "without asking" either, and the prompt ("look for and
  connect to devices on your local network") reads worse than a folder
  picker.
- It only works while the app runs, which on Android means a foreground
  service serving half a gigabyte to a browser tab that is about to
  compete with it for memory; and the device then holds the data twice
  anyway, because the page has to keep what it fetched (in OPFS) to start
  without the app next time.
- It is a file server in the app: it would need loopback-only binding, a
  fixed read-only list of packs, an `Origin` check and a port the page can
  guess, and every one of those is a place to get security wrong.

The hand-over to the app gives the player the installed game with no
download at all, which is what this route would be for.

## The shell

What shows before and around the canvas (web/index.html, shell.css,
shell.js) looks and behaves like a libadwaita app, the Linux Godnotic app's
toolkit: Adwaita's named colours as CSS custom properties (light and dark
from `prefers-color-scheme`, the system accent through `AccentColor` where
the browser has it), a header bar with the menu button, a clamp, status
pages for the download, progress and errors, pill and suggested-action
buttons, the Adwaita progress bar, toasts, a popover with boxed lists and a
switch, visible focus rings, 44 px targets on touch screens, and no motion
under `prefers-reduced-motion`. Plain HTML and CSS, no framework. The
banner is the hero, on its own black card.

- **Returning visitors** see the banner on black and then the game: the
  start-up steps show their status page only after 0.6 s, and the game
  starts without a Play button (fullscreen and sound begin at the first tap
  or key, which browsers require).
- **First visit**: one status page, one size, one Download button; the free
  space the browser offers under it; then the progress page (Pause,
  Cancel); then the game.
- **Storage popover** (menu button; `web_storage` in the console over the
  game, which frees the mouse): Stored in (OPFS, cache storage, or memory
  only with the reason, and the folder when one is used), Game data
  (stored of total, which set), Space (`estimate()`), the switch Keep game
  data on this device (`persist()` on the click, `persisted()` shown), Last
  problem; Install app, Verify data, Clear data (with a confirmation;
  refused while another tab of the site is open, whose game it would
  break), Use an existing Godnotic folder / Stop using the folder, Import
  settings (a `config.cfg`); render resolution and fullscreen.
  Closes on Escape or a tap outside it; taps on it never grab the mouse.
- **Problems in one line**, on the download page, in the popover and in the
  console: "The browser closed this page while downloading (what had
  arrived is kept; it carries on from there), most likely to free memory.",
  "39 MB of the game data is gone from this browser: the browser removed
  it …",
  "Not enough space: this needs 405 MB and the browser offers 300 MB …",
  "Nothing can be stored in this window …", "An update: 3 MB changed."
- **Breadcrumbs**: each start writes its phase to localStorage (download
  with pieces saved, mount, engine, running, quit), and "closed" on
  `pagehide`. A phase without "closed" at the next load means the browser
  killed the page; that is what the first line above says. Not every such
  breadcrumb is a kill: each page holds a Web Lock named after itself, so a
  breadcrumb whose page still holds its lock is another tab at work, and
  one left in the download phase says nothing once the data is complete.
  A second tab that has to wait for the first one's download says so.

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
curl -L -o slim.zip https://github.com/dixonSolutions/Godnotic-data/releases/download/data-slim/Godnotic-data.zip
scripts/stage-web-data.sh --zip data.zip --astc slim.zip
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
- `?profile=web|astc|full` picks the data set; `?lazy=0` copies the data
  into memory instead of reading it in place (the fallback path);
  `?allmaps=0|1` turns the background map download off or on for a visit.
- `xt.lazyStats()` in the devtools console: synchronous reads, their time,
  read-ahead, cache size; `xt.backgroundStatus()`, `xt.maps`.

Tests: `scripts/test-web-cache.sh` runs `tests/web-cache/cache.test.mjs` (the
store and the loader flow in node, against an in-memory OPFS, Cache API and
a data host with Range: first visit, warm start with zero requests, small and
large updates, pause and resume, reload mid-chunk, damaged chunks, adopting
the older layouts, prune safety; the persist switch, data removed by the
browser, a new deploy with the same packs, the installed-app offer, the
checks on a picked folder; maps on demand, a cancelled map download,
the background download with pause and resume, a map asked for during it,
the profile by GPU, a returning visitor keeping the full set, and one whose
full set is incomplete) and `scripts/test-web-cache-repro.sh` (staging and
the map split are reproducible; where each traced file goes). CI
runs both on every push and pull request that touches the web loader.
`tests/web-cache/browser.sh` runs the store in real browsers (headless
Chromium and Firefox), killing and restarting them between steps: a
download into OPFS cut off and resumed, a restart, the worker writer, an
evicted and a damaged chunk, `persist()`; with `--site build/web` it also
drives the page in Chromium: first visit without `persist()`, a restart and
a new deploy with no data requests, one chunk evicted or damaged (one
request each), the switch denied and granted, the installed-app offer, and
with `--install DIR` an existing folder (see tests/web-cache/README.md).
`tests/web-cache/e2e.sh PROFILE URL LABEL` drives a real headless GPU Chrome
through a visit (clicks Download, waits for the menu) and prints times,
heap (V8 heap, ArrayBuffer backing stores, the renderer's private memory)
and every data request; run it twice on one profile for a reload. With
`--map stormkeep --map2 dance --expect-fetch` (and `?autoplay&allmaps=0` in
the URL) it then plays a bot match and changes to a map that has to be
downloaded on demand, and fails if it was not; `--throttle-bps`,
`--settle SEC` measure slow connections and memory after a load.
- `xt.command('...')` in the devtools console runs a console command.
- `scripts/build-web.sh --debug` keeps function names and Emscripten
  assertions.

## Deploying

**Vercel** (page and game data) builds itself from Git. The project imports
this repository; `vercel.json` sets the commands, so no dashboard settings
are needed:

- `ignoreCommand`: `main` and `work/web*` (loader previews) are built;
  other branches and pull requests are skipped (each build stages 1.5 GB of
  data).
- `installCommand` (`scripts/vercel-build.sh --install`): build tools,
  Python 3.10+ and GCC 12+ from `dnf` where the build image lacks them, and
  the Emscripten SDK, kept in `.vercel/cache` between builds.
- `buildCommand` (`scripts/vercel-build.sh`): `scripts/build-web.sh` (and
  libopus, cached), then the stock data from the pinned Godnotic data
  release (`data-20261107`; move the pin to change the data) and the slim
  set (`data-slim`, sha256-pinned: the build fails when it cannot be
  fetched or is not the pinned file, rather than deploying without the
  ASTC profile or with other data), split and staged with `scripts/stage-web-data.sh` (the split
  takes seconds). Output: about 1.46 GB of blobs (web 1.07 GB, astc 0.40 GB,
  the full set's shared chunks counted once, its maps pk3 not hosted).
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

Bandwidth: a new player downloads 159-444 MB to play, about 1.07 GB (web)
or 0.40 GB (astc) once every map and the music are in. Before maps on
demand, each new player downloaded about 1 GB. Vercel's Hobby plan includes
100 GB of transfer a month (about 100 players who download everything,
300-600 who play a few maps), and is for non-commercial
use. Pro, or another host for the stock data (any static host that sends
`Access-Control-Allow-Origin: *`), raises that; point the Pages shell at it
with the repository variable `XT_WEB_DATA_URL`.

**Data releases the build consumes** (`Godnotic-data`; the build never
creates them):

| Asset | Used as | How it is built |
|---|---|---|
| `data-20261107/Godnotic-data-20261107.zip` | the stock set: `web` today, and `full` | the upstream pk3s |
| `data-slim/Godnotic-data.zip` (sha256 pinned in `scripts/vercel-build.sh`) | `astc` | `repack-phone-assets.py --profile slim` (ASTC) |
| a web slim set, e.g. `data-web-20261107/Godnotic-data-web.zip` with its `.sha256` | `web`, once `XT_WEB_DATA_ARCHIVE`/`XT_WEB_DATA_SHA256` (or the defaults in `scripts/vercel-build.sh`) name it | `python3.14 scripts/repack-phone-assets.py --profile slim --textures keep -j $(nproc) -o out Xonotic/data/{font-unifont,font-xolonium}-20261107.pk3 Xonotic/data/xonotic-20261107-{data,maps,music,xoncompat}.pk3`, then each `out/<name>-phone.pk3` zipped as `Xonotic/data/<name>.pk3` (the layout of the other archives); 878 MB measured |

The zip must keep the stock file names under `Xonotic/data/`: the stager
finds the data and maps pk3 by `xonotic-*-data.pk3` / `xonotic-*-maps.pk3`
and splits the maps pk3 itself. `data-slim` is a tag that moves: the build
is pinned to the asset's hash and fails when it changes. The slim set
should be published under an immutable tag (`data-slim-<stock date>-<n>`,
for example `data-slim-20261107-1`, asset `Godnotic-data.zip` with its
`.sha256`), and `SLIM_URL`/`SLIM_SHA256` in `scripts/vercel-build.sh`
moved to it on purpose.

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
