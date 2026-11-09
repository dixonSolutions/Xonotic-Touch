// Xonotic Touch in the browser: data loader and platform shim.
//
// The engine is DarkPlaces built with Emscripten (scripts/build-web.sh). This
// file is the "platform layer" that a phone or desktop OS would otherwise be:
//
//   data        downloads the pk3s once into the Cache API (streamed to disk,
//               not held in memory), then hands them to the engine's MEMFS
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
  const DATA_CACHE = 'xt-data-v1';
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
    wantLock: false,
    fsPending: false,
    kbdOn: false,
    wakeLock: null,
    buildId: 'dev',
  };

  // ------------------------------------------------------------------ UI

  const panels = ['xt-status', 'xt-download', 'xt-progress', 'xt-ready', 'xt-error'];
  function show(id) { for (const p of panels) $(p).hidden = p !== id; }
  function status(text) { $('xt-status-text').textContent = text; show('xt-status'); }
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

  // ------------------------------------------------------------- data

  // Where the pk3s live: ?data=URL, else config.js (written by
  // scripts/build-web.sh from XT_WEB_DATA_URL, so a GitHub Pages copy of the
  // page can use the data hosted next to the Vercel copy), else ./data/.
  const CONFIG = window.XT_CONFIG || {};
  const dataBase = new URL(params.get('data') || CONFIG.dataUrl || 'data/', location.href);

  async function fetchManifest(base) {
    const url = new URL('manifest.json', base);
    let res;
    try {
      res = await fetch(url, { cache: 'no-cache' });
    } catch (e) {
      throw new Error(`The game data is not available right now (${url.host}: ${e.message}). Try again later.`);
    }
    if (!res.ok) throw new Error(`The game data is not available right now (${url.host}: HTTP ${res.status}). Try again later.`);
    const m = await res.json();
    for (const f of m.files) f.base = base.href;
    return m;
  }

  // A shell copy of the page (GitHub Pages) carries only this fork's own
  // files and names a "stock" host for the stock Xonotic pk3s, the way the
  // apps carry the game code and download the rest on first launch. The
  // stock list is read from that host each visit, so it can move to a newer
  // upstream build without the shell being republished.
  async function loadManifest() {
    const m = await fetchManifest(dataBase);
    if (m.stock) {
      const stockBase = new URL(params.get('stock') || m.stock, dataBase);
      const remote = await fetchManifest(stockBase);
      const have = new Set(m.files.map((f) => f.name));
      for (const f of remote.files) if (f.stock && !have.has(f.name)) m.files.push(f);
    }
    return m;
  }

  // Where downloaded files live between visits.
  //
  // OPFS (the origin private file system) first: a file is written as a
  // stream and committed on close, so a 600 MB pk3 never sits in memory and
  // an interrupted download leaves nothing behind. Browsers without writable
  // OPFS get the Cache API, in 32 MB parts: Chromium rejects single Cache
  // entries somewhere above 200 MB.
  const fileId = (f) => `${f.rev}-${f.path.replace(/[^A-Za-z0-9._-]/g, '_')}`;

  async function openOPFS() {
    if (!navigator.storage || !navigator.storage.getDirectory) return null;
    if (!('createWritable' in (window.FileSystemFileHandle ? FileSystemFileHandle.prototype : {}))) return null;
    try {
      const root = await navigator.storage.getDirectory();
      const dir = await root.getDirectoryHandle(DATA_CACHE, { create: true });
      return {
        kind: 'opfs',
        async has(f) {
          try { return (await (await dir.getFileHandle(fileId(f))).getFile()).size === f.size; } catch (e) { return false; }
        },
        async put(f, stream) {
          const h = await dir.getFileHandle(fileId(f), { create: true });
          const w = await h.createWritable();
          try { await stream.pipeTo(w); } catch (e) { try { await w.abort(); } catch (e2) { /* gone */ } throw e; }
        },
        async get(f) { return new Uint8Array(await (await (await dir.getFileHandle(fileId(f))).getFile()).arrayBuffer()); },
        async drop(f) { try { await dir.removeEntry(fileId(f)); } catch (e) { /* gone */ } },
        async prune(keep) {
          for await (const name of dir.keys()) {
            if (/^[0-9a-f]{16}-/.test(name) && !keep.has(name)) await dir.removeEntry(name).catch(() => {});
          }
        },
      };
    } catch (e) {
      return null; // Firefox private windows have no OPFS
    }
  }

  async function openCacheStore() {
    const PART = 32 * 1024 * 1024;
    const store = await openStore();
    state.store = store.kind;
    const key = (f, i) => new URL(`${f.path}?rev=${f.rev}&part=${i}`, f.base || dataBase).href;
    const parts = (f) => Math.max(1, Math.ceil(f.size / PART));
    return {
      kind: 'cache',
      async has(f) {
        // Parts are written in order, so the last one says the file is whole.
        const r = await cache.match(key(f, parts(f) - 1));
        return !!r && r.headers.get('x-xt-size') === String(f.size);
      },
      async put(f, stream) {
        const reader = stream.getReader();
        let buf = new Uint8Array(Math.min(PART, f.size || PART));
        let fill = 0;
        let part = 0;
        const flush = async () => {
          await cache.put(key(f, part++), new Response(buf.slice(0, fill), {
            headers: { 'content-type': 'application/octet-stream', 'x-xt-size': String(f.size) },
          }));
          fill = 0;
        };
        for (;;) {
          const { done, value } = await reader.read();
          if (done) break;
          let off = 0;
          while (off < value.length) {
            const n = Math.min(buf.length - fill, value.length - off);
            buf.set(value.subarray(off, off + n), fill);
            fill += n;
            off += n;
            if (fill === buf.length) await flush();
          }
        }
        if (fill || part === 0) await flush();
      },
      async get(f) {
        const out = new Uint8Array(f.size);
        let off = 0;
        for (let i = 0; i < parts(f); i++) {
          const r = await cache.match(key(f, i));
          if (!r) throw new Error(`${f.name} is incomplete in the browser cache. Reload to download it again.`);
          const b = new Uint8Array(await r.arrayBuffer());
          out.set(b, off);
          off += b.length;
        }
        return out;
      },
      async drop(f) { for (let i = 0; i < parts(f); i++) await cache.delete(key(f, i)); },
      async prune(keepIds, manifest) {
        const keep = new Set();
        for (const f of manifest.files) for (let i = 0; i < parts(f); i++) keep.add(key(f, i));
        for (const req of await cache.keys()) if (!keep.has(req.url)) await cache.delete(req);
      },
    };
  }

  async function openStore() {
    const opfs = await openOPFS();
    if (!opfs) return openCacheStore();
    caches.delete(DATA_CACHE).catch(() => {}); // anything an older page left there
    return opfs;
  }

  async function missingFiles(store, files) {
    const missing = [];
    for (const f of files) if (!(await store.has(f))) missing.push(f);
    return missing;
  }

  // One file as one stream, whether it is stored whole or in parts. Each
  // part is retried a few times: a dropped connection on a phone should not
  // restart a 600 MB download from zero.
  function fileStream(f) {
    const parts = f.parts || [{ url: f.url, size: f.size }];
    let i = 0;
    let reader = null;
    async function open(part) {
      let lastErr;
      for (let attempt = 0; attempt < 4; attempt++) {
        try {
          const res = await fetch(new URL(part.url, f.base || dataBase), { cache: 'no-store' });
          if (!res.ok || !res.body) throw new Error(`HTTP ${res.status}`);
          return res.body.getReader();
        } catch (e) {
          lastErr = e;
          await new Promise((r) => setTimeout(r, 1000 * (attempt + 1)));
        }
      }
      throw new Error(`Download of ${f.name} failed: ${lastErr && lastErr.message || lastErr}`);
    }
    return new ReadableStream({
      async pull(ctl) {
        for (;;) {
          if (!reader) {
            if (i >= parts.length) { ctl.close(); return; }
            reader = await open(parts[i]);
          }
          const { done, value } = await reader.read();
          if (!done) { ctl.enqueue(value); return; }
          reader = null;
          i++;
        }
      },
      cancel() { if (reader) reader.cancel(); },
    });
  }

  async function download(store, files) {
    const total = files.reduce((n, f) => n + f.size, 0);
    let done = 0;
    const t0 = performance.now();
    show('xt-progress');
    for (const f of files) {
      $('xt-progress-label').textContent = `Downloading ${f.name}…`;
      const body = fileStream(f);
      let got = 0;
      let lastPaint = 0;
      const counted = body.pipeThrough(new TransformStream({
        transform(chunk, ctl) {
          got += chunk.byteLength;
          const now = performance.now();
          if (now - lastPaint > 150) {
            lastPaint = now;
            const all = done + got;
            const pct = total ? (100 * all / total) : 0;
            $('xt-bar-fill').style.width = pct.toFixed(1) + '%';
            $('xt-bar').setAttribute('aria-valuenow', pct.toFixed(0));
            const rate = all / Math.max(0.001, (now - t0) / 1000);
            const eta = rate > 0 ? Math.round((total - all) / rate) : 0;
            $('xt-progress-detail').textContent =
              `${mb(all)} of ${mb(total)} · ${(rate / 1e6).toFixed(1)} MB/s` + (eta > 1 ? ` · about ${eta > 90 ? Math.round(eta / 60) + ' min' : eta + ' s'} left` : '');
          }
          ctl.enqueue(chunk);
        },
      }));
      try {
        await store.put(f, counted);
      } catch (e) {
        await store.drop(f);
        if (e && e.name === 'QuotaExceededError') throw new Error('The browser ran out of storage for the game data. Free some disk space, or leave out the music.');
        throw new Error(`Saving ${f.name} failed: ${e && e.message || e}`);
      }
      if (got !== f.size) {
        await store.drop(f);
        throw new Error(`${f.name} arrived incomplete (${got} of ${f.size} bytes). Check your connection and try again.`);
      }
      done += f.size;
    }
  }

  function mkdirs(FS, path) {
    let cur = '';
    for (const part of path.split('/').slice(0, -1)) {
      if (!part) continue;
      cur += '/' + part;
      try { FS.mkdir(cur); } catch (e) { /* exists */ }
    }
  }

  async function mountData(store, files) {
    const FS = Module.FS;
    let i = 0;
    for (const f of files) {
      i++;
      status(`Loading game data (${i} of ${files.length})…`);
      const buf = await store.get(f);
      const path = `${BASEDIR}/${f.path}`;
      mkdirs(FS, path);
      // canOwn: MEMFS keeps this buffer as the file, no second copy.
      FS.writeFile(path, buf, { canOwn: true });
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
      renderSize, setFullscreen, mouseGrab, screenKeyboard,
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
      else { $('xt-overlay').classList.remove('xt-gone'); status('The game has quit. Reload the page to play again.'); }
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
    locateFile: (path) => path + '?v=' + encodeURIComponent(state.buildId),
    onRuntimeInitialized: () => runtimeReady(),
    onAbort(what) {
      if (state.aborted) return;
      state.aborted = true;
      const tail = logTail.slice(-12).join('\n');
      fail(`The game stopped: ${what}\n\n${tail}`);
    },
  };

  function loadEngine() {
    const s = document.createElement('script');
    s.src = 'xonotic-touch.js?v=' + encodeURIComponent(state.buildId);
    s.async = true;
    s.onerror = () => fail('Could not load the game engine (xonotic-touch.js).');
    document.body.appendChild(s);
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
    if (navigator.storage && navigator.storage.persist) navigator.storage.persist().catch(() => {});

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
  }

  // ---------------------------------------------------------- startup

  async function main() {
    if (typeof WebAssembly !== 'object') return fail('This browser cannot run WebAssembly.');
    const gpu = probeGPU();
    if (!gpu) return fail('This browser has no WebGL 2, which the game needs to draw. Update the browser, or turn on hardware acceleration in its settings.');
    if (!('caches' in window)) return fail('This page needs a secure connection (https or localhost) to store the game data.');

    try {
      const b = await fetch('build.json', { cache: 'no-cache' });
      if (b.ok) state.buildId = (await b.json()).build || 'dev';
    } catch (e) { /* offline: use cached engine */ }
    loadEngine();

    const diag = [`GPU: ${gpu.renderer || 'unknown'}`, gpu.s3tc ? 'compressed textures: yes' : 'compressed textures: no (decoded in software)'];
    if (navigator.gpu) diag.push('WebGPU present (the engine draws with WebGL 2)');
    $('xt-diag').textContent = diag.join(' · ');
    if (gpu.software) toast('Your browser is drawing without the GPU. Turn on hardware acceleration for a playable frame rate.', 9000);

    status('Checking game data…');
    let manifest;
    try { manifest = await loadManifest(); } catch (e) { return fail(e.message); }
    const files = await openStore();
    state.store = files.kind;

    const music = manifest.files.filter((f) => f.group === 'music');
    const musicSize = music.reduce((n, f) => n + f.size, 0);
    const wantMusic = () => store.get('music', navigator.connection && navigator.connection.saveData ? '0' : '1') === '1';
    const selected = () => manifest.files.filter((f) => f.group !== 'music' || wantMusic());

    let missing = await missingFiles(files, selected());
    if (missing.length) {
      $('xt-dl-music').checked = wantMusic();
      $('xt-dl-music-size').textContent = mb(musicSize);
      $('xt-dl-music').closest('label').hidden = !music.length;
      const refresh = async () => {
        missing = await missingFiles(files, selected());
        $('xt-dl-size').textContent = mb(missing.reduce((n, f) => n + f.size, 0));
      };
      $('xt-dl-music').onchange = () => { store.set('music', $('xt-dl-music').checked ? 1 : 0); refresh(); };
      await refresh();
      if (navigator.storage && navigator.storage.estimate) {
        const est = await navigator.storage.estimate();
        const free = (est.quota || 0) - (est.usage || 0);
        const need = missing.reduce((n, f) => n + f.size, 0);
        if (est.quota && free < need * 1.05) {
          $('xt-dl-quota').hidden = false;
          $('xt-dl-quota').textContent = `This browser offers about ${mb(free)} of storage here, less than the download. Private windows and low disk space shrink it.`;
        }
      }
      show('xt-download');
      await new Promise((resolve) => { $('xt-dl-go').onclick = resolve; });
      try {
        if (navigator.storage && navigator.storage.persist) navigator.storage.persist().catch(() => {});
        await download(files, missing);
      } catch (e) {
        return fail(e.message || String(e));
      }
    }

    status('Starting the engine…');
    await runtimePromise;
    try {
      await mountUserdir();
      await mountData(files, selected());
    } catch (e) {
      return fail(e.message || String(e));
    }
    // Only once every file this manifest needs has loaded: drop what older
    // releases left behind (files whose hash is no longer listed).
    files.prune(new Set(manifest.files.map(fileId)), manifest).catch(() => {});

    $('xt-opt-fullscreen').checked = store.get('fullscreen', '1') === '1';
    const fsSupported = !!(document.documentElement.requestFullscreen || document.documentElement.webkitRequestFullscreen);
    $('xt-opt-fullscreen').closest('label').hidden = !fsSupported;
    const hints = [];
    if (!fsSupported && hasTouch) hints.push('For fullscreen, add this page to your Home Screen and open it from there.');
    if (!touchOnly) hints.push('Click the game to capture the mouse. Esc opens the menu.');
    $('xt-ready-hint').textContent = hints.join(' ');
    show('xt-ready');
    $('xt-play').disabled = false;
    $('xt-play').focus();
    $('xt-play').onclick = play;
    if (params.has('autoplay')) play();
  }

  // Settings
  $('xt-opt-scale').value = store.get('scale', 'auto');
  $('xt-opt-scale').onchange = (e) => { store.set('scale', e.target.value); refit(); };
  $('xt-clear').onclick = async () => {
    if (!confirm('Delete the downloaded game data from this browser? Your settings stay.')) return;
    try {
      const root = await navigator.storage.getDirectory();
      await root.removeEntry(DATA_CACHE, { recursive: true });
    } catch (e) { /* no OPFS */ }
    await caches.delete(DATA_CACHE).catch(() => {});
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
