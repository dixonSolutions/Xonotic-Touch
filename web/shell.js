// Godnotic in the browser: data loader and platform shim.
//
// The engine is DarkPlaces built with Emscripten (scripts/build-web.sh). This
// file is the "platform layer" that a phone or desktop OS would otherwise be:
//
//   data        downloads the game data once, in hash-checked 32 MiB chunks,
//               into the origin private file system (store.js), which the
//               engine reads in place (lazyfs.js); or reads the stock packs
//               from a Godnotic folder the player picked; or hands over to
//               the installed app
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
    del(k) { try { localStorage.removeItem('xt.' + k); } catch (e) { /* private mode */ } },
    keys() { try { return Object.keys(localStorage).filter((k) => k.startsWith('xt.')).map((k) => k.slice(3)); } catch (e) { return []; } },
  };

  // Keys of pages before this one (there is one download with everything,
  // the music included, and nothing to choose).
  store.del('music');
  store.del('allmaps');

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
  //
  // Every page holds a Web Lock named after itself for as long as it lives:
  // a breadcrumb whose page still holds its lock is another tab at work, not
  // a page the browser closed, and "Clear data" can tell that another tab is
  // playing.
  const PAGE_ID = Math.random().toString(36).slice(2, 10);
  const locks = navigator.locks && navigator.locks.request ? navigator.locks : null;
  if (locks) { try { locks.request('xt-page-' + PAGE_ID, () => new Promise(() => {})).catch(() => {}); } catch (e) { /* not allowed here */ } }
  /** The ids of other live pages of this site; null where that cannot be known. */
  async function otherPages() {
    if (!locks || !locks.query) return null;
    try {
      return ((await locks.query()).held || []).map((l) => l.name)
        .filter((n) => n.startsWith('xt-page-') && n !== 'xt-page-' + PAGE_ID).map((n) => n.slice(8));
    } catch (e) { return null; }
  }
  function crumb(phase, extra) {
    const c = Object.assign({ phase, t: Date.now(), build: CONFIG.build || '', id: PAGE_ID }, extra || {});
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
  // missing: is any game data still to download? (A breadcrumb left in the
  // download phase says nothing once the download is complete.)
  function killedReason(missing) {
    const c = prevCrumb;
    if (!c || c.closed || c.phase === 'quit' || state.prevAlive) return '';
    if (c.phase === 'download' && !missing) return '';
    const at = {
      download: 'while downloading (what had arrived is kept; it carries on from there)',
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
  const SMALL_UPDATE = Number(params.get('small')) || 50e6; // ?small=BYTES: tests
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
    // A split set (format 5): what to fetch before the game starts, and what each map needs.
    const splitOf = (m) => (m && Array.isArray(m.firstPlay) ? { set: m.set || '', firstPlay: m.firstPlay, maps: m.maps || {} } : null);
    let split = splitOf(main.m);
    if (main.m.stock) {
      const stockBase = new URL(params.get('stock') || main.m.stock, dataBase);
      const j = await pointer(stockBase, 'stock.json');
      const sp = (j.profiles && j.profiles[name]) || { manifest: j.manifest };
      const stock = await getManifest(stockBase, sp.manifest);
      ids.push(stock.id);
      defaultMap = defaultMap || stock.m.defaultMap || '';
      split = split || splitOf(stock.m);
      const have = new Set(files.map((f) => f.name));
      for (const f of stock.m.files) if (!have.has(f.name)) files.push(Object.assign({}, f, { base: stockBase.href }));
    }
    if (main.m.chunk) data.chunk = main.m.chunk;
    return { name, ids, files, defaultMap, keep: !!spec.keep, split };
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
    // A set this GPU can no longer draw (another GPU, a driver without ASTC)
    // is not kept.
    const drawable = (name) => !(offered[name].gpu === 'astc' && !(gpu && gpu.astc));
    if (saved && offered[saved] && drawable(saved)) {
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
                concurrency: PARALLEL,
                onBytes: (c, n) => {
                  got.set(c.h, n);
                  noteBytes(c, n);
                  paint();
                  if (n === c.size && main) crumb('download', { done: [...got].filter(([h, v]) => v === sizeOf.get(h)).length, total: list.length });
                },
              });
            };
            if (locks) {
              let mine = false;
              await locks.request('xt-data-download', { ifAvailable: true }, async (l) => { if (l) { mine = true; await fetchAll(); } });
              if (!mine) {
                el('label').textContent = 'Another tab is downloading the game data. This one carries on when it is done.';
                await locks.request('xt-data-download', { signal: controller.signal }, async () => {
                  el('label').textContent = label;
                  await fetchAll();
                });
              }
            } else await fetchAll();
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
  // files are copied into the JS heap: everything but the map packs, and the
  // one map being played.
  const mounts = new Map(); // engine path -> manifest file
  const enginePaths = new Set(); // mounted paths the engine has scanned (fs_rescan)
  const pathOf = (f) => `${BASEDIR}/${f.path}`;

  async function chunkBlobs(f) {
    const blobs = [];
    for (const c of data.chunks(f)) {
      let b;
      try {
        b = await data.be.blob('c-' + c.h);
      } catch (e) {
        // Unreadable now is not proof of damage: keep it, say so.
        throw new Error(`${f.name} could not be read from the browser's storage (${e && e.name}: ${e && e.message}). Reload to try again.`);
      }
      if (b.size !== c.size) {
        await data.be.remove('c-' + c.h); // the wrong size can never be right
        throw new Error(`${f.name} is incomplete in the browser's storage. Reload to download the missing part again.`);
      }
      blobs.push(b);
    }
    return blobs;
  }

  // (The background download and a map being asked for may both want a file.)
  const mounting = new Map();
  function mount(f) {
    const path = pathOf(f);
    if (mounts.has(path)) return Promise.resolve();
    if (!mounting.has(path)) mounting.set(path, mountOne(f).finally(() => mounting.delete(path)));
    return mounting.get(path);
  }
  async function mountOne(f) {
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
    mountedAt.set(path, performance.now());
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
    let here = !f || (enginePaths.has(pathOf(f)) && !state.rescanPending);
    // A split set in memory: with another map's packs still there, go the
    // long way, which drops them.
    if (here && f && prog.on && !state.lazy && otherMapPacks(mapNeeds(name, f)).length) here = false;
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
      // A split set: the shared packs the map needs, then its own pack.
      const need = mapNeeds(name, f);
      let missing = need.length ? hosted(await data.missing(need)) : [];
      if (missing.length) {
        $('xt-map-title').textContent = `Downloading ${name}`;
        if (document.pointerLockElement) document.exitPointerLock();
        // This map first: the background download waits.
        await bgHold();
        try {
          missing = hosted(await data.missing(need));
          wantMap(name, need, missing);
          await runDownload(missing, `Downloading the map ${name}…`, MAP_UI);
        } catch (e) {
          wantDone(name);
          if (maps.pending && maps.pending.name !== name) continue; // another map was asked for meanwhile
          maps.pending = null;
          if (e && e.name === 'AbortError') { toast(`${name}: download cancelled.`); return; }
          throw new Error(`Could not download the map ${name}: ${e.message || e}`);
        } finally {
          bgRelease();
        }
      }
      if (maps.pending && maps.pending.name !== name) continue;
      if (f && !state.lazy) {
        // In memory: one map's packs at a time.
        for (const g of otherMapPacks(need)) { unmount(g); state.rescanPending = true; }
      }
      for (const g of need) { prog.have.add(g.name); await mount(g); }
      wantDone(name);
      await warm(f);
      if (maps.pending && maps.pending.name !== name) continue;
      maps.pending = null;
      maps.warmed = line;
      if (prog.on && state.lazy) {
        // Packs that came after the engine started go to the end of its
        // search path, the map's own pack last; then the command again.
        for (const g of need) announce(g);
        await announced;
        command(line);
        mountReady().catch(() => {});
      } else if (state.rescanPending || (f && !enginePaths.has(pathOf(f)))) rescanAnd(line);
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
  const hotKey = () => 'hot:' + state.profile + (state.folder ? '+folder' : '');
  function hotList() { try { return JSON.parse(store.get(hotKey(), '{}')) || {}; } catch (e) { return {}; } }
  function remember(touched) {
    const hot = hotList();
    for (const [p, ks] of Object.entries(touched)) {
      // Map packs too: the menu reads every map's .mapinfo and level shot
      // from them at start-up.
      if (!mounts.get(p)) continue;
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
    for (const g of mapFile ? mapNeeds(mapFile.map, mapFile).slice().reverse() : []) all(pathOf(g), g.size);
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
    if (arg === 'pause' || arg === 'resume') bgPause(arg === 'pause');
    const going = bg.running && !bg.paused;
    backgroundStatus().then((s) => toast(s.left ? `Game data: ${mb(s.total - s.left)} of ${mb(s.total)} here; ` +
      (going ? 'the rest is downloading while you play.' : bg.paused ? 'the download is paused (web_maps resume).' : 'a map that is missing downloads when played.') : `Game data: all ${mb(s.total)} here.`));
    return '';
  }
  const MAP_UI = { panel: 'xt-map', fill: 'xt-map-fill', bar: 'xt-map-bar', detail: 'xt-map-detail', label: 'xt-map-label', cancel: 'xt-map-cancel' };

  // ---------------------------------------------- progressive download
  //
  // A split set (docs/WEB.md, "Progressive download"; the contract every
  // client follows is docs/DATA_FETCH.md): the game starts once the core
  // group and the "first play" packs are stored; the rest is fetched while
  // it runs, in the packs' priority order (the music, then map by map with
  // the shared packs each needs, then detail and extras), a map that is
  // asked for first. A pack that lands is mounted in place and announced
  // with "fs_mountpack <name>", which adds it at the end of the engine's
  // search path; a map's own pack (the one with its .bsp) only once every
  // other pack the map needs is in, so ".bsp there" means "playable". The
  // engine hears the progress as "touch_data_status ...".
  //
  // Where the packs are copied into memory (no reads in place), or kept in
  // memory (nothing can be stored), only the music follows by itself; a
  // map's packs come when the map is asked for.
  const PARALLEL = Math.max(1, Math.min(6, Number(params.get('parallel')) || 2));
  // (While playing: one. On a 20 Mbit/s line two or three fetched no faster.)
  const BG_PARALLEL = Math.max(1, Math.min(6, Number(params.get('bgparallel')) || 1));
  const prog = {
    on: false, // this start runs a split set
    first: new Set(), // names of the packs a start waits for, besides the core group
    needs: new Map(), // map -> its packs besides the core group, its own pack last
    have: new Set(), // names of the files whole in storage
    counted: [], // the files whose progress the engine hears about
    sizes: new Map(), // their chunks: hash -> size
    got: new Map(), // chunk hash -> bytes here
    want: null, // {name, chunks: [[hash, size]], total}: the map asked for
  };
  const bg = {
    files: [], // what is fetched while the game runs, in order
    storeOnly: false, // an older set is running: the new one is stored, not mounted
    onDemand: false, // only the music by itself (in memory)
    want: '', // the map QuakeC asked for (dataWant)
    running: false, paused: false, held: 0, parkedNow: false, parked: [], wake: null,
    current: null, controller: null, skip: new Set(), whenDone: null,
    state: '', message: '', sent: '', sentAt: 0, timer: 0, mountFailures: 0,
  };
  const isFirst = (f) => f.group === 'core' || prog.first.has(f.name);
  const mapNeeds = (name, f) => (prog.on && prog.needs.get(name)) || (f ? [f] : []);
  const otherMapPacks = (need) => [...mounts.values()].filter((g) => g.group === 'map' && !need.includes(g));
  // A map's own pack waits for the other packs the map needs.
  const mountable = (f) => !(prog.on && f.map && prog.needs.has(f.map)) || prog.needs.get(f.map).every((g) => prog.have.has(g.name));
  const kb = (n) => Math.round(n / 1024);

  function progSetup(profile, files, older) {
    if (!profile.split || older) return;
    const byName = new Map(files.map((f) => [f.name, f]));
    // (With a Godnotic folder holding the packs, only what it lacks is left.)
    prog.on = files.some((f) => f.kind);
    prog.first = new Set(profile.split.firstPlay);
    for (const [map, names] of Object.entries(profile.split.maps)) {
      const list = names.map((n) => byName.get(n));
      if (list.length && list.every(Boolean)) prog.needs.set(normMap(map), list);
    }
  }

  /**
   * A new data set, cut differently (no chunk in common), while the set of
   * the last start is whole in this browser: that one keeps running, with
   * this build's own files, and the new one is fetched in the background;
   * a later start takes the new set once all of it is here. Nothing waits,
   * nothing is asked, and no map goes missing meanwhile. Returns the files
   * to run and the older manifests' ids, or null: a first visit, the same
   * set, a change small enough to wait for, or an older set with holes.
   */
  async function olderSet(profile) {
    if (!profile.split || params.get('older') === '0') return null;
    const ids = (store.get('ids-' + profile.name, '') || '').split(',').filter(Boolean);
    if (!ids.length || ids.join(',') === profile.ids.join(',')) return null;
    const firstNames = new Set(profile.split.firstPlay);
    const first = profile.files.filter((f) => f.hosted !== false && (f.group === 'core' || firstNames.has(f.name)));
    const lack = sum(await data.missing(first));
    if (lack <= SMALL_UPDATE || lack <= first.reduce((n, f) => n + f.size, 0) / 4) return null;
    const old = [];
    const seen = new Set();
    for (const id of ids) {
      const m = await data.loadManifest(id);
      if (!m) return null;
      for (const f of m.files) {
        if (!f.stock || seen.has(f.name)) continue;
        seen.add(f.name);
        old.push(Object.assign({}, f, { hosted: false }));
      }
    }
    if (!old.length || (await data.missing(old)).length) return null;
    return { ids, files: profile.files.filter((f) => !f.stock).concat(old) };
  }

  // The first-visit page of a split set: the size is what the game needs to
  // start. Returns the bytes that follow in the background.
  async function progLead() {
    const rest = sum(hosted(await data.missing(state.files.filter((f) => !isFirst(f)))));
    state.dlRest = rest;
    const lead = $('xt-dl-size').parentNode;
    if (rest && lead && lead.firstChild && lead.lastChild && lead.firstChild !== lead.lastChild &&
      lead.firstChild.nodeType === 3 && lead.lastChild.nodeType === 3) {
      lead.firstChild.textContent = 'To start playing: ';
      lead.lastChild.textContent = `. The other ${mb(rest)} (the music, the other maps) follow while you play. It stays in this browser for the next visit.`;
    }
    return data.kind === 'memory' ? 0 : rest;
  }

  // After the mount: what is left for the background. True when anything is.
  async function progPlan(profile, files, older) {
    if (!older && !prog.on) return false;
    // This build's own files have no priority: they are here before the start.
    const order = (f) => (f.priority === undefined ? -1 : f.priority);
    bg.storeOnly = !!older;
    prog.counted = (older ? profile.files : files).filter((f) => f.hosted !== false);
    let list = prog.counted.slice().sort((a, b) => order(a) - order(b));
    const all = !!older || params.get('allmaps') === '1' || (params.get('allmaps') !== '0' && state.lazy && data.kind !== 'memory');
    if (!all) {
      bg.onDemand = true;
      list = list.filter((f) => f.group === 'music');
    }
    const missing = new Set((await data.missing(prog.counted)).map((c) => c.h));
    for (const f of prog.counted) {
      for (const c of data.chunks(f)) {
        prog.sizes.set(c.h, c.size);
        if (!missing.has(c.h)) prog.got.set(c.h, c.size);
      }
      if (!f.chunks.some((h) => missing.has(h))) prog.have.add(f.name);
    }
    bg.files = list.filter((f) => !prog.have.has(f.name));
    return missing.size > 0;
  }

  function command(text) {
    if (!state.engineUp || !state.started) return;
    try { Module.ccall('xt_web_command', null, ['string'], [text]); } catch (e) { /* the engine is gone */ }
  }

  // Tell the running engine about a pack mounted after it started, in the
  // order asked. The engine looks a file up through a listing of its data
  // directory that it reads again at most once a second (fs.c,
  // FS_DirMayHave): a file younger than that is "not found", so the command
  // waits until the file is a little over a second old. (Zero once
  // fs_mountpack drops that listing itself.)
  const ANNOUNCE_AFTER_MS = 1100;
  // The music pack brings the track list, cdtracks.cfg, which the start-up
  // configs ran while it was not there yet ("cd loop <number>", as the maps
  // ask for their music, needs it): run it again the way they do
  // (xonotic-common.cfg, xonotic-client.cfg).
  const CDTRACKS = [
    'set _cdtrack_first "1"',
    'alias _cdtrack_0 "g_cdtracks_remaplist \\"$g_cdtracks_remaplist $1\\""',
    'alias _cdtrack_1 "g_cdtracks_remaplist \\"$1\\"; set _cdtrack_first 0"',
    'alias _cdtrack "_cdtrack_$_cdtrack_first $2"',
    'set g_cdtracks_remaplist ""',
    'exec cdtracks.cfg',
    'unset _cdtrack_first', 'unalias _cdtrack_0', 'unalias _cdtrack_1', 'unalias _cdtrack',
    'cd remap $g_cdtracks_remaplist',
  ].join('\n');
  const mountedAt = new Map(); // engine path -> when the file appeared
  const announcing = new Set();
  let announced = Promise.resolve();
  function announce(f) {
    const path = pathOf(f);
    if (enginePaths.has(path) || announcing.has(path) || !mounts.has(path)) return announced;
    announcing.add(path);
    announced = announced.then(async () => {
      const wait = (mountedAt.get(path) || 0) + ANNOUNCE_AFTER_MS - performance.now();
      if (state.engineUp && wait > 0) await new Promise((r) => setTimeout(r, wait));
      announcing.delete(path);
      if (!mounts.has(path)) return;
      enginePaths.add(path);
      if (/\.pk3$/i.test(f.name)) command('fs_mountpack ' + f.name);
      if (f.kind === 'music') command(CDTRACKS);
    });
    return announced;
  }
  const announcedOrSoon = (f) => enginePaths.has(pathOf(f)) || announcing.has(pathOf(f));

  // Mount and announce what is stored and not mounted yet (reads in place).
  async function mountReady() {
    if (!prog.on || !state.lazy || bg.storeOnly) return;
    for (const f of prog.counted) {
      if (!prog.have.has(f.name) || announcedOrSoon(f)) continue;
      if (f.map && prog.needs.has(f.map) && !prog.needs.get(f.map).every((g) => g === f || announcedOrSoon(g))) continue;
      await mount(f);
      // The pk3's directory, which the engine reads when it mounts the pack:
      // read ahead here, so that read does not cost the game a frame.
      if (state.engineUp && XTLazyFS.tail) { try { await XTLazyFS.tail(pathOf(f), 2 << 20); } catch (e) { /* the engine reads it itself */ } }
      announce(f);
    }
  }

  // ---- what the engine hears (docs/DATA_FETCH.md, "Host to game")
  function noteBytes(c, n) {
    if (!prog.sizes.has(c.h)) return;
    prog.got.set(c.h, n);
    statusSoon();
  }
  function statusLine() {
    let done = 0;
    let total = 0;
    for (const [h, size] of prog.sizes) { total += size; done += Math.min(size, prog.got.get(h) || 0); }
    let line = `touch_data_status state ${bg.state} done_kb ${kb(done)} total_kb ${kb(total)}`;
    const w = prog.want;
    if (w) {
      let d = 0;
      for (const [h, size] of w.chunks) d += Math.min(size, prog.got.get(h) || 0);
      line += ` want ${w.name.replace(/[^A-Za-z0-9_.+-]/g, '_')} want_done_kb ${kb(d)} want_total_kb ${kb(w.total)}`;
    }
    if (bg.message && (bg.state === 'error' || bg.state === 'waiting')) line += ` message "${bg.message.replace(/["\;\r\n]/g, ' ').slice(0, 120)}"`;
    return line;
  }
  // At most twice a second, and only when something changed.
  function statusSoon(now) {
    if (!state.engineUp || !bg.state) return;
    const t = performance.now();
    if (!now && t - bg.sentAt < 500) {
      if (!bg.timer) bg.timer = setTimeout(() => { bg.timer = 0; statusSoon(true); }, 500);
      return;
    }
    const line = statusLine();
    if (line === bg.sent) return;
    bg.sent = line;
    bg.sentAt = t;
    command(line);
  }
  function bgSay(st) {
    if (bg.state === st) return;
    bg.state = st;
    statusSoon(true);
  }
  function wantMap(name, need, missing) {
    if (!prog.on) return;
    prog.want = { name, chunks: missing.map((c) => [c.h, c.size]), total: sum(missing) };
    statusSoon(true);
  }
  function wantDone(name) {
    if (!prog.want || prog.want.name !== name) return;
    prog.want = null;
    statusSoon(true);
  }

  /**
   * The engine wants this map (QuakeC asked for it; sys_wasm.c / the
   * engine's touch_datafetch.c call Module.xt.dataWant): its packs jump the
   * queue. No command is run again here: the engine goes on when the map's
   * .bsp appears. 1 when the map is one of the set's.
   */
  function dataWant(raw) {
    const name = normMap(raw);
    const need = prog.on ? prog.needs.get(name) : null;
    if (!need || !state.engineUp) return 0;
    bg.want = name;
    (async () => {
      const missing = hosted(await data.missing(need));
      if (bg.want !== name) return;
      wantMap(name, need, missing);
      // What is being fetched now is not one of them: let go of it (what
      // arrived of it is kept).
      if (bg.current && !need.includes(bg.current) && bg.controller) bg.controller.abort();
      bgRun();
    })().catch(() => {});
    return 1;
  }

  // ---- the download while the game runs
  /** Stop the background download until bgRelease(): a map is being fetched with its own progress card. */
  async function bgHold() {
    bg.held++;
    if (bg.controller) bg.controller.abort();
    if (bg.running && !bg.parkedNow) await new Promise((r) => bg.parked.push(r));
  }
  function bgWake() {
    const w = bg.wake;
    bg.wake = null;
    if (w) w();
  }
  function bgRelease() {
    if (bg.held && !--bg.held) bgWake();
  }
  // The console's "web_maps pause" and "web_maps resume".
  function bgPause(on) {
    bg.paused = on;
    if (on && bg.controller) bg.controller.abort();
    if (!on) { bgWake(); bgRun(); }
    if (bg.running || !on) bgSay(on ? 'paused' : 'fetching');
  }
  async function bgNext() {
    if (bg.want) {
      const name = bg.want;
      const need = prog.needs.get(name) || [];
      const f = need.find((g) => !prog.have.has(g.name) && !bg.skip.has(g.name));
      if (f) return f;
      bg.want = '';
      try {
        // In memory: mounted now, next to the map being played (the next
        // "map" command drops the packs it does not need).
        if (!state.lazy) for (const g of need) { await mount(g); announce(g); }
        else await mountReady();
        await announced;
      } catch (e) { toast(e.message || String(e), 8000); }
      wantDone(name);
    }
    return bg.files.find((f) => !prog.have.has(f.name) && !bg.skip.has(f.name)) || null;
  }
  async function bgFetch(f, signal) {
    const run = async () => {
      // (Another tab may have stored it while this one waited for the lock.)
      const list = hosted(await data.missing([f]));
      if (list.length) await data.download(list, { urlFor: blobUrl, signal, concurrency: BG_PARALLEL, onBytes: noteBytes });
      for (const c of data.chunks(f)) noteBytes(c, c.size);
    };
    // One tab downloads at a time.
    if (locks) await locks.request('xt-data-download', { signal }, run); else await run();
  }
  // false: stop for this visit.
  async function bgFailed(f, e) {
    const quota = e && e.name === 'QuotaExceededError';
    bg.message = quota ? 'no room left in this browser' : (e && e.message) || String(e);
    if (quota) {
      const est = await estimate();
      problem('The rest of the game data did not fit: the browser stopped the download at its limit for this site' +
        (est && est.quota ? ` (${mb(est.usage || 0)} of ${mb(est.quota)})` : '') + '. What is here plays; a map that is missing downloads when it is played.');
      bgSay('error');
      return false;
    }
    if (e && e.fatal) {
      // Not on the server any more, or damaged twice: the other packs go on.
      bg.skip.add(f.name);
      bgSay('error');
      return true;
    }
    // The connection (the chunk was tried six times over half a minute).
    bgSay(navigator.onLine === false ? 'waiting' : 'error');
    await new Promise((r) => {
      const t = setTimeout(r, 30000);
      bg.wake = () => { clearTimeout(t); r(); };
      window.addEventListener('online', () => bgWake(), { once: true });
    });
    bg.wake = null;
    return true;
  }
  async function bgRun() {
    if (bg.running || !state.engineUp) return;
    bg.running = true;
    let stopped = false;
    try {
      for (;;) {
        if (bg.paused || bg.held) {
          bg.parkedNow = true;
          for (const r of bg.parked.splice(0)) r();
          await new Promise((r) => { bg.wake = r; });
          bg.parkedNow = false;
          continue;
        }
        const f = await bgNext();
        if (!f) break;
        bg.current = f;
        bg.controller = new AbortController();
        bg.message = '';
        bgSay('fetching');
        try {
          await bgFetch(f, bg.controller.signal);
        } catch (e) {
          if (e && e.name === 'AbortError') continue; // paused, held, or another map is wanted
          if (await bgFailed(f, e)) continue;
          stopped = true;
          break;
        } finally {
          bg.current = null;
          bg.controller = null;
        }
        prog.have.add(f.name);
        try {
          await mountReady();
        } catch (e) {
          // A stored pack that cannot be mounted (a chunk gone from under
          // the page): what is not whole any more is fetched again.
          console.warn('Godnotic:', e && e.message || e);
          if (++bg.mountFailures > 5) throw e;
          for (const g of prog.counted) {
            if (!prog.have.has(g.name) || enginePaths.has(pathOf(g)) || !(await data.missing([g])).length) continue;
            prog.have.delete(g.name);
            if (!bg.files.includes(g)) bg.files.push(g);
          }
        }
        statusSoon();
        // The game's frames come first.
        await new Promise((r) => setTimeout(r, 0));
      }
      await announced;
      const whole = prog.counted.every((f) => prog.have.has(f.name));
      if (!stopped) bgSay(whole ? 'complete' : bg.skip.size ? 'error' : 'paused');
      if (whole && bg.whenDone) {
        const done = bg.whenDone;
        bg.whenDone = null;
        done();
      }
    } catch (e) {
      console.warn('Godnotic: the background download stopped:', e);
      bg.message = (e && e.message) || String(e);
      bgSay('error');
    } finally {
      bg.running = false;
      bg.parkedNow = false;
      for (const r of bg.parked.splice(0)) r();
    }
  }
  // The engine is up: say where the data stands, fetch the rest.
  function dataStart() {
    state.engineUp = true;
    const whole = prog.counted.every((f) => prog.have.has(f.name));
    // "legacy": a set that was downloaded whole before the game started.
    bg.state = !prog.counted.length ? 'legacy' : whole ? 'complete' : bg.files.length ? 'fetching' : 'paused';
    statusSoon(true);
    if (bg.files.length) bgRun();
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

  const ua = navigator.userAgent || '';

  // ------------------------------------------------ the installed app
  //
  // A page cannot look at the device's files or at another app's data, so it
  // cannot find a Godnotic install by itself (docs/WEB.md, "An existing
  // install"). What it can do: ask Chromium on Android whether the Godnotic
  // app is installed (getInstalledRelatedApps: the web manifest names the
  // app, the app names this site), and hand over to the app, which has the
  // game data and the settings. Elsewhere there is no way to know, so the
  // first visit offers the hand-over as a plain link.
  const APP_PACKAGE = 'io.github.dixonsolutions.xonotictouch';
  const isAndroid = /Android/i.test(ua);
  const isChromium = !!navigator.userAgentData || /Chrom(e|ium)\//.test(ua);
  const isLinux = !isAndroid && /Linux|X11/i.test(navigator.platform || ua);
  function appHref() {
    if (isAndroid && isChromium) {
      // An intent URL names the package, and comes back here (with ?noapp)
      // when the app is not installed after all.
      const back = new URL(location.href);
      back.searchParams.set('noapp', '1');
      return `intent://open#Intent;scheme=godnotic;package=${APP_PACKAGE};S.browser_fallback_url=${encodeURIComponent(back.href)};end`;
    }
    return 'godnotic://open';
  }
  /** {known, href}: known when the browser says the app is installed. */
  async function nativeApp() {
    if (params.has('noapp')) return null;
    let known = false;
    if (navigator.getInstalledRelatedApps) {
      try { known = (await navigator.getInstalledRelatedApps()).some((a) => a.id === APP_PACKAGE); } catch (e) { /* not top-level, or not allowed */ }
    }
    if (known || isAndroid || isLinux) return { known, href: appHref() };
    return null;
  }

  // ------------------------------------------ an existing Godnotic folder
  //
  // Desktop Chromium only (showDirectoryPicker): the player picks the folder
  // of an installed Godnotic (or Xonotic) once; the handle is kept in
  // IndexedDB and the browser asks, or remembers, the permission on later
  // visits. The stock packs in it (data, maps, music, fonts) are read in
  // place, read only; this build's own files still come from the site.
  const FOLDER_HINT = 'The Flatpak keeps it in ~/.var/app/io.github.dixonSolutions.XonoticTouch/data/xonotic-touch/data (type the path with Ctrl+L: the folder is hidden).';
  /** "data", "maps", "music", "xoncompat", "font-unifont"…: which stock pack a file name is, '' for others. */
  function packKind(name, whole) {
    let m = /^xonotic-[^-]+-(data|maps|music|xoncompat)(-[0-9a-z_.]+)?\.pk3$/i.exec(name);
    if (m) return whole && m[2] ? '' : m[1].toLowerCase();
    m = /^(font-[a-z0-9]+)-[^-]+\.pk3$/i.exec(name);
    return m ? m[1].toLowerCase() : '';
  }
  /** The shallowest directory under root (depth 4) with a data and a maps pk3. */
  async function folderScan(root) {
    let level = [root];
    let seen = 0;
    for (let depth = 0; depth <= 4 && level.length; depth++) {
      const next = [];
      for (const dir of level) {
        const pk3 = new Map();
        let config = null;
        for await (const [name, h] of dir.entries()) {
          if (++seen > 20000) return null;
          if (h.kind === 'file') {
            const kind = packKind(name, true);
            if (kind && (!pk3.has(kind) || name > pk3.get(kind).name)) pk3.set(kind, { name, handle: h });
            if (name === 'config.cfg') config = h;
          } else if (!/\.pk3dir$/i.test(name) && name !== 'dlcache') next.push(h);
        }
        if (pk3.has('data') && pk3.has('maps')) return { dir, pk3, config };
      }
      level = next;
    }
    return null;
  }
  /** A pk3's central directory: null when the file is not a whole zip. */
  async function zipDirectory(file) {
    const tail = new Uint8Array(await file.slice(Math.max(0, file.size - 65557)).arrayBuffer());
    for (let i = tail.length - 22; i >= 0; i--) {
      if (tail[i] !== 0x50 || tail[i + 1] !== 0x4b || tail[i + 2] !== 5 || tail[i + 3] !== 6) continue;
      const v = new DataView(tail.buffer, i);
      const size = v.getUint32(12, true);
      const off = v.getUint32(16, true);
      if (off + size > file.size) return null;
      return new Uint8Array(await file.slice(off, off + size).arrayBuffer());
    }
    return null;
  }
  const hasBytes = (hay, text) => {
    const n = Array.from(text, (c) => c.charCodeAt(0));
    outer: for (let i = 0; i + n.length <= hay.length; i++) {
      for (let j = 0; j < n.length; j++) if (hay[i + j] !== n[j]) continue outer;
      return true;
    }
    return false;
  };
  /** Check a picked folder and open its packs: {name, files: [{kind, name, file, size}], kinds, config}. */
  async function folderOpen(root, gpu) {
    const found = await folderScan(root);
    if (!found) throw new Error(`No Godnotic game data in “${root.name}”: it needs xonotic-…-data.pk3 and xonotic-…-maps.pk3. ${FOLDER_HINT}`);
    const files = [];
    for (const [kind, { name, handle }] of found.pk3) {
      const file = await handle.getFile();
      const dir = await zipDirectory(file);
      if (!dir) throw new Error(`${name} in that folder is not complete (still downloading in the app?)`);
      if (kind === 'data' && hasBytes(dir, '.ktx') && !(gpu && gpu.astc)) {
        throw new Error('That folder holds the phone data set (ASTC textures), which this device’s graphics chip cannot draw');
      }
      files.push({ kind, name, file, size: file.size });
    }
    const big = files.find((x) => x.kind === 'data');
    if (typeof XTLazyFS === 'undefined' || !(await XTLazyFS.probe(big.file))) throw new Error('This browser cannot read game files in place from a folder');
    return { name: root.name, files, kinds: new Set(files.map((x) => x.kind)), config: found.config };
  }
  function folderKv(mode, fn) {
    return new Promise((resolve, reject) => {
      const open = indexedDB.open('xt-folder', 1);
      open.onupgradeneeded = () => open.result.createObjectStore('h');
      open.onerror = () => reject(open.error);
      open.onsuccess = () => {
        const db = open.result;
        const tx = db.transaction('h', mode);
        const rq = fn(tx.objectStore('h'));
        tx.oncomplete = () => { db.close(); resolve(rq && rq.result); };
        tx.onerror = tx.onabort = () => { db.close(); reject(tx.error); };
      };
    });
  }
  /** The folder chosen earlier: opened, or {pending} while the browser wants to be asked again, or null. */
  async function folderAtStart(gpu) {
    if (store.get('folder', '') !== '1' || typeof indexedDB === 'undefined') return null;
    let h = null;
    try { h = await folderKv('readonly', (s) => s.get('root')); } catch (e) { return null; }
    if (!h) { store.del('folder'); return null; }
    let perm = 'granted';
    try { if (h.queryPermission) perm = await h.queryPermission({ mode: 'read' }); } catch (e) { perm = 'denied'; }
    if (perm !== 'granted') return { pending: h, name: h.name };
    try {
      return await folderOpen(h, gpu);
    } catch (e) {
      problem(`The folder “${h.name}” cannot be used: ${e.message || e}. Using the browser’s own copy of the game data instead.`);
      return null;
    }
  }
  function mountFolder() {
    if (!state.lazy) throw new Error('This browser cannot read game files in place from a folder. Stop using the folder in the menu.');
    for (const x of state.folder.files) {
      const path = `${BASEDIR}/data/${x.name}`;
      const parts = [];
      for (let off = 0; off < x.size; off += data.chunk) parts.push(x.file.slice(off, Math.min(x.size, off + data.chunk)));
      XTLazyFS.create(Module.FS, path, x.size, parts, data.chunk);
      mounts.set(path, { group: 'folder', size: x.size, name: x.name, path: 'data/' + x.name });
      enginePaths.add(path);
    }
  }
  // Settings to take over (a config.cfg from the folder, or one the player
  // chose with "Import settings"): written into the userdir before the
  // engine starts.
  function importConfig() {
    const text = store.get('import-config', '');
    if (!text) return;
    store.del('import-config');
    try {
      const path = `${USERDIR}/data/config.cfg`;
      mkdirs(Module.FS, path);
      Module.FS.writeFile(path, text);
      syncFS(true);
      toast('Settings imported.');
    } catch (e) {
      problem('Importing the settings failed: ' + (e && e.message || e));
    }
  }
  const looksLikeConfig = (text) => text.length < 2e6 && /^\s*(seta?|bind)\s/m.test(text);

  // ------------------------------------------- storage: persistence, install

  const isIOS = /iPad|iPhone|iPod/.test(ua) || (/Macintosh/.test(ua) && (navigator.maxTouchPoints || 0) > 1);
  const isSafari = /^((?!chrome|chromium|crios|fxios|android|edg).)*safari/i.test(ua);
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
  // Best-effort storage is the browser's to clear under disk pressure.
  // Persistent storage is the player's choice, never the page's: the switch
  // in the Storage popover calls persist() inside that click and shows what
  // the browser answered (Chrome decides by itself, Firefox asks the player,
  // nothing can take the answer back but the browser's own site settings).
  const PERSIST_NO = /firefox/i.test(navigator.userAgent || '')
    ? 'The browser did not allow it (its question was dismissed or blocked for this site). The game data stays, but the browser may clear it when the device runs short of space.'
    : 'The browser did not allow it. It decides by itself, and says yes for sites that are installed (Install app), bookmarked or used often. The game data stays, but the browser may clear it when the device runs short of space.';
  async function setPersist(sw) {
    const out = $('xt-persist-text');
    const can = !!(navigator.storage && navigator.storage.persist);
    if (!sw.checked) {
      if (await persisted()) {
        sw.checked = true;
        out.textContent = 'Only the browser can turn this off again: in its site settings for this page.';
      }
      return;
    }
    if (!can) { sw.checked = false; out.textContent = 'This browser has no persistent storage.'; return; }
    sw.disabled = true;
    out.textContent = 'Asking the browser…';
    state.persistAnswer = 'pending';
    try { await navigator.storage.persist(); } catch (e) { /* not offered */ }
    const kept = await persisted();
    state.persistAnswer = kept ? 'granted' : 'denied';
    sw.disabled = false;
    sw.checked = kept;
    out.textContent = kept ? 'The browser keeps the game data until you delete it.' : PERSIST_NO;
    if (data) showStorage().catch(() => {});
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
    const total = files.reduce((n, f) => n + f.size, 0);
    const est = await estimate();
    const kept = await persisted();
    let where = (BACKEND_LABEL[data.kind] || data.kind) + (data.why && data.why.length && data.kind !== 'opfs' ? ` (${data.why.join('; ')})` : '');
    const fold = state.folder;
    if (fold) where = `Folder “${fold.name}” (read only, ${mb(fold.files.reduce((n, x) => n + x.size, 0))}) · ${where}`;
    $('xt-st-backend').textContent = where;
    $('xt-folder').hidden = !window.showDirectoryPicker || !!fold || !!state.folderPending;
    $('xt-folder-stop').hidden = !fold && !state.folderPending;
    $('xt-st-quota').textContent = est && est.quota ? `${mb(est.usage || 0)} used of ${mb(est.quota)}` : 'not reported';
    const sw = $('xt-opt-persist');
    $('xt-opt-persist-row').hidden = data.kind === 'memory' || !(navigator.storage && navigator.storage.persist);
    if (!sw.disabled) sw.checked = kept;
    if (!state.persistAnswer) $('xt-persist-text').textContent = kept ? 'The browser keeps the game data until you delete it.' : 'Off: the data stays between visits, but the browser may clear it when the device runs short of space.';
    const last = state.problem || (() => { try { return JSON.parse(store.get('problem', '') || 'null').text; } catch (e) { return ''; } })();
    $('xt-st-error').textContent = last || data.be.lastError || 'None';
    let why = '';
    if (!kept && data.kind !== 'memory') {
      if (isSafari) why = 'Safari deletes it after 7 days without a visit unless the page is on the Home Screen.';
      else if (isIOS) why = 'To keep it, add this page to the Home Screen (Share, then Add to Home Screen) and play from there.';
    }
    $('xt-storage-text').textContent = why;
    // Last: looking at every chunk takes a moment.
    const used = await data.storedBytes(files);
    $('xt-st-stored').textContent = `${mb(used)} of ${mb(total)}` + (state.profile ? ` · ${state.profile === 'astc' ? 'ASTC set' : state.profile === 'full' ? 'full set' : 'standard set'}` : '');
  }

  async function freeSpaceLine(need) {
    let est = null;
    try { est = navigator.storage && navigator.storage.estimate ? await navigator.storage.estimate() : null; } catch (e) { /* none */ }
    const el = $('xt-dl-space');
    if (!est || !est.quota) { el.textContent = ''; return true; }
    const free = est.quota - (est.usage || 0);
    const fits = free > need * 1.05;
    el.textContent = fits ? `${mb(free)} free for this site.` : '';
    if (!fits) problem(`Not enough space: this needs ${mb(need)} and the browser offers ${mb(free)} for this site. Free some disk space${navigator.storage && navigator.storage.persist ? ', or turn on "Keep game data on this device" in the menu (some browsers then allow more)' : ''}.`);
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
      renderSize, setFullscreen, mouseGrab, screenKeyboard, quit, needMap, mapsCommand, dataWant,
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
    dataStart();
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
      data = await XTStore.Store.open({ preferCache: params.get('store') === 'cache', writer: params.get('store') === 'opfs-worker' ? 'worker' : 'auto' });
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
    // A Godnotic folder the player picked (Storage popover): its stock packs
    // are read in place, and only the rest is kept in the browser.
    const fold = await folderAtStart(gpu);
    if (fold && fold.files) state.folder = fold;
    else if (fold && fold.pending) state.folderPending = fold;
    // A new data set while the last one is whole in this browser: that one
    // keeps running, the new one arrives in the background.
    const older = await olderSet(profile);
    const runFiles = older ? older.files : profile.files;
    // (A split set's packs are what the folder's data and maps pk3 hold.)
    const folderKind = (f) => (f.kind ? (f.group === 'music' ? 'music' : 'data') : packKind(f.name));
    const files = state.folder ? runFiles.filter((f) => !(f.stock && state.folder.kinds.has(folderKind(f)))) : runFiles;
    state.files = files;
    state.profile = profile.name;
    progSetup(profile, files, older);
    {
      const others = await otherPages();
      state.prevAlive = !!(others && prevCrumb && prevCrumb.id && others.includes(prevCrumb.id));
    }
    for (const f of files) if (f.group === 'map' && f.map) maps.byName.set(normMap(f.map), f);
    mark('manifest');
    // Claim this release's chunks before downloading any: another tab's
    // prune (an older release) keeps what a recorded manifest lists.
    const refIds = older ? profile.ids.concat(older.ids) : profile.ids;
    data.ref(location.pathname, refIds).catch(() => {});

    // One download with everything in it (every map, the music): nothing
    // to choose. Files the host no longer serves (the full set's maps pk3)
    // only count when they are here.
    const startedKey = profile.name === 'web' ? 'started' : 'started-' + profile.name;
    const idsKey = 'ids-' + profile.name;
    const hadStart = store.get(startedKey, '') === '1' || (profile.name === 'full' && store.get('started', '') === '1');
    // (A split set: what the game needs to start; the rest follows in the
    // background, while it runs.)
    const selected = () => files.filter((f) => f.hosted !== false && (!prog.on || isFirst(f)));

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
    if (!missing.length && killedReason(false)) problem(killedReason(false));

    if (missing.length) {
      // Why the page has to download, in one line (and the console).
      const allChunks = new Set(selected().flatMap((f) => f.chunks)).size;
      const firstInstall = missing.length === allChunks;
      const total = selected().reduce((n, f) => n + f.size, 0);
      // The release this browser last started with: when it is the same one
      // and data is missing, the browser (or the player) removed it; that is
      // not an update.
      // (Not when the last start read its stock packs from a folder that is
      // no longer used: then nothing was removed, it was never here.)
      const leftFolder = store.get('via', '') === 'folder' && !state.folder;
      const sameRelease = hadStart && !leftFolder && store.get(idsKey, '') === profile.ids.join(',');
      // A page closed mid-download explains missing data best; closed at
      // another time, it explains nothing about data that is gone.
      const killed = killedReason(true);
      let reason = prevCrumb && prevCrumb.phase === 'download' ? killed : '';
      if (!reason && data.kind === 'memory') reason = 'Nothing can be stored in this window, so the game data downloads on every visit.';
      // "Verify data" found damage and removed it: say that, not "the browser".
      if (!reason && store.get('damaged', '')) reason = `${mb(sum(missing))} of the game data was damaged and has been removed (Verify data): downloading that part again.`;
      store.del('damaged');
      if (!reason && leftFolder) reason = state.folderPending ? '' : 'The Godnotic folder is no longer used, so the game data it held has to be downloaded.';
      if (!reason && hadStart && !leftFolder && (firstInstall || sameRelease)) {
        reason = (firstInstall ? 'The game data is gone from this browser' : `${mb(sum(missing))} of the game data is gone from this browser`) +
          ': the browser removed it (to free space, or because its site data was deleted). Downloading ' + (firstInstall ? 'it' : 'that part') + ' again.';
      }
      if (!reason && !firstInstall && hadStart) reason = `An update: ${mb(sum(missing))} changed.`;
      if (!reason) reason = killed;
      if (reason) problem(reason);
      // No question for a returning visitor whose data the browser removed
      // (the same release as last time: only what is gone comes again), nor
      // for an update that is small (new game code, a config) or a small
      // part of the whole. The question is for first installs, big updates,
      // and a Godnotic folder waiting for its permission.
      const restore = hadStart && !leftFolder && (firstInstall || sameRelease) && data.kind !== 'memory';
      // (With a Godnotic folder in use, what is left is this build's own
      // files: small, even the first time.)
      const small = (!firstInstall || !!state.folder) && (sum(missing) <= SMALL_UPDATE || (!firstInstall && !leftFolder && sum(missing) <= total / 4));
      if ((restore || small) && !state.folderPending && !params.has('ask')) {
        try {
          await runDownload(missing, restore ? 'Downloading the game data again…' : 'Updating…');
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
          await freeSpaceLine(need + (prog.on ? await progLead() : 0));
          // Where the browser may not keep it, say so before the download.
          $('xt-dl-keep').hidden = data.kind === 'memory' || await persisted();
          // The installed app, or a folder waiting for its permission.
          const app = hadStart ? null : await nativeApp();
          $('xt-app').hidden = !app && !state.folderPending;
          $('xt-app-text').hidden = !(app && app.known);
          $('xt-app-open').hidden = !app;
          if (app) {
            $('xt-app-open').href = app.href;
            $('xt-app-open').textContent = app.known ? 'Open the Godnotic app' : 'Have the Godnotic app? Open it';
            $('xt-app-open').classList.toggle('adw-suggested', app.known);
            $('xt-app-open').classList.toggle('adw-flat', !app.known);
            $('xt-dl-go').classList.toggle('adw-suggested', !app.known);
            state.app = app;
          }
          $('xt-folder-use').hidden = !state.folderPending;
          if (state.folderPending) {
            $('xt-folder-use').textContent = `Use the folder “${state.folderPending.name}”`;
            $('xt-folder-use').onclick = async () => {
              let p = 'denied';
              try { p = await state.folderPending.pending.requestPermission({ mode: 'read' }); } catch (e) { /* dismissed */ }
              if (p === 'granted') location.reload();
              else toast('The browser did not allow reading that folder.');
            };
          }
        };
        await refresh();
        while (missing.length) {
          show('xt-download');
          $('xt-dl-go').focus({ preventScroll: true });
          // ?autodownload (tests): no click.
          if (!params.has('autodownload')) await new Promise((resolve) => { $('xt-dl-go').onclick = resolve; });
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
    }
    mark('data-ready');

    status('Starting…');
    crumb('mount');
    await runtimePromise;
    mark('engine-ready');
    try {
      await mountUserdir();
      importConfig();
      state.lazy = await decideLazy();
      // Lazy: everything stored costs nothing until read. In memory:
      // everything but the map packs (the extra pack too: what no map was
      // traced to may still be used by one); maps when they are played.
      const complete = [];
      const here = await Promise.all(files.map(async (f) => !(await data.missing([f])).length));
      files.forEach((f, i) => { if (here[i]) prog.have.add(f.name); });
      files.forEach((f, i) => {
        if (!here[i]) return;
        // In memory: everything but the map packs (one at a time, when played).
        if (!state.lazy && f.group === 'map') return;
        // A split set: a map's own pack only once everything it needs is here.
        if (!mountable(f)) return;
        complete.push(f);
      });
      await mountData(complete);
      if (state.folder) mountFolder();
    } catch (e) {
      problem('Loading the game data failed: ' + (e.message || e));
      return fail(e.message || String(e));
    }
    mark('mounted');
    state.warming = warm(null).then(() => mark('warmed'));
    store.set(startedKey, 1);
    store.set(idsKey, (older ? older.ids : profile.ids).join(','));
    store.set('via', state.folder ? 'folder' : 'store');
    store.set('profile', profile.name);
    // Every file this start needs was there: drop what no page path that
    // started recently still lists (older releases' chunks, old layouts).
    // (With a download still going in the background: once that is whole.)
    const prune = () => data.prune(location.pathname, refIds).then((r) => { state.pruned = r; }).catch(() => {});
    if (await progPlan(profile, files, older)) bg.whenDone = prune; else prune();

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
      if (r.bad) store.set('damaged', r.bad);
      out.textContent = r.bad
        ? `${r.bad} damaged ${r.bad === 1 ? 'piece was' : 'pieces were'} removed. Reload to download ${r.bad === 1 ? 'it' : 'them'} again.`
        : `All ${r.checked - r.missing} stored pieces are intact` + (r.missing ? ` (${r.missing} not downloaded).` : '.');
    } catch (e) {
      out.textContent = 'Checking failed: ' + (e.message || e);
    }
    btn.disabled = false;
  };
  $('xt-opt-persist').onchange = (e) => { setPersist(e.target).catch(() => {}); };
  $('xt-folder').onclick = async () => {
    const out = $('xt-verify-text');
    let h;
    try {
      h = await window.showDirectoryPicker({ id: 'godnotic-data', mode: 'read' });
    } catch (e) {
      if (!e || e.name !== 'AbortError') out.textContent = 'The browser did not open that folder: ' + (e && e.message || e);
      return;
    }
    out.textContent = 'Looking for the game data…';
    try {
      const f = await folderOpen(h, state.gpu);
      await folderKv('readwrite', (s) => s.put(h, 'root'));
      store.set('folder', 1);
      if (f.config) {
        const text = await (await f.config.getFile()).text();
        const fresh = !store.keys().some((k) => k.startsWith('started'));
        if (looksLikeConfig(text) && (fresh || confirm('Also use the settings (config.cfg) found in that folder? They replace the settings of the game in this browser.'))) store.set('import-config', text);
      }
      out.textContent = `Using ${f.files.map((x) => x.name).join(', ')}. Restarting…`;
      location.reload();
    } catch (e) {
      out.textContent = (e && e.message || String(e)) + '.';
    }
  };
  $('xt-folder-stop').onclick = async () => {
    try { await folderKv('readwrite', (s) => s.delete('root')); } catch (e) { /* gone */ }
    store.del('folder');
    location.reload();
  };
  $('xt-import').onclick = () => $('xt-import-file').click();
  $('xt-import-file').onchange = async (e) => {
    const out = $('xt-verify-text');
    const file = e.target.files && e.target.files[0];
    if (!file) return;
    const text = file.size < 2e6 ? await file.text() : '';
    if (!looksLikeConfig(text)) { out.textContent = 'That is not a Godnotic or Xonotic config.cfg.'; return; }
    if (!confirm('Replace the settings of the game in this browser with this file? The page restarts.')) return;
    store.set('import-config', text);
    location.reload();
  };
  if (params.has('noapp')) setTimeout(() => toast('The Godnotic app is not installed on this device.', 6000), 0);
  $('xt-clear').onclick = async () => {
    const others = await otherPages();
    if (others && others.length) {
      $('xt-verify-text').textContent = 'Godnotic is open in another tab or window. Close it first: clearing the data would stop its game.';
      return;
    }
    if (!confirm('Delete the downloaded game data from this browser? Your settings stay. The next start downloads it again.')) return;
    // The player's own doing: the next visit is a first visit, with its
    // question, not data "removed by the browser" that comes back unasked.
    for (const k of store.keys()) if (/^(started|ids-|crumb$|problem$)/.test(k)) store.del(k);
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
    prog, bg, dataWant,
    folder: { packKind, folderScan, folderOpen, zipDirectory },
    nativeApp,
  };

  main().catch((e) => fail(e && e.message || String(e)));
})();
