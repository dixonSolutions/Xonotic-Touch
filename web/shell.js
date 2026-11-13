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

  // The music is no longer optional (one download with everything).
  try { localStorage.removeItem('xt.music'); localStorage.removeItem('xt.allmaps'); } catch (e) { /* private mode */ }

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
  let statusTimer = 0;
  function show(id) {
    clearTimeout(statusTimer);
    for (const p of panels) $(p).hidden = p !== id;
    $('xt-overlay').classList.remove('xt-splash');
  }
  // Start-up steps say what they do only when they take long enough to
  // notice (0.6 s): a returning visitor sees the splash, then the game.
  function status(text, detail) {
    $('xt-status-text').textContent = text;
    $('xt-status-detail').textContent = detail || '';
    if (!$('xt-status').hidden) return;
    clearTimeout(statusTimer);
    statusTimer = setTimeout(() => show('xt-status'), 600);
  }

  // ------------------------------------------------- breadcrumbs, problems
  //
  // Phones close pages without warning (memory, a discarded tab). Each start
  // writes where it got to into localStorage, and "closed" on a normal
  // close, so the next load can tell the player what happened instead of
  // silently starting over.
  function crumb(phase, extra) {
    const c = Object.assign({ phase, t: Date.now(), build: CONFIG.build || '' }, extra || {});
    state.crumb = c;
    store.set('crumb', JSON.stringify(c));
  }
  const prevCrumb = (() => { try { return JSON.parse(store.get('crumb', '') || 'null'); } catch (e) { return null; } })();
  function problem(text) {
    if (!text) return;
    console.warn('Godnotic:', text);
    state.problem = text;
    store.set('problem', JSON.stringify({ text, t: Date.now() }));
    const el = $('xt-st-error');
    if (el) el.textContent = text;
  }
  function killedReason() {
    const c = prevCrumb;
    if (!c || c.closed || c.phase === 'quit') return '';
    const at = {
      download: `while downloading (${c.done || 0} of ${c.total || '?'} pieces were saved; it carries on from there)`,
      mount: 'while loading the game data',
      engine: 'while starting the game',
      running: 'during a game',
    }[c.phase];
    return at ? `The browser closed this page ${at}, most likely to free memory.` : '';
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
      astc: !!gl.getExtension('WEBGL_compressed_texture_astc'),
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

  function onGesture(e) {
    if (!state.started) return;
    if (e && e.target && e.target.closest && (e.target.closest('#xt-more') || e.target.closest('#xt-map'))) return;
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
  window.addEventListener('pagehide', () => {
    if (state.crumb) store.set('crumb', JSON.stringify(Object.assign({}, state.crumb, { closed: true })));
    if (state.started) saveNow();
    if (state.lazy) remember(XTLazyFS.takeTouched());
  });
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
  // (XT_WEB_DATA_URL in scripts/build-web.sh), else ./data/. The profiles
  // and their manifests are the ones the page was built with
  // (scripts/stage-web-data.sh), manifests named by their own hash, so a
  // warm start reads them from storage and asks the network nothing. A page
  // without them (served from web/, or the release zip) asks
  // data/profiles.json, then data/manifest.json.
  const dataBase = new URL(params.get('data') || CONFIG.dataUrl || 'data/', location.href);
  const SMALL_UPDATE = 50e6;
  const manifestId = (p) => (/([0-9a-f]{8,64})\.json$/.exec(p || '') || [])[1];

  // A no-cache pointer file (profiles.json, stock.json, manifest.json).
  // Offline, the last one seen will do.
  async function pointer(base, name) {
    const url = new URL(name, base);
    const remember = 'pointer-json:' + url.href;
    try {
      const res = await fetch(url, { cache: 'no-cache' });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const j = await res.json();
      if (!j.manifest && !j.profiles) throw new Error('the data there is from an older release');
      store.set(remember, JSON.stringify(j));
      return j;
    } catch (e) {
      try { const last = JSON.parse(store.get(remember, '') || 'null'); if (last) return last; } catch (e2) { /* none */ }
      // Pages before profiles remembered just the manifest name.
      const old = store.get('pointer:' + url.href, '');
      if (old) return { manifest: old };
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

  // Profiles: one data set per kind of browser (docs/WEB.md, "Profiles"):
  //   web    DXT textures, every GPU (desktops have S3TC)
  //   astc   the slim phone set, for GPUs with WEBGL_compressed_texture_astc
  //   full   the stock pk3s whole, as pages before maps on demand stored
  //          them: kept while every chunk of it is in this browser, so a
  //          returning visitor downloads nothing ("keep")
  // Returns {default, profiles: {name: {manifest, gpu?, keep?}}}.
  async function listProfiles() {
    if (params.get('manifest')) return { default: 'web', profiles: { web: { manifest: params.get('manifest') } } };
    if (CONFIG.profiles && CONFIG.profiles.profiles && !params.get('data')) return CONFIG.profiles;
    if (CONFIG.manifest && !params.get('data')) return { default: 'web', profiles: { web: { manifest: CONFIG.manifest } } };
    let j;
    try { j = await pointer(dataBase, 'profiles.json'); } catch (e) { j = await pointer(dataBase, 'manifest.json'); }
    return j.profiles ? j : { default: 'web', profiles: { web: { manifest: j.manifest } } };
  }

  /**
   * One profile's files. A shell copy of the page (GitHub Pages) carries only
   * this fork's own files and names a "stock" host for the stock pk3s, the
   * way the apps carry the game code and download the rest on first launch;
   * that host's stock.json lists its profiles' stock manifests, so it can
   * move to newer data without the shell being republished.
   */
  async function loadProfile(name, spec) {
    const main = await getManifest(dataBase, spec.manifest);
    const ids = [main.id];
    const files = main.m.files.map((f) => Object.assign({}, f, { base: dataBase.href }));
    let defaultMap = main.m.defaultMap || '';
    if (main.m.stock) {
      const stockBase = new URL(params.get('stock') || main.m.stock, dataBase);
      const j = await pointer(stockBase, 'stock.json');
      const sp = (j.profiles && j.profiles[name]) || { manifest: j.manifest };
      const stock = await getManifest(stockBase, sp.manifest);
      ids.push(stock.id);
      defaultMap = defaultMap || stock.m.defaultMap || '';
      const have = new Set(files.map((f) => f.name));
      for (const f of stock.m.files) if (!have.has(f.name)) files.push(Object.assign({}, f, { base: stockBase.href }));
    }
    if (main.m.chunk) data.chunk = main.m.chunk;
    return { name, ids, files, defaultMap, keep: !!spec.keep };
  }

  // The stock host's profiles, for a shell (its own config lists only its
  // own manifest): the same names, each with the shell's own files.
  async function shellProfiles(list) {
    // The manifest this browser has used (stored: no request), else the default.
    const first = list.profiles[store.get('profile', '')] || list.profiles[list.default] || Object.values(list.profiles)[0];
    const main = await getManifest(dataBase, first.manifest);
    if (!main.m.stock) return list;
    const stockBase = new URL(params.get('stock') || main.m.stock, dataBase);
    const j = await pointer(stockBase, 'stock.json');
    if (!j.profiles) return list;
    const out = { default: j.default || 'web', profiles: {} };
    for (const [n, sp] of Object.entries(j.profiles)) out.profiles[n] = Object.assign({}, sp, { manifest: first.manifest });
    return out;
  }

  const PROFILE_LABEL = {
    web: 'Standard (for every graphics chip)',
    astc: 'Light (ASTC textures, Opus music: phones, tablets, Apple)',
    full: 'Full download (as stored before maps on demand)',
  };

  /**
   * Which profile this browser uses: ?profile=, the one chosen before (while
   * it is still offered and, for a "keep" profile, still whole), a returning
   * visitor's full set when every chunk of it is here (older layouts adopted
   * first), else by GPU.
   */
  async function chooseProfile(list, gpu) {
    const offered = list.profiles;
    const forced = params.get('profile');
    if (forced && offered[forced]) return forced;
    // A "keep" profile stays while none of its unhosted chunks is missing
    // (its hosted ones, new game code say, download as usual).
    const keepWhole = async (name) => {
      const p = await loadProfile(name, offered[name]);
      const need = p.files.filter((f) => f.group === 'core');
      const lacking = async () => (await data.missing(need)).some((c) => c.file.hosted === false);
      if (!(await lacking())) return true;
      // Older layouts (whole files, Cache API parts): adopt, then look again.
      try {
        if (await data.adopt(need, (n, total) => status('Moving downloaded data to the new storage layout…', `${mbPair(n, total)} checked`))) {
          return !(await lacking());
        }
      } catch (e) { console.warn('Adopting older downloads failed:', e); }
      return false;
    };
    const saved = store.get('profile', '');
    if (saved && offered[saved]) {
      if (!offered[saved].keep || await keepWhole(saved)) return saved;
    }
    for (const [name, spec] of Object.entries(offered)) {
      if (spec.keep && (await data.anything()) && await keepWhole(name)) return name;
    }
    if (offered.astc && gpu && gpu.astc) return 'astc';
    return offered[list.default] ? list.default : Object.keys(offered)[0];
  }

  const blobUrl = (c) => new URL(`blobs/${c.h}.bin`, c.file.base || dataBase).href;
  const sum = (list) => list.reduce((n, c) => n + c.size, 0);
  const fmtTime = (s) => s > 5400 ? `${Math.round(s / 3600)} h` : s > 90 ? `${Math.round(s / 60)} min` : `${Math.max(1, Math.round(s))} s`;
  const hosted = (list) => list.filter((c) => c.file.hosted !== false);

  // Download the given chunks with the progress panel: percent, "x / y MB ·
  // speed · time left", Pause / Resume and Cancel. Pause and Cancel keep
  // what has arrived (whole chunks, and 4 MiB pieces of the rest). Resolves
  // when all are stored; rejects with AbortError on Cancel. ui: the element
  // ids (the start-up panel, or the in-game map panel).
  const MAIN_UI = { panel: 'xt-progress', percent: 'xt-percent', fill: 'xt-bar-fill', bar: 'xt-bar', detail: 'xt-progress-detail',
    live: 'xt-progress-live', label: 'xt-progress-label', pause: 'xt-dl-pause', cancel: 'xt-dl-cancel' };
  function runDownload(list, label, ui = MAIN_UI) {
    const total = sum(list);
    const got = new Map();
    const sizeOf = new Map(list.map((c) => [c.h, c.size]));
    let paused = false;
    let cancelled = false;
    let controller = null;
    let wake = null;
    const samples = [];
    let lastPaint = 0;
    let lastSaid = -1;
    const el = (k) => ui[k] && $(ui[k]);
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
      if (el('percent')) el('percent').textContent = Math.floor(pct) + '%';
      el('fill').style.width = pct.toFixed(1) + '%';
      el('bar').setAttribute('aria-valuenow', pct.toFixed(0));
      el('bar').setAttribute('aria-valuetext', `${Math.floor(pct)}%, ${mbPair(all, total)}`);
      if (paused) {
        el('detail').textContent = `Stopped at ${mbPair(all, total)}. Resume carries on from there.`;
      } else {
        el('detail').textContent = `${mbPair(all, total)}` +
          (rate > 0 ? ` · ${(rate / 1e6).toFixed(1)} MB/s · ${fmtTime((total - all) / rate)} left` : '');
      }
      // Screen readers: every 10%, not every repaint.
      const step = Math.floor(pct / 10);
      if (step !== lastSaid && el('live')) { lastSaid = step; el('live').textContent = `${step * 10}% downloaded`; }
    };
    const setPaused = (on) => {
      paused = on;
      el('panel').classList.toggle('xt-paused', on);
      el('label').textContent = on ? 'Paused' : label;
      if (el('pause')) el('pause').textContent = on ? 'Resume' : 'Pause';
      paint(true);
    };
    if (el('pause')) {
      el('pause').onclick = () => {
        if (!paused) { setPaused(true); if (controller) controller.abort(); } else { setPaused(false); if (wake) wake(); }
      };
    }
    el('cancel').onclick = () => {
      cancelled = true;
      el('cancel').textContent = 'Cancelling…';
      if (controller) controller.abort();
      if (wake) wake();
    };
    const main = ui === MAIN_UI;
    if (main) {
      state.downloading = true;
      $('xt-verify').disabled = true;
      crumb('download', { done: 0, total: list.length });
      show('xt-progress');
    } else {
      el('panel').hidden = false;
    }
    el('cancel').textContent = 'Cancel';
    setPaused(false);
    if (el('pause')) el('pause').focus({ preventScroll: true });
    return (async () => {
      try {
        let todo = list;
        for (;;) {
          controller = new AbortController();
          if (main) state.downloadController = controller;
          else state.mapController = controller;
          try {
            // One tab downloads at a time (Web Locks): a second tab waits,
            // then skips the chunks the first one stored.
            const fetchAll = async () => {
              const left = [];
              for (const c of todo) if (await data.has(c)) { got.set(c.h, c.size); paint(); } else left.push(c);
              todo = left;
              await data.download(todo, {
                urlFor: blobUrl,
                signal: controller.signal,
                onBytes: (c, n) => {
                  got.set(c.h, n);
                  paint();
                  if (n === c.size && main) crumb('download', { done: [...got].filter(([h, v]) => v === sizeOf.get(h)).length, total: list.length });
                },
              });
            };
            if (navigator.locks && navigator.locks.request) await navigator.locks.request('xt-data-download', { signal: controller.signal }, fetchAll);
            else await fetchAll();
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
        if (main) {
          state.downloading = false;
          state.downloadController = null;
          $('xt-verify').disabled = false;
        } else {
          state.mapController = null;
          el('panel').hidden = true;
        }
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

  // ------------------------------------------------- mounting the data
  //
  // Lazy (docs/WEB.md, "Memory"): every stored file becomes a file whose
  // reads come from the browser's storage (web/lazyfs.js): nothing is
  // copied at start-up, and a map's pack costs nothing until the engine
  // reads it. Where the browser cannot do that (probe), the old way: the
  // files are copied into the JS heap, and only the core, the music and the
  // one map being played are mounted.
  const mounts = new Map(); // engine path -> manifest file
  const enginePaths = new Set(); // mounted paths the engine has scanned (fs_rescan)
  const pathOf = (f) => `${BASEDIR}/${f.path}`;

  async function chunkBlobs(f) {
    const blobs = [];
    for (const c of data.chunks(f)) {
      const b = await data.be.blob('c-' + c.h).catch(() => null);
      if (!b || b.size !== c.size) {
        await data.be.remove('c-' + c.h);
        throw new Error(`${f.name} is incomplete in the browser's storage. Reload to download the missing part again.`);
      }
      blobs.push(b);
    }
    return blobs;
  }

  async function mount(f) {
    const path = pathOf(f);
    if (mounts.has(path)) return;
    const FS = Module.FS;
    if (state.lazy) {
      XTLazyFS.create(FS, path, f.size, await chunkBlobs(f), data.chunk);
    } else {
      const buf = await data.read(f);
      mkdirs(FS, path);
      FS.writeFile(path, buf, { canOwn: true });
    }
    mounts.set(path, f);
  }

  function unmount(f) {
    const path = pathOf(f);
    if (!mounts.has(path)) return;
    if (state.lazy) XTLazyFS.remove(Module.FS, path);
    else try { Module.FS.unlink(path); } catch (e) { /* gone */ }
    mounts.delete(path);
    enginePaths.delete(path);
  }

  async function mountData(files) {
    let i = 0;
    for (const f of files) {
      i++;
      if (!state.lazy) status('Loading game data…', `file ${i} of ${files.length}`);
      await mount(f);
      enginePaths.add(pathOf(f));
    }
  }

  async function decideLazy() {
    if (params.get('lazy') === '0' || typeof XTLazyFS === 'undefined') return false;
    const f = state.files.find((x) => x.group === 'core' && x.size >= 1024);
    if (!f) return false;
    try {
      const b = await data.be.blob('c-' + f.chunks[0]);
      return await XTLazyFS.probe(b);
    } catch (e) {
      return false;
    }
  }

  // ---------------------------------------------- maps on demand
  //
  // The engine asks before every "map" or "changelevel" (sys_wasm.c,
  // js_NeedMap). 1: go ahead. 0: the page fetches and mounts the map's pack
  // with a progress panel, then runs "fs_rescan" and the same command again.
  const maps = {
    byName: new Map(), // map name -> manifest file of its pack
    pending: null, // {name, line}: the command waiting for its pack
    current: '',
  };
  const normMap = (s) => String(s || '').toLowerCase().replace(/^maps\//, '').replace(/\.bsp$/, '');

  function needMap(raw, line) {
    const name = normMap(raw);
    const f = maps.byName.get(name);
    const here = !f || (enginePaths.has(pathOf(f)) && !state.rescanPending);
    if (here && (!state.lazy || maps.warmed === line)) {
      maps.warmed = '';
      maps.current = name;
      settleLater();
      return 1;
    }
    const first = !maps.pending;
    maps.pending = { name, line, f };
    if (first) fetchMap().catch((e) => { maps.pending = null; toast(e.message || String(e), 8000); });
    return 0;
  }

  async function fetchMap() {
    while (maps.pending) {
      const { name, line, f } = maps.pending;
      const missing = f ? hosted(await data.missing([f])) : [];
      if (missing.length) {
        $('xt-map-title').textContent = `Downloading ${name}`;
        if (document.pointerLockElement) document.exitPointerLock();
        try {
          await runDownload(missing, `Downloading the map ${name}…`, MAP_UI);
        } catch (e) {
          if (maps.pending && maps.pending.name !== name) continue; // another map was asked for meanwhile
          maps.pending = null;
          if (e && e.name === 'AbortError') { toast(`${name}: download cancelled.`); return; }
          throw new Error(`Could not download the map ${name}: ${e.message || e}`);
        }
      }
      if (maps.pending && maps.pending.name !== name) continue;
      if (f && !state.lazy) {
        // In memory: one map pack at a time.
        for (const g of maps.byName.values()) if (g !== f) unmount(g);
      }
      if (f) await mount(f);
      await warm(f);
      if (maps.pending && maps.pending.name !== name) continue;
      maps.pending = null;
      maps.warmed = line;
      if (state.rescanPending || (f && !enginePaths.has(pathOf(f)))) rescanAnd(line);
      else Module.ccall('xt_web_command', null, ['string'], [line]);
      return;
    }
  }

  // ---------------------------------------------- warming the read cache
  //
  // Lazy reads are synchronous and cost about 10 ms per MiB; reading the
  // same bytes ahead of time, asynchronously, costs a tenth of that. So the
  // page remembers which 1 MiB blocks the engine read during start-up and map
  // loads (per profile, in localStorage), reads those and the map's own pack
  // into the cache before the engine starts or loads a map, and shrinks the
  // cache again once the load is over.
  // How much may be read ahead: phones get less.
  const lowMemory = (navigator.deviceMemory && navigator.deviceMemory <= 4) || touchOnly;
  const AHEAD_MAX = (lowMemory ? 160 : 384) * 1048576;
  const IDLE_CACHE = 16 * 1048576;
  const hotKey = () => 'hot:' + state.profile;
  function hotList() { try { return JSON.parse(store.get(hotKey(), '{}')) || {}; } catch (e) { return {}; } }
  function remember(touched) {
    const hot = hotList();
    for (const [p, ks] of Object.entries(touched)) {
      const f = mounts.get(p);
      if (!f || f.group === 'map') continue; // a map's pack is read whole before its load
      hot[p] = [...new Set([...(hot[p] || []), ...ks])].sort((a, b) => a - b);
    }
    for (const p of Object.keys(hot)) if (!mounts.has(p)) delete hot[p]; // files of older releases
    store.set(hotKey(), JSON.stringify(hot));
  }
  // Read ahead, most wanted first: the blocks earlier loads read, the map's
  // own pack, then the rest of the core files, up to AHEAD_MAX.
  async function warm(mapFile) {
    if (!state.lazy) return;
    remember(XTLazyFS.takeTouched()); // what the last load read
    const order = [];
    const all = (path, size) => { for (let k = 0; k * XTLazyFS.BLOCK < size; k++) order.push([path, k]); };
    for (const [p, ks] of Object.entries(hotList())) for (const k of ks) order.push([p, k]);
    const known = order.length;
    if (mapFile) all(pathOf(mapFile), mapFile.size);
    const withMap = order.length;
    for (const [p, f] of mounts) if (f.group === 'core') all(p, f.size);
    // Once earlier loads have shown what gets read, that and a margin will
    // do; before that, as much of the core as the budget allows.
    // At start-up (no map), only what earlier starts read: the engine
    // waits for this, and reads anything else itself.
    const max = known ? Math.min(AHEAD_MAX, ((mapFile ? withMap : known) + (mapFile ? 32 : 0)) * XTLazyFS.BLOCK) : AHEAD_MAX;
    try { await XTLazyFS.prefetch(order, max); } catch (e) { console.warn('Read-ahead failed:', e); }
  }
  let settleTimer = 0;
  function settleLater() {
    if (!state.lazy) return;
    clearTimeout(settleTimer);
    settleTimer = setTimeout(() => { remember(XTLazyFS.takeTouched()); XTLazyFS.trim(IDLE_CACHE); }, 15000);
  }

  // Let the engine see every mounted file, then run `line`.
  function rescanAnd(line) {
    for (const p of mounts.keys()) enginePaths.add(p);
    state.rescanPending = false;
    Module.ccall('xt_web_command', null, ['string'], [`fs_rescan\n${line || ''}`]);
  }

  // The engine's console command "web_maps" (sys_wasm.c): what is stored.
  async function backgroundStatus() {
    const files = (state.files || []).filter((f) => f.hosted !== false);
    const missing = await data.missing(files);
    const total = files.reduce((n, f) => n + f.size, 0);
    return { total, left: sum(missing) };
  }
  function mapsCommand(arg) {
    if (arg === 'storage') { popover(true); return ''; }
    backgroundStatus().then((s) => toast(s.left ? `Game data: ${mb(s.total - s.left)} of ${mb(s.total)} here; a map that is missing downloads when played.` : `Game data: all ${mb(s.total)} here.`));
    return '';
  }
  const MAP_UI = { panel: 'xt-map', fill: 'xt-map-fill', bar: 'xt-map-bar', detail: 'xt-map-detail', label: 'xt-map-label', cancel: 'xt-map-cancel' };

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

  const BACKEND_LABEL = {
    opfs: 'Browser files (OPFS)',
    cache: 'Browser cache storage',
    memory: 'Memory only: downloaded again each visit',
  };
  async function estimate() {
    try { return navigator.storage && navigator.storage.estimate ? await navigator.storage.estimate() : null; } catch (e) { return null; }
  }

  // The Storage panel: where the data is, how much, how much room, whether
  // the browser keeps it, and the last problem: readable without devtools.
  async function showStorage() {
    const files = (state.files || []).filter((f) => f.hosted !== false || f.group === 'core');
    const used = await data.storedBytes(files);
    const total = files.reduce((n, f) => n + f.size, 0);
    const est = await estimate();
    const kept = await persisted();
    $('xt-st-backend').textContent = (BACKEND_LABEL[data.kind] || data.kind) + (data.why && data.why.length && data.kind !== 'opfs' ? ` (${data.why.join('; ')})` : '');
    $('xt-st-stored').textContent = `${mb(used)} of ${mb(total)}` + (state.profile ? ` · ${state.profile === 'astc' ? 'ASTC set' : state.profile === 'full' ? 'full set' : 'standard set'}` : '');
    $('xt-st-quota').textContent = est && est.quota ? `${mb(est.usage || 0)} used of ${mb(est.quota)}` : 'not reported';
    $('xt-st-persist').textContent = kept ? 'Yes, until you delete it' : 'No: may be cleared when space runs low';
    const last = state.problem || (() => { try { return JSON.parse(store.get('problem', '') || 'null').text; } catch (e) { return ''; } })();
    $('xt-st-error').textContent = last || data.be.lastError || 'None';
    let why = '';
    if (!kept && data.kind !== 'memory') {
      if (isSafari) why = 'Safari deletes it after 7 days without a visit unless the page is on the Home Screen.';
      else if (installPrompt) why = 'Installing the app keeps it.';
      else if (isIOS) why = 'To keep it, add this page to the Home Screen (Share, then Add to Home Screen) and play from there.';
      else if (!installed()) why = 'Installing this page as an app (browser menu) keeps it.';
    }
    $('xt-storage-text').textContent = why;
  }

  async function freeSpaceLine(need) {
    let est = null;
    try { est = navigator.storage && navigator.storage.estimate ? await navigator.storage.estimate() : null; } catch (e) { /* none */ }
    const el = $('xt-dl-space');
    if (!est || !est.quota) { el.textContent = ''; return true; }
    const free = est.quota - (est.usage || 0);
    const fits = free > need * 1.05;
    el.textContent = fits ? `${mb(free)} free for this site.` : '';
    if (!fits) problem(`Not enough space: this needs ${mb(need)} and the browser offers ${mb(free)} for this site. Free some disk space${navigator.storage && navigator.storage.persist ? ', or allow the site to store data persistently when asked' : ''}.`);
    return fits;
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
      renderSize, setFullscreen, mouseGrab, screenKeyboard, quit, needMap, mapsCommand,
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
      state.logLines = (state.logLines || 0) + 1;
      logTail.push(text);
      if (logTail.length > 200) logTail.shift();
      if (DEBUG) console.log(text);
    },
    printErr(text) {
      if (LOGSINK) sink('[err] ' + text);
      state.logLines = (state.logLines || 0) + 1;
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
    crumb('quit');
    setTimeout(() => show('xt-status'), 0);
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
    // Inside a click the browser lets the page go fullscreen; started
    // without one (the usual way now), it happens at the first tap or key.
    if (fullscreen && !(await enterFullscreen())) state.fsPending = true;
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
    await state.warming;
    crumb('engine');
    mark('engine-start');
    try {
      Module.callMain(engineArgs(fullscreen));
    } catch (e) {
      if (e !== 'unwind' && !(e && e.name === 'ExitStatus')) {
        console.error(e);
        fail('The game failed to start: ' + (e && e.message || e));
        return;
      }
    }
    // Normally done by the engine's startup command buffer (sys_wasm.c).
    try { Module._xt_web_hook_maps(); } catch (e) { /* older engine */ }
    settleLater();
    refit();
    resumeAudio();
    setInterval(autosave, 250);
    requestAnimationFrame(() => mark('first-frame'));
    setTimeout(() => { if (state.started) crumb('running'); }, 5000);
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
    state.gpu = gpu;

    // The engine downloads and compiles while storage is checked (the page
    // preloads it from <head>).
    loadEngine();

    const diag = [`GPU: ${gpu.renderer || 'unknown'}`, gpu.s3tc ? 'S3TC' : 'no S3TC (DXT decoded in software)'];
    if (gpu.astc) diag.push('ASTC');
    if (CONFIG.build) diag.push(`build ${CONFIG.build}`);
    $('xt-diag').textContent = diag.join(' · ');
    if (gpu.software) toast('Your browser is drawing without the GPU. Turn on hardware acceleration for a playable frame rate.', 9000);

    status('Starting…');
    try {
      data = await XTStore.Store.open({ preferCache: params.get('store') === 'cache' });
    } catch (e) {
      return fail(e.message || String(e));
    }
    state.store = data.kind;
    if (data.kind === 'memory') problem('This browser does not let the page store data here (a private window, or storage blocked for the site): the game data is downloaded on every visit. ' + data.why.join('; '));
    let profile;
    try {
      let list = await listProfiles();
      list = await shellProfiles(list);
      state.profiles = list;
      const name = await chooseProfile(list, gpu);
      profile = await loadProfile(name, list.profiles[name]);
    } catch (e) { return fail(e.message || String(e)); }
    const files = profile.files;
    state.files = files;
    state.profile = profile.name;
    for (const f of files) if (f.group === 'map' && f.map) maps.byName.set(normMap(f.map), f);
    mark('manifest');

    // One download with everything in it (every map, the music): nothing
    // to choose. Files the host no longer serves (the full set's maps pk3)
    // only count when they are here.
    const startedKey = profile.name === 'web' ? 'started' : 'started-' + profile.name;
    const hadStart = store.get(startedKey, '') === '1' || (profile.name === 'full' && store.get('started', '') === '1');
    const selected = () => files.filter((f) => f.hosted !== false);

    let missing = await data.missing(selected());
    let adopted = 0;
    // Downloads made by earlier versions of this page: turn them into chunks
    // once (each checked against its hash) instead of downloading again.
    if (missing.length && await data.anything()) {
      try {
        adopted = await data.adopt(files, (n, total) => status('Moving downloaded data to the new storage layout…', `${mbPair(n, total)} checked`));
        if (adopted) missing = await data.missing(selected());
      } catch (e) {
        problem('Moving older downloads failed: ' + (e && e.message || e));
      }
    }
    mark('checked');
    // Closed by the browser last time (memory, a discarded tab): say so,
    // even when nothing has to be downloaded again.
    if (!missing.length && killedReason()) problem(killedReason());

    if (missing.length) {
      // Why the page has to download, in one line (and the console).
      const allChunks = new Set(selected().flatMap((f) => f.chunks)).size;
      const firstInstall = missing.length === allChunks;
      let reason = killedReason();
      if (!reason && data.kind === 'memory') reason = 'Nothing can be stored in this window, so the game data downloads on every visit.';
      if (!reason && firstInstall && hadStart) reason = 'The game data is gone from this browser: it was cleared (site data deleted, or removed by the browser to free space, since it was not kept persistently).';
      if (!reason && !firstInstall && hadStart) reason = `An update: ${mb(sum(missing))} changed.`;
      if (reason) problem(reason);
      // A small update (new game code, a config): fetch it and go on. The
      // question is for first installs and big downloads.
      // Visitors who left the music out when it was optional get it the
      // same way, whatever its size.
      const onlyMusic = missing.every((c) => c.file.group === 'music');
      if (!firstInstall && (sum(missing) <= SMALL_UPDATE || onlyMusic) && !params.has('ask')) {
        try {
          await runDownload(missing, 'Updating…');
        } catch (e) {
          if (!(e && e.name === 'AbortError')) problem(e.message || String(e));
        }
        missing = await data.missing(selected());
      }
      if (missing.length) {
        const refresh = async () => {
          missing = await data.missing(selected());
          let partial = 0;
          for (const c of missing) partial += await data.resumable(c);
          const stored = await data.storedBytes(selected());
          const need = sum(missing) - partial;
          $('xt-dl-size').textContent = mb(need);
          $('xt-dl-title').textContent = hadStart || adopted || stored ? 'Finish downloading Godnotic' : 'Download Godnotic';
          const already = stored + partial;
          $('xt-dl-resumed').hidden = already < 1e6;
          $('xt-dl-resumed').textContent = `${mb(already)} is already here; it carries on from there.`;
          $('xt-dl-reason').hidden = !reason;
          $('xt-dl-reason').textContent = reason;
          $('xt-dl-go').textContent = already >= 1e6 ? 'Continue' : 'Download';
          await freeSpaceLine(need);
        };
        await refresh();
        while (missing.length) {
          show('xt-download');
          $('xt-dl-go').focus({ preventScroll: true });
          // ?autodownload (tests): no click.
          if (!params.has('autodownload')) await new Promise((resolve) => { $('xt-dl-go').onclick = resolve; });
          // Asked inside the click: Firefox shows its prompt only then, and
          // persistent storage gets the larger quota and is not evicted.
          requestPersist().catch(() => {});
          try {
            await runDownload(missing, 'Downloading Godnotic…');
          } catch (e) {
            if (e && e.name === 'AbortError') { await refresh(); continue; }
            if (e && e.name === 'QuotaExceededError') {
              const est = await estimate();
              problem(`Not enough space: the browser stopped the download at its limit for this site` + (est && est.quota ? ` (${mb(est.usage || 0)} of ${mb(est.quota)})` : '') + '. What arrived is kept.');
              return fail(state.problem + ' Free some disk space and reload; it carries on from there.');
            }
            problem(e.message || String(e));
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

    status('Starting…');
    crumb('mount');
    await runtimePromise;
    mark('engine-ready');
    try {
      await mountUserdir();
      state.lazy = await decideLazy();
      // Lazy: everything stored costs nothing until read. In memory: the
      // core and the music; maps when they are played.
      const complete = [];
      const here = await Promise.all(files.map(async (f) => !(await data.missing([f])).length));
      files.forEach((f, i) => {
        if (!here[i]) return;
        if (!state.lazy && f.group !== 'core' && f.group !== 'music') return;
        complete.push(f);
      });
      await mountData(complete);
    } catch (e) {
      problem('Loading the game data failed: ' + (e.message || e));
      return fail(e.message || String(e));
    }
    mark('mounted');
    state.warming = warm(null).then(() => mark('warmed'));
    store.set(startedKey, 1);
    store.set('profile', profile.name);
    // Every file this start needs was there: drop what no page path that
    // started recently still lists (older releases' chunks, old layouts).
    data.prune(location.pathname, profile.ids).then((r) => { state.pruned = r; }).catch(() => {});

    $('xt-opt-fullscreen').checked = store.get('fullscreen', touchOnly ? '1' : '0') === '1';
    const fsSupported = !!(document.documentElement.requestFullscreen || document.documentElement.webkitRequestFullscreen);
    $('xt-opt-fullscreen-row').hidden = !fsSupported;
    showStorage().catch(() => {});
    mark('ready');
    // Straight into the game: no Play button. Fullscreen and sound need a
    // tap or key, so they start with the first one.
    if (params.has('noplay')) {
      show('xt-ready');
      $('xt-play').onclick = play;
      return;
    }
    play();
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

  // Storage and settings: a popover from the header bar's menu button, or
  // over the game from the console (web_storage). Closes on Escape and on
  // a click or tap outside it.
  function popover(open) {
    const el = $('xt-more');
    if (open === undefined) open = el.hidden;
    el.hidden = !open;
    $('xt-menu').setAttribute('aria-expanded', open ? 'true' : 'false');
    if (open) {
      // Over the game, the mouse has to be free to use it.
      if (document.pointerLockElement) document.exitPointerLock();
      if (data) showStorage().catch(() => {});
      el.focus && el.setAttribute('tabindex', '-1');
      el.focus && el.focus({ preventScroll: true });
    }
  }
  $('xt-menu').onclick = (e) => { e.stopPropagation(); popover(); };
  document.addEventListener('pointerdown', (e) => {
    const el = $('xt-more');
    if (!el.hidden && !el.contains(e.target) && e.target !== $('xt-menu')) popover(false);
  }, true);
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape' && !$('xt-more').hidden) { e.stopPropagation(); e.preventDefault(); popover(false); }
  }, true);
  $('xt-opt-fullscreen').onchange = (e) => store.set('fullscreen', e.target.checked ? 1 : 0);
  // The last problem from an earlier visit, until something replaces it.
  try { const p0 = JSON.parse(store.get('problem', '') || 'null'); if (p0) $('xt-st-error').textContent = `${p0.text} (${new Date(p0.t).toLocaleString()})`; } catch (e) { /* none */ }

  // For the dev console and automated tests: xt.command('map stormkeep')
  window.xt = {
    command: (text) => Module.ccall('xt_web_command', null, ['string'], [text]),
    log: () => logTail.slice(),
    state,
    maps,
    mounts,
    lazyStats: () => typeof XTLazyFS !== 'undefined' ? Object.assign({ cacheBytes: XTLazyFS.cacheBytes }, XTLazyFS.stats) : null,
    backgroundStatus,
  };

  main().catch((e) => fail(e && e.message || String(e)));
})();
