// Godnotic in the browser: data loader and platform shim.
//
// The engine is DarkPlaces built with Emscripten (scripts/build-web.sh). This
// file is the "platform layer" that a phone or desktop OS would otherwise be:
//
//   data        downloads the game data once, in hash-checked 32 MiB chunks,
//               into OPFS or the Cache API (store.js), then hands the files
//               to the engine's MEMFS
//   saves       mirrors the userdir (config.cfg, layouts) to IndexedDB
//   display     canvas fills the page; render resolution = CSS size × scale
//   GPU         WebGL 2 context forced opaque and high-performance; reports
//               when the browser is rendering without hardware acceleration
//   fullscreen  page fullscreen + landscape lock + keyboard lock (so Esc,
//               Tab and Ctrl+key reach the game); vid_fullscreen drives it
//   mouse       pointer lock with raw (unaccelerated) motion where offered;
//               a lock the browser drops (Esc) opens the game menu
//   keyboard    hidden text field raises phone keyboards for chat/console
//   audio       resumes Web Audio on the first gesture, pauses in background
//   power       screen wake lock while playing
//
// The engine calls into Module.xt (see engine/darkplaces/sys_wasm.c); the
// page calls the engine through the exported xt_web_* functions.
'use strict';

(() => {
  const $ = (id) => document.getElementById(id);
  const canvas = $('canvas');
  const params = new URLSearchParams(location.search);
  const DEBUG = params.has('debug');
  // ?logsink: send every engine line to the dev server (web/serve.py writes
  // them to its log). Beacons leave even if the page freezes right after, and
  // it is the easy way to read the console of a phone.
  const LOGSINK = params.has('logsink');
  const sink = (text) => { try { navigator.sendBeacon('/__log', text); } catch (e) { /* ignore */ } };
  const USERDIR = '/xt-user';
  const BASEDIR = '/game';

  const store = {
    get(k, d) { try { const v = localStorage.getItem('xt.' + k); return v === null ? d : v; } catch (e) { return d; } },
    set(k, v) { try { localStorage.setItem('xt.' + k, String(v)); } catch (e) { /* private mode */ } },
  };

  const mq = (q) => { try { return matchMedia(q).matches; } catch (e) { return false; } };
  const hasTouch = (navigator.maxTouchPoints || 0) > 0 || mq('(any-pointer: coarse)');
  const touchOnly = hasTouch && !mq('(any-pointer: fine)');

  const state = {
    started: false,
    downloadController: null,
    wantLock: false,
    fsPending: false,
    kbdOn: false,
    wakeLock: null,
    downloading: false,
    timing: {},
  };
  // Build config (index.html, written by scripts/build-web.sh); config.js
  // from older builds still counts.
  const CONFIG = Object.assign({ engine: 'godnotic.js', wasm: 'godnotic.wasm' }, (() => {
    try { return JSON.parse($('xt-config').textContent); } catch (e) { return {}; }
  })(), window.XT_CONFIG || {});
  let data = null; // the game data store (store.js)

  // ------------------------------------------------------------------ UI

  const panels = ['xt-status', 'xt-download', 'xt-progress', 'xt-ready', 'xt-error'];
  function show(id) { for (const p of panels) $(p).hidden = p !== id; }
  function status(text, detail) {
    $('xt-status-text').textContent = text;
    $('xt-status-detail').textContent = detail || '';
    show('xt-status');
  }
  const logTail = [];
  function fail(text) {
    $('xt-error-text').textContent = text;
    show('xt-error');
    $('xt-overlay').classList.remove('xt-gone');
    if (document.fullscreenElement && document.exitFullscreen) document.exitFullscreen().catch(() => {});
  }
  let toastTimer = 0;
  function toast(text, ms = 3500) {
    const t = $('xt-toast');
    t.textContent = text;
    t.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { t.hidden = true; }, ms);
  }
  const mb = (n) => n >= 1e9 ? (n / 1e9).toFixed(2) + ' GB' : Math.round(n / 1e6) + ' MB';

  // ------------------------------------------------------- GPU / WebGL 2

  // DarkPlaces asks SDL for an RGBA framebuffer and does not keep alpha at 1,
  // so a default (premultiplied, alpha:true) WebGL canvas lets the page show
  // through translucent surfaces. Force an opaque, discrete-GPU context.
  const realGetContext = HTMLCanvasElement.prototype.getContext;
  HTMLCanvasElement.prototype.getContext = function (type, attrs) {
    if (this === canvas && (type === 'webgl2' || type === 'webgl')) {
      attrs = Object.assign({}, attrs, {
        alpha: false,
        premultipliedAlpha: false,
        powerPreference: 'high-performance',
        preserveDrawingBuffer: false,
        failIfMajorPerformanceCaveat: false,
        desynchronized: params.has('lowlatency'),
      });
    }
    return realGetContext.call(this, type, attrs);
  };

  function probeGPU() {
    const c = document.createElement('canvas');
    const gl = c.getContext('webgl2', { powerPreference: 'high-performance' });
    if (!gl) return null;
    let renderer = gl.getParameter(gl.RENDERER) || '';
    const dbg = gl.getExtension('WEBGL_debug_renderer_info');
    if (dbg) renderer = gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) || renderer;
    const info = {
      renderer: String(renderer),
      s3tc: !!gl.getExtension('WEBGL_compressed_texture_s3tc'),
      maxTexture: gl.getParameter(gl.MAX_TEXTURE_SIZE),
      software: /swiftshader|llvmpipe|softpipe|software|basic render|microsoft basic/i.test(String(renderer)),
    };
    const lose = gl.getExtension('WEBGL_lose_context');
    if (lose) lose.loseContext();
    return info;
  }

  canvas.addEventListener('webglcontextlost', (e) => {
    e.preventDefault();
    fail('The graphics driver was reset (WebGL context lost). Reload to continue: settings are saved.');
  });

  // ------------------------------------------------------ render size

  // Render pixels per CSS pixel. Phones get a lower default: a 3x screen at
  // full resolution is 9x the pixels for little visible gain.
  function renderScale() {
    const dpr = window.devicePixelRatio || 1;
    const pref = store.get('scale', 'auto');
    if (pref === 'auto') return Math.min(dpr, touchOnly ? 1.25 : 2);
    const v = parseFloat(pref);
    // Never above the screen's own pixels: that is only extra work.
    return Number.isFinite(v) ? Math.min(Math.max(v, 0.25), Math.max(dpr, 1)) : 1;
  }
  function renderSize() {
    const r = canvas.getBoundingClientRect();
    let w = r.width || window.innerWidth;
    let h = r.height || window.innerHeight;
    let s = renderScale();
    const maxPixels = 3840 * 2160;
    if (w * s * h * s > maxPixels) s = Math.sqrt(maxPixels / (w * h));
    return [Math.max(64, Math.round(w * s)), Math.max(64, Math.round(h * s))];
  }
  function refit() { if (state.started) Module._xt_web_refit(); }

  // Window moved to a screen with another pixel ratio.
  (function watchDpr() {
    const m = matchMedia(`(resolution: ${window.devicePixelRatio || 1}dppx)`);
    m.addEventListener('change', () => { refit(); watchDpr(); }, { once: true });
  })();

  // ------------------------------------------------------- fullscreen

  const inFullscreen = () => !!(document.fullscreenElement || document.webkitFullscreenElement);

  async function enterFullscreen() {
    const el = document.documentElement;
    const req = el.requestFullscreen || el.webkitRequestFullscreen;
    if (!req) return false; // iPhone Safari: only "Add to Home Screen" is fullscreen
    try {
      await req.call(el, { navigationUI: 'hide' });
    } catch (e) {
      return false;
    }
    return true;
  }
  function onFullscreenEntered() {
    if (hasTouch && screen.orientation && screen.orientation.lock) {
      screen.orientation.lock('landscape').catch(() => {});
    }
    // Chromium: in fullscreen, keys the browser would take (Esc, Tab,
    // Ctrl+W/T/N, Alt+Tab-like) go to the page instead; hold Esc to leave.
    if (!touchOnly && navigator.keyboard && navigator.keyboard.lock) {
      navigator.keyboard.lock().catch(() => {});
    }
  }
  function setFullscreen(on) {
    if (on && !inFullscreen()) {
      enterFullscreen().then((ok) => {
        // No user activation right now (vid_fullscreen from the console,
        // or the click is too old): do it on the next tap or key.
        if (!ok) state.fsPending = true;
      });
    } else if (!on && inFullscreen()) {
      state.fsPending = false;
      const r = (document.exitFullscreen || document.webkitExitFullscreen).call(document);
      if (r && r.catch) r.catch(() => {});
    } else if (!on) {
      state.fsPending = false;
    }
  }
  function onFullscreenChange() {
    const fs = inFullscreen();
    if (fs) onFullscreenEntered();
    if (state.started) {
      Module._xt_web_fullscreen_changed(fs ? 1 : 0);
      setTimeout(refit, 50); // some browsers settle the viewport late
    }
  }
  document.addEventListener('fullscreenchange', onFullscreenChange);
  document.addEventListener('webkitfullscreenchange', onFullscreenChange);

  // ---------------------------------------------------- pointer lock

  function requestLock() {
    if (document.pointerLockElement === canvas || !canvas.requestPointerLock) return;
    try {
      // Raw motion, without the OS pointer acceleration (Chromium).
      const p = canvas.requestPointerLock({ unadjustedMovement: true });
      if (p && p.catch) {
        p.catch(() => {
          try { const q = canvas.requestPointerLock(); if (q && q.catch) q.catch(() => {}); } catch (e) { /* no gesture yet */ }
        });
      }
    } catch (e) { /* retried on the next click */ }
  }
  function mouseGrab(on) {
    state.wantLock = on;
    if (on) requestLock();
    else if (document.pointerLockElement === canvas) document.exitPointerLock();
  }
  document.addEventListener('pointerlockchange', () => {
    // The engine still wants the mouse but the browser let go: the player
    // pressed Esc (browsers keep that key for themselves without keyboard
    // lock) or switched away. Do what Esc does in game: open the menu.
    if (!document.pointerLockElement && state.wantLock && state.started && !document.hidden) {
      Module.ccall('xt_web_command', null, ['string'], ['togglemenu 1']);
    }
  });

  // -------------------------------------------------- soft keyboard

  // Phone keyboards send composition and "Unidentified" key events that SDL
  // cannot turn into text. Keep a hidden field focused while the engine wants
  // text and forward what changes in it: added characters as text, removed
  // ones as Backspace. A sentinel character lets Backspace register on an
  // otherwise empty field.
  const kbd = $('xt-kbd');
  const SENTINEL = ' ';
  let kbdLast = SENTINEL;
  function kbdReset() { kbd.value = SENTINEL; kbdLast = SENTINEL; }
  function screenKeyboard(on) {
    state.kbdOn = on;
    if (on) {
      kbdReset();
      kbd.focus({ preventScroll: true });
    } else if (document.activeElement === kbd) {
      kbd.blur();
      canvas.focus({ preventScroll: true });
    }
  }
  kbd.addEventListener('input', () => {
    const now = Array.from(kbd.value);
    const was = Array.from(kbdLast);
    let p = 0;
    while (p < now.length && p < was.length && now[p] === was[p]) p++;
    for (let i = p; i < was.length; i++) Module._xt_web_key(8);
    const added = now.slice(p).join('').replace(/ /g, '');
    if (added) Module.ccall('xt_web_text', null, ['string'], [added]);
    if (!kbd.value.length) kbdReset(); else kbdLast = kbd.value;
  });
  for (const type of ['keydown', 'keyup', 'keypress']) {
    kbd.addEventListener(type, (e) => {
      // Keep SDL from seeing these as well: the field is the only path.
      e.stopPropagation();
      if (type !== 'keydown' || e.isComposing || e.keyCode === 229) return;
      const map = { Enter: 13, Escape: 27, Tab: 9 };
      if (map[e.key]) {
        e.preventDefault();
        Module._xt_web_key(map[e.key]);
        kbdReset();
      }
    });
  }
  kbd.addEventListener('blur', () => {
    // A tap on the game blurs the field; raise the keyboard again while the
    // engine still wants text.
    if (state.kbdOn) setTimeout(() => { if (state.kbdOn) kbd.focus({ preventScroll: true }); }, 0);
  });

  // ------------------------------------------------------------ audio

  const audioCtx = () => (window.Module && Module.SDL2 && Module.SDL2.audioContext) || null;
  function resumeAudio() {
    const c = audioCtx();
    if (c && c.state === 'suspended' && !document.hidden) c.resume().catch(() => {});
  }

  // ---------------------------------------------- gestures / lifecycle

  function onGesture() {
    if (!state.started) return;
    resumeAudio();
    if (state.fsPending) {
      state.fsPending = false;
      enterFullscreen();
    }
    if (state.wantLock && !touchOnly) requestLock();
  }
  window.addEventListener('pointerdown', onGesture, true);
  window.addEventListener('keydown', onGesture, true);
  window.addEventListener('touchend', onGesture, true);

  async function wakeLock() {
    if (!navigator.wakeLock || document.hidden || state.wakeLock) return;
    try {
      state.wakeLock = await navigator.wakeLock.request('screen');
      state.wakeLock.addEventListener('release', () => { state.wakeLock = null; });
    } catch (e) { /* battery saver or not allowed */ }
  }

  document.addEventListener('visibilitychange', () => {
    if (!state.started) return;
    if (document.hidden) {
      // The main loop stops with requestAnimationFrame; save now, it may be
      // the last chance (phones kill background tabs without warning).
      saveNow();
      const c = audioCtx();
      if (c && c.state === 'running') c.suspend().catch(() => {});
    } else {
      resumeAudio();
      wakeLock();
    }
  });
  window.addEventListener('pagehide', () => { if (state.started) saveNow(); });
  // Save on the way out, but never ask "Leave site?": settings and layouts
  // are already in IndexedDB, and the prompt fired on every reload.
  window.addEventListener('beforeunload', () => { if (state.started) saveNow(); });

  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
  document.addEventListener('gesturestart', (e) => e.preventDefault()); // iOS pinch zoom
  document.addEventListener('dblclick', (e) => e.preventDefault(), { passive: false });

  const portrait = matchMedia('(orientation: portrait)');
  function checkRotate() { $('xt-rotate').hidden = !(state.started && touchOnly && portrait.matches); }
  portrait.addEventListener('change', checkRotate);

  // ------------------------------------------------- saves (IndexedDB)

  let syncing = false;
  let syncAgain = false;
  let syncTimer = 0;
  function syncFS(immediate) {
    clearTimeout(syncTimer);
    if (!immediate) { syncTimer = setTimeout(() => syncFS(true), 800); return; }
    if (syncing) { syncAgain = true; return; }
    syncing = true;
    Module.FS.syncfs(false, (err) => {
      syncing = false;
      if (err) console.warn('Saving to IndexedDB failed:', err);
      if (syncAgain) { syncAgain = false; syncFS(true); }
    });
  }
  function saveNow() {
    try { Module._xt_web_save(); } catch (e) { /* engine not up */ }
    syncFS(true);
  }
  // Settings changed in the menu or console reach IndexedDB within a few
  // seconds: the engine rewrites config.cfg when an archived cvar or a
  // binding changed (at most every 2 s; a check takes about 0.1 ms),
  // and the sync starts at once. The saves on hide and
  // unload stay as a last resort; the IndexedDB write they start often does
  // not finish before the page is gone.
  function autosave() {
    if (!state.started || document.hidden) return;
    let saved = 0;
    try { saved = Module._xt_web_autosave(); } catch (e) { return; }
    if (saved) syncFS(true);
  }

  // ------------------------------------------------------------- data

  // Where the game data lives: ?data=URL, else the page's build config
  // (XT_WEB_DATA_URL in scripts/build-web.sh), else ./data/. The manifest is
  // the one the page was built with (scripts/stage-web-data.sh), named by its
  // own hash, so a warm start reads it from storage and asks the network
  // nothing. A page without one (served from web/, or the release zip) asks
  // data/manifest.json which one is current.
  const dataBase = new URL(params.get('data') || CONFIG.dataUrl || 'data/', location.href);
  const SMALL_UPDATE = 50e6;
  const manifestId = (p) => (/([0-9a-f]{8,64})\.json$/.exec(p || '') || [])[1];

  // A no-cache pointer ({manifest: "m/<id>.json"}). Offline, the last one
  // seen will do.
  async function pointer(base, name) {
    const url = new URL(name, base);
    const remember = 'pointer:' + url.href;
    try {
      const res = await fetch(url, { cache: 'no-cache' });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const j = await res.json();
      if (!j.manifest) throw new Error('the data there is from an older release');
      store.set(remember, j.manifest);
      return j.manifest;
    } catch (e) {
      const last = store.get(remember, '');
      if (last) return last;
      throw new Error(`The game data is not available right now (${url.host}: ${e.message}). Try again later.`);
    }
  }

  // Manifests are immutable: from storage when seen before, else fetched
  // and checked against the hash in their name.
  async function getManifest(base, path) {
    const id = manifestId(path);
    if (!id) throw new Error(`Bad manifest name: ${path}`);
    const kept = await data.loadManifest(id);
    if (kept) return { id, m: kept };
    const url = new URL(path, base);
    let res;
    try {
      res = await fetch(url);
    } catch (e) {
      throw new Error(`The game data is not available right now (${url.host}: ${e.message}). Try again later.`);
    }
    if (!res.ok) throw new Error(`The game data is not available right now (${url.host}: HTTP ${res.status}). Try again later.`);
    const text = await res.text();
    if (!(await XTStore.sha256(new TextEncoder().encode(text))).startsWith(id)) {
      throw new Error(`The game data list from ${url.host} is damaged. Try again later.`);
    }
    await data.saveManifest(id, text);
    return { id, m: JSON.parse(text) };
  }

  // A shell copy of the page (GitHub Pages) carries only this fork's own
  // files and names a "stock" host for the stock Xonotic pk3s, the way the
  // apps carry the game code and download the rest on first launch. That
  // host's stock.json says which stock manifest is current, so it can move
  // to newer data without the shell being republished.
  async function loadManifest() {
    const path = params.get('manifest') || CONFIG.manifest || await pointer(dataBase, 'manifest.json');
    const main = await getManifest(dataBase, path);
    const ids = [main.id];
    const files = main.m.files.map((f) => Object.assign({}, f, { base: dataBase.href }));
    if (main.m.stock) {
      const stockBase = new URL(params.get('stock') || main.m.stock, dataBase);
      const stock = await getManifest(stockBase, await pointer(stockBase, 'stock.json'));
      ids.push(stock.id);
      const have = new Set(files.map((f) => f.name));
      for (const f of stock.m.files) if (!have.has(f.name)) files.push(Object.assign({}, f, { base: stockBase.href }));
    }
    if (main.m.chunk) data.chunk = main.m.chunk;
    return { ids, files };
  }

  const blobUrl = (c) => new URL(`blobs/${c.h}.bin`, c.file.base || dataBase).href;
  const sum = (list) => list.reduce((n, c) => n + c.size, 0);
  const fmtTime = (s) => s > 5400 ? `${Math.round(s / 3600)} h` : s > 90 ? `${Math.round(s / 60)} min` : `${Math.max(1, Math.round(s))} s`;

  // Download the given chunks with the progress panel: percent, "x / y MB ·
  // speed · time left", Pause / Resume and Cancel. Pause and Cancel keep
  // what has arrived (whole chunks, and 4 MiB pieces of the rest). Resolves
  // when all are stored; rejects with AbortError on Cancel.
  function runDownload(list, label) {
    const total = sum(list);
    const got = new Map();
    let paused = false;
    let cancelled = false;
    let controller = null;
    let wake = null;
    const samples = [];
    let lastPaint = 0;
    let lastSaid = -1;
    const bytes = () => { let n = 0; for (const v of got.values()) n += v; return n; };
    const paint = (force) => {
      const now = performance.now();
      if (!force && now - lastPaint < 200) return;
      lastPaint = now;
      const all = bytes();
      samples.push([now, all]);
      while (samples.length > 2 && now - samples[0][0] > 8000) samples.shift();
      const [t0, b0] = samples[0];
      const rate = now - t0 > 500 ? Math.max(0, all - b0) / ((now - t0) / 1000) : 0;
      const pct = total ? Math.min(100, 100 * all / total) : 100;
      $('xt-percent').textContent = Math.floor(pct) + '%';
      $('xt-bar-fill').style.width = pct.toFixed(1) + '%';
      $('xt-bar').setAttribute('aria-valuenow', pct.toFixed(0));
      $('xt-bar').setAttribute('aria-valuetext', `${Math.floor(pct)}%, ${mbPair(all, total)}`);
      if (paused) {
        $('xt-progress-detail').textContent = `Stopped at ${mbPair(all, total)}. Resume carries on from there.`;
      } else {
        $('xt-progress-detail').textContent = `${mbPair(all, total)}` +
          (rate > 0 ? ` · ${(rate / 1e6).toFixed(1)} MB/s · ${fmtTime((total - all) / rate)} left` : '');
      }
      // Screen readers: every 10%, not every repaint.
      const step = Math.floor(pct / 10);
      if (step !== lastSaid) { lastSaid = step; $('xt-progress-live').textContent = `${step * 10}% downloaded`; }
    };
    const setPaused = (on) => {
      paused = on;
      $('xt-progress').classList.toggle('xt-paused', on);
      $('xt-progress-label').textContent = on ? 'Paused' : label;
      $('xt-dl-pause').textContent = on ? 'Resume' : 'Pause';
      paint(true);
    };
    $('xt-dl-pause').onclick = () => {
      if (!paused) { setPaused(true); if (controller) controller.abort(); } else { setPaused(false); if (wake) wake(); }
    };
    $('xt-dl-cancel').onclick = () => {
      cancelled = true;
      $('xt-dl-cancel').textContent = 'Cancelling…';
      if (controller) controller.abort();
      if (wake) wake();
    };
    state.downloading = true;
    $('xt-verify').disabled = true;
    $('xt-dl-cancel').textContent = 'Cancel';
    setPaused(false);
    show('xt-progress');
    $('xt-dl-pause').focus({ preventScroll: true });
    return (async () => {
      try {
        let todo = list;
        for (;;) {
          controller = new AbortController();
          state.downloadController = controller;
          try {
            await data.download(todo, {
              urlFor: blobUrl,
              signal: controller.signal,
              onBytes: (c, n) => { got.set(c.h, n); paint(); },
            });
            paint(true);
            return;
          } catch (e) {
            if (!(e && e.name === 'AbortError')) throw e;
            if (cancelled) throw e;
            if (paused) await new Promise((r) => { wake = r; });
            wake = null;
            if (cancelled) throw new DOMException('Download cancelled', 'AbortError');
            // Carry on with what is still missing.
            const still = [];
            for (const c of todo) if (await data.has(c)) got.set(c.h, c.size); else still.push(c);
            todo = still;
          }
        }
      } finally {
        state.downloading = false;
        state.downloadController = null;
        $('xt-verify').disabled = false;
      }
    })();
  }
  const mbPair = (a, b) => b >= 1e9 ? `${(a / 1e9).toFixed(2)} / ${(b / 1e9).toFixed(2)} GB` : `${Math.round(a / 1e6)} / ${Math.round(b / 1e6)} MB`;

  function cancelDownload() {
    if (state.downloadController) $('xt-dl-cancel').onclick();
  }

  function mkdirs(FS, path) {
    let cur = '';
    for (const part of path.split('/').slice(0, -1)) {
      if (!part) continue;
      cur += '/' + part;
      try { FS.mkdir(cur); } catch (e) { /* exists */ }
    }
  }

  // Into the engine's in-memory filesystem. This is the slow part of a
  // warm start: every pk3 is read from storage into the JS heap (docs/WEB.md,
  // "Start-up time"). canOwn: MEMFS keeps this buffer as the file.
  async function mountData(files) {
    const FS = Module.FS;
    const total = files.reduce((n, f) => n + f.size, 0);
    let done = 0;
    let i = 0;
    for (const f of files) {
      i++;
      status('Loading game data…', `${mbPair(done, total)} · file ${i} of ${files.length}`);
      const buf = await data.read(f);
      const path = `${BASEDIR}/${f.path}`;
      mkdirs(FS, path);
      FS.writeFile(path, buf, { canOwn: true });
      done += f.size;
    }
  }

  function mountUserdir() {
    const FS = Module.FS;
    try { FS.mkdir(USERDIR); } catch (e) { /* exists */ }
    FS.mount(Module.IDBFS, {}, USERDIR);
    return new Promise((resolve) => {
      FS.syncfs(true, (err) => {
        if (err) console.warn('Could not read saved settings:', err);
        resolve();
      });
    });
  }

  // ------------------------------------------- storage: persistence, install

  const ua = navigator.userAgent || '';
  const isIOS = /iPad|iPhone|iPod/.test(ua) || (/Macintosh/.test(ua) && (navigator.maxTouchPoints || 0) > 1);
  const isSafari = /^((?!chrome|chromium|crios|fxios|android|edg).)*safari/i.test(ua);
  const installed = () => mq('(display-mode: standalone)') || mq('(display-mode: fullscreen)') || navigator.standalone === true;
  let installPrompt = null;
  window.addEventListener('beforeinstallprompt', (e) => {
    e.preventDefault();
    installPrompt = e;
    $('xt-install').hidden = false;
    if (state.files) showStorage();
  });
  window.addEventListener('appinstalled', () => { installPrompt = null; $('xt-install').hidden = true; });
  $('xt-install').onclick = async () => {
    if (!installPrompt) return;
    installPrompt.prompt();
    try { await installPrompt.userChoice; } catch (e) { /* dismissed */ }
    installPrompt = null;
    $('xt-install').hidden = true;
  };

  async function persisted() {
    try { return !!(navigator.storage && navigator.storage.persisted && await navigator.storage.persisted()); } catch (e) { return false; }
  }
  // Best-effort storage is the browser's to clear under disk pressure; ask
  // for it to be kept once there is something worth keeping.
  async function requestPersist() {
    if (!navigator.storage || !navigator.storage.persist || await persisted()) return;
    try { await navigator.storage.persist(); } catch (e) { /* not offered */ }
  }

  // Say plainly what is stored, how much room there is, and whether the
  // browser may delete it.
  async function showStorage() {
    const files = state.files || [];
    const used = await data.storedBytes(files);
    let est = null;
    try { est = navigator.storage && navigator.storage.estimate ? await navigator.storage.estimate() : null; } catch (e) { /* none */ }
    const kept = await persisted();
    const parts = [`Game data: ${mb(used)} in this browser` + (est && est.quota ? ` (${mb(est.usage || 0)} used of ${mb(est.quota)} it allows this site).` : '.')];
    if (kept) {
      parts.push('It is kept until you delete it.');
    } else {
      let why = 'The browser may delete it if the device runs low on space';
      if (isSafari) why += ', and Safari deletes it after 7 days without a visit';
      why += '.';
      if (installed()) why += '';
      else if (installPrompt) why += ' Installing the app keeps it.';
      else if (isIOS) why += ' To keep it, add this page to the Home Screen (Share, then Add to Home Screen) and play from there.';
      else why += ' Installing this page as an app (the browser menu) keeps it.';
      parts.push(why);
    }
    $('xt-storage-text').textContent = parts.join(' ');
    $('xt-storage-text').classList.toggle('xt-warn', !kept);
    $('xt-storage').hidden = false;
  }

  async function freeSpaceLine(need) {
    let est = null;
    try { est = navigator.storage && navigator.storage.estimate ? await navigator.storage.estimate() : null; } catch (e) { /* none */ }
    const el = $('xt-dl-space');
    if (!est || !est.quota) { el.textContent = ''; return; }
    const free = est.quota - (est.usage || 0);
    const fits = free > need * 1.05;
    el.textContent = fits
      ? `${mb(free)} free for this site; this needs ${mb(need)}.`
      : `Not enough room: this needs ${mb(need)} and the browser offers ${mb(free)} here. Private windows and a nearly full disk shrink it.`;
    el.classList.toggle('xt-warn', !fits);
  }

  // ------------------------------------------------------- the engine

  let runtimeReady;
  const runtimePromise = new Promise((r) => { runtimeReady = r; });

  window.Module = {
    canvas,
    noInitialRun: true,
    // The engine polls stdin for console commands every frame. Emscripten's
    // default stdin in a browser is window.prompt(): a modal "Input" dialog
    // that blocks the page. There is no terminal here, so stdin is always
    // at end of file.
    stdin: () => null,
    xt: {
      renderSize, setFullscreen, mouseGrab, screenKeyboard, quit,
      syncFS: () => syncFS(false),
      fatal(title, text) {
        state.aborted = true;
        state.started = false;
        if (document.pointerLockElement) document.exitPointerLock();
        const tail = logTail.slice(-8).join('\n').replace(/\x1b\[[0-9;]*m/g, '');
        fail(`${title}\n${text}\n\n${tail}`);
      },
    },
    onExit(code) {
      if (state.aborted) return;
      state.started = false;
      if (code) fail(`The game exited with code ${code}.\n\n` + logTail.slice(-8).join('\n').replace(/\x1b\[[0-9;]*m/g, ''));
      else quit();
    },
    print(text) {
      if (LOGSINK) sink(text);
      logTail.push(text);
      if (logTail.length > 200) logTail.shift();
      if (DEBUG) console.log(text);
    },
    printErr(text) {
      if (LOGSINK) sink('[err] ' + text);
      logTail.push(text);
      if (logTail.length > 200) logTail.shift();
      console.warn(text);
    },
    // The wasm's name carries its hash in a built page (index.html config).
    locateFile: (path) => path === 'godnotic.wasm' ? CONFIG.wasm : path,
    onRuntimeInitialized: () => runtimeReady(),
    onAbort(what) {
      if (state.aborted) return;
      state.aborted = true;
      const tail = logTail.slice(-12).join('\n');
      fail(`The game stopped: ${what}\n\n${tail}`);
    },
  };

  // The engine shut down cleanly (menu Quit): offer a fresh start, which
  // is a reload, since the engine cannot be started twice in one page.
  function quit() {
    if (state.aborted || state.quit) return;
    state.quit = true;
    state.started = false;
    state.wantLock = false;
    state.kbdOn = false;
    if (document.pointerLockElement) document.exitPointerLock();
    if (inFullscreen()) setFullscreen(false);
    $('xt-overlay').classList.remove('xt-gone');
    status('The game has quit.');
    $('xt-again').hidden = false;
    $('xt-again').focus();
  }

  function loadEngine() {
    const s = document.createElement('script');
    s.src = CONFIG.engine;
    s.async = true;
    s.onerror = () => fail(`Could not load the game engine (${CONFIG.engine}).`);
    document.body.appendChild(s);
  }

  /**
   * The browser's preferred languages for the menu's first start ("pl_PL,pl,en_US"), most preferred first. The menu
   * picks the closest language it has a translation for; a language chosen in the game afterwards stands.
   */
  function browserLanguages() {
    const tags = (navigator.languages && navigator.languages.length ? navigator.languages : [navigator.language]) || [];
    return tags
      .filter(t => typeof t === 'string' && /^[A-Za-z]{2,3}(-[A-Za-z0-9]{2,8})*$/.test(t))
      .map(t => t.replace(/-/g, '_'))
      .join(',');
  }

  function engineArgs(fullscreen) {
    const [w, h] = renderSize();
    const args = [
      '-xonotic',
      '-basedir', BASEDIR,
      '-userdir', USERDIR + '/',
      '+vid_resizable', '1',
      '+vid_desktopfullscreen', '1',
      '+vid_fullscreen', fullscreen ? '1' : '0',
      '+vid_width', String(w),
      '+vid_height', String(h),
    ];
    if (state.sampleRate) args.push('+snd_speed', String(state.sampleRate));
    const languages = browserLanguages();
    if (languages) args.push('+set', '_menu_system_languages', languages);
    const extra = params.get('args');
    if (extra) args.push(...extra.split(' ').filter(Boolean));
    return args;
  }

  async function play() {
    $('xt-play').disabled = true;
    const fullscreen = $('xt-opt-fullscreen').checked;
    store.set('fullscreen', fullscreen ? 1 : 0);
    // Everything below runs inside the click, so the browser lets us go
    // fullscreen and start audio.
    if (fullscreen) await enterFullscreen();
    try {
      const AC = window.AudioContext || window.webkitAudioContext;
      if (AC) { const probe = new AC(); state.sampleRate = probe.sampleRate; probe.close(); }
    } catch (e) { /* no audio device */ }
    requestPersist().catch(() => {});

    $('xt-overlay').classList.add('xt-gone');
    canvas.focus({ preventScroll: true });
    state.started = true;
    checkRotate();
    wakeLock();
    try {
      Module.callMain(engineArgs(fullscreen));
    } catch (e) {
      if (e !== 'unwind' && !(e && e.name === 'ExitStatus')) {
        console.error(e);
        fail('The game failed to start: ' + (e && e.message || e));
        return;
      }
    }
    refit();
    resumeAudio();
    setInterval(autosave, 250);
  }

  // ---------------------------------------------------------- startup

  const mark = (name) => {
    state.timing[name] = Math.round(performance.now());
    try { performance.mark('xt-' + name); } catch (e) { /* old browser */ }
  };

  async function main() {
    if (typeof WebAssembly !== 'object') return fail('This browser cannot run WebAssembly.');
    const gpu = probeGPU();
    if (!gpu) return fail('This browser has no WebGL 2, which the game needs to draw. Update the browser, or turn on hardware acceleration in its settings.');
    if (!window.isSecureContext && !('caches' in window)) return fail('This page needs a secure connection (https or localhost) to store the game data.');
    if (!window.crypto || !crypto.subtle) return fail('This page needs a secure connection (https or localhost) to check the game data.');

    // The engine downloads and compiles while storage is checked (the page
    // preloads it from <head>).
    loadEngine();

    const diag = [`GPU: ${gpu.renderer || 'unknown'}`, gpu.s3tc ? 'compressed textures: yes' : 'compressed textures: no (decoded in software)'];
    if (navigator.gpu) diag.push('WebGPU present (the engine draws with WebGL 2)');
    if (CONFIG.build) diag.push(`build ${CONFIG.build}`);
    $('xt-diag').textContent = diag.join(' · ');
    if (gpu.software) toast('Your browser is drawing without the GPU. Turn on hardware acceleration for a playable frame rate.', 9000);

    status('Checking game data…');
    try {
      data = await XTStore.Store.open({ preferCache: params.get('store') === 'cache' });
    } catch (e) {
      return fail(e.message || String(e));
    }
    state.store = data.kind;
    let manifest;
    try { manifest = await loadManifest(); } catch (e) { return fail(e.message); }
    const files = manifest.files;
    state.files = files;
    mark('manifest');

    const music = files.filter((f) => f.group === 'music');
    const wantMusic = () => store.get('music', navigator.connection && navigator.connection.saveData ? '0' : '1') === '1';
    const selected = () => files.filter((f) => f.group !== 'music' || wantMusic());

    let missing = await data.missing(selected());
    // Downloads made by earlier versions of this page: turn them into chunks
    // once (each checked against its hash) instead of downloading again.
    if (missing.length) {
      try {
        const adopted = await data.adopt(files, (n, total) => status('Moving downloaded data to the new storage layout…', `${mbPair(n, total)} checked`));
        if (adopted) missing = await data.missing(selected());
      } catch (e) {
        console.warn('Adopting older downloads failed:', e);
      }
    }
    mark('checked');

    if (missing.length) {
      const allMissing = await data.missing(files);
      const chunkCount = new Set(files.flatMap((f) => f.chunks)).size;
      const firstInstall = allMissing.length === chunkCount;
      // A small update (new game code, a config): fetch it and go on. The
      // question is for first installs and big downloads.
      if (!firstInstall && sum(missing) <= SMALL_UPDATE && !params.has('ask')) {
        try {
          await runDownload(missing, 'Updating game data…');
        } catch (e) {
          if (!(e && e.name === 'AbortError')) toast(e.message || String(e), 8000);
        }
        missing = await data.missing(selected());
      }
      if (missing.length) {
        const musicSize = music.reduce((n, f) => n + f.size, 0);
        const musicMissing = (await data.missing(music)).length > 0;
        $('xt-dl-music').checked = wantMusic();
        $('xt-dl-music-size').textContent = mb(musicSize);
        $('xt-dl-music-row').hidden = !music.length || !musicMissing;
        const refresh = async () => {
          missing = await data.missing(selected());
          let partial = 0;
          for (const c of missing) partial += await data.resumable(c);
          const stored = await data.storedBytes(selected());
          const need = sum(missing) - partial;
          $('xt-dl-size').textContent = mb(need);
          $('xt-dl-title').textContent = firstInstall && !stored && !partial ? 'Download the game data'
            : (missing.every((c) => c.file.group === 'music') ? 'Download the music' : 'Update the game data');
          const already = stored + partial;
          $('xt-dl-resumed').hidden = !(already > 0 && firstInstall);
          $('xt-dl-resumed').textContent = `${mb(already)} is already downloaded; it carries on from there.`;
          $('xt-dl-go').textContent = partial > 0 ? 'Resume' : 'Download';
          await freeSpaceLine(need);
        };
        $('xt-dl-music').onchange = () => { store.set('music', $('xt-dl-music').checked ? 1 : 0); refresh(); };
        await refresh();
        while (missing.length) {
          show('xt-download');
          $('xt-dl-go').focus({ preventScroll: true });
          await new Promise((resolve) => { $('xt-dl-go').onclick = resolve; });
          try {
            await runDownload(missing, 'Downloading game assets…');
          } catch (e) {
            if (e && e.name === 'AbortError') { await refresh(); continue; }
            if (e && e.name === 'QuotaExceededError') return fail('The browser ran out of storage for the game data. Free some disk space, or leave out the music, then reload.');
            return fail(e.message || String(e));
          }
          // What the download just stored is no longer missing.
          missing = await data.missing(selected());
          await refresh();
        }
      }
      await requestPersist();
    }
    mark('data-ready');

    status('Starting the engine…');
    await runtimePromise;
    mark('engine-ready');
    try {
      await mountUserdir();
      await mountData(selected());
    } catch (e) {
      return fail(e.message || String(e));
    }
    mark('mounted');
    // Every file this start needs was there: drop what no page path that
    // started recently still lists (older releases' chunks, old layouts).
    data.prune(location.pathname, manifest.ids).then((r) => { state.pruned = r; }).catch(() => {});

    $('xt-opt-fullscreen').checked = store.get('fullscreen', '1') === '1';
    const fsSupported = !!(document.documentElement.requestFullscreen || document.documentElement.webkitRequestFullscreen);
    $('xt-opt-fullscreen-row').hidden = !fsSupported;
    const hints = [];
    if (!fsSupported && hasTouch) hints.push('For fullscreen, add this page to your Home Screen and open it from there.');
    if (!touchOnly) hints.push('Click the game to capture the mouse. Esc opens the menu.');
    $('xt-ready-hint').textContent = hints.join(' ');
    show('xt-ready');
    showStorage().catch(() => {});
    $('xt-play').disabled = false;
    $('xt-play').focus({ preventScroll: true });
    $('xt-play').onclick = play;
    mark('ready');
    if (params.has('autoplay')) play();
  }

  // Settings
  $('xt-opt-scale').value = store.get('scale', 'auto');
  $('xt-opt-scale').onchange = (e) => { store.set('scale', e.target.value); refit(); };
  $('xt-verify').onclick = async () => {
    if (!data || !state.files || state.downloading) return;
    const btn = $('xt-verify');
    btn.disabled = true;
    const out = $('xt-verify-text');
    try {
      const r = await data.verify(state.files, (n, total) => { out.textContent = `Checking ${n} of ${total} pieces…`; });
      out.textContent = r.bad
        ? `${r.bad} damaged ${r.bad === 1 ? 'piece was' : 'pieces were'} removed. Reload to download ${r.bad === 1 ? 'it' : 'them'} again.`
        : `All ${r.checked - r.missing} stored pieces are intact` + (r.missing ? ` (${r.missing} not downloaded).` : '.');
    } catch (e) {
      out.textContent = 'Checking failed: ' + (e.message || e);
    }
    btn.disabled = false;
  };
  $('xt-clear').onclick = async () => {
    if (!confirm('Delete the downloaded game data from this browser? Your settings stay. The next start downloads it again.')) return;
    try {
      const root = await navigator.storage.getDirectory();
      await root.removeEntry(XTStore.NAME, { recursive: true });
    } catch (e) { /* no OPFS */ }
    await caches.delete(XTStore.NAME).catch(() => {});
    location.reload();
  };

  // For the dev console and automated tests: xt.command('map stormkeep')
  window.xt = {
    command: (text) => Module.ccall('xt_web_command', null, ['string'], [text]),
    log: () => logTail.slice(),
    state,
  };

  main().catch((e) => fail(e && e.message || String(e)));
})();
