# Xonotic Touch in the browser

The same engine and game code as the Android, Flatpak and Click builds,
compiled to WebAssembly with Emscripten and drawn with WebGL 2. Touch
controls and keyboard and mouse work; gamepads go through the browser's
Gamepad API (not tested yet). Local games against bots run entirely in the
page.

- Vercel: https://xonotic-web.vercel.app (page and game data)
- GitHub Pages: https://dixonSolutions.github.io/Xonotic-Touch/web/ (page;
  game data comes from the Vercel deployment)
- Every GitHub release carries `xonotic-touch-web-<version>.zip`: the page and
  engine without game data, for hosting elsewhere

## What the player gets

| | |
|---|---|
| First visit | About 1 GB of game data downloads once (965 MB, plus 111 MB of optional music) |
| Later visits | Starts from the browser's own storage, no download |
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
  bots work.
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
web/manifest.webmanifest      installable app: fullscreen, landscape
web/vercel.json               headers for the Vercel deployment
web/serve.py                  local dev server
engine/darkplaces/sys_wasm.c  engine side of the shim (Module.xt, xt_web_*)
scripts/build-web.sh          engine + QuakeC + page -> build/web
scripts/stage-web-data.sh     game data -> build/web/data + manifest.json
.github/workflows/web.yml     build, release asset, Vercel, GitHub Pages
```

`shell.js` is what an OS would otherwise provide:

- **Game data.** `data/manifest.json` lists every file with a content hash.
  Files go to the origin private file system (OPFS), streamed to disk as they
  download; browsers without writable OPFS get the Cache API in 32 MB pieces.
  Files over 40 MB are stored on the server in parts (static hosts cap single
  files at 100 MB), each part retried on failure. A file is downloaded again
  only when its hash changes. At startup the files are handed to the engine's
  in-memory filesystem without a second copy.
- **User data.** The engine's userdir is IndexedDB-backed (IDBFS), synced a
  moment after each write and when the tab is hidden or closed.
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
- Dynamic vertex data through `glBufferSubData`: WebGL has no buffer mapping,
  and Emscripten rejects the unsynchronized `glMapBufferRange` the Android build
  uses (an error per draw call).
- S3TC detected under WebGL's extension name, fatal errors shown on the page,
  and the display, mouse and keyboard hooks above.

The page itself also answers the engine's stdin with end-of-file: Emscripten's
default stdin in a browser is `window.prompt()`, a modal "Input" dialog that
the engine would open every frame.

## Build and run locally

```bash
git clone --depth 1 https://github.com/emscripten-core/emsdk.git ~/.cache/emsdk
~/.cache/emsdk/emsdk install 7.0.0 && ~/.cache/emsdk/emsdk activate 7.0.0
scripts/build-web.sh
```

Game data comes from the stock Xonotic release (the archive the Android build
downloads):

```bash
curl -L --user xonotic:g-23 -o Xonotic-latest.zip https://beta.xonotic.org/autobuild/Xonotic-latest.zip
scripts/stage-web-data.sh --zip Xonotic-latest.zip
python3 web/serve.py build/web
```

Open http://localhost:8080. `localhost` counts as a secure origin, which the
storage, pointer lock and fullscreen APIs need; anything else needs https.

Debugging:

- `?debug` prints the engine console to the browser console.
- `?logsink` sends every console line to `serve.py`, which prints it. This
  survives a page that freezes, and is the easy way to read a phone's console.
- `?args=+map%20stormkeep` adds engine arguments; `?autoplay` skips the Play
  button (no fullscreen or audio until the first click).
- `xt.command('...')` in the devtools console runs a console command.
- `scripts/build-web.sh --debug` keeps function names and Emscripten
  assertions.

## Deploying

`.github/workflows/web.yml` runs after every successful "Build and publish"
run (each push to `main`), or by hand from the Actions tab.

**Vercel** (page and data). One-time setup:

1. Create a Vercel project named `xonotic-web` (any framework preset; the
   workflow uploads a ready-made static site).
2. Create a token at https://vercel.com/account/tokens.
3. Add the repository secrets `VERCEL_TOKEN`, `VERCEL_ORG_ID` and
   `VERCEL_PROJECT_ID` (the IDs are in the project's settings, or in
   `.vercel/project.json` after `vercel link`).

Without the token the Vercel step is skipped and the rest still runs.

Bandwidth: each new player downloads about 1 GB. Vercel's Hobby plan includes
100 GB of transfer a month, about 100 new players. Pro, or another host for
`data/` (any static host that sends `Access-Control-Allow-Origin: *`), raises
that. Point the page at it with the repository variable `XT_WEB_DATA_URL`.

**GitHub Pages** (page only). Pages serves one site per repository, already
used by the Flatpak and Click remotes, and caps it at 1 GB. The workflow takes
the site that "Build and publish" deployed, adds `web/`, and deploys it again.
The page there loads its data from `XT_WEB_DATA_URL` (default
`https://xonotic-web.vercel.app/data/`).
