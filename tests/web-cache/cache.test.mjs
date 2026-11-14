// Tests for the web data store (web/store.js) and the loader flow in
// web/shell.js, against an in-memory OPFS, an in-memory Cache API and a fake
// data host that answers Range requests. Run: scripts/test-web-cache.sh
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import vm from 'node:vm';

const read = (p) => fs.readFileSync(new URL(p, import.meta.url), 'utf8');
const storeSource = read('../../web/store.js');
const shellSource = read('../../web/shell.js');
const startPoint = '  main().catch((e) => fail(e && e.message || String(e)));';
assert.ok(shellSource.includes(startPoint), 'shell startup point exists');

const CHUNK = 1024 * 1024 * 8; // tests use 8 MiB chunks (2 pieces of 4 MiB)
const PIECE = 4 * 1024 * 1024;
const sha = (b) => crypto.createHash('sha256').update(b).digest('hex');

// ------------------------------------------------------------ fake browser

/** OPFS: a directory of files; a writable commits on close(). */
class FakeDir {
  constructor() { this.files = new Map(); this.dirs = new Map(); this.kind = 'directory'; }
  async getDirectoryHandle(name, { create } = {}) {
    if (!this.dirs.has(name)) {
      if (!create) throw new DOMException('no dir', 'NotFoundError');
      this.dirs.set(name, new FakeDir());
    }
    return this.dirs.get(name);
  }
  async getFileHandle(name, { create } = {}) {
    if (!this.files.has(name)) {
      if (!create) throw new DOMException('no file', 'NotFoundError');
      this.files.set(name, new Uint8Array(0));
    }
    const dir = this;
    return {
      kind: 'file',
      async getFile() {
        if (!dir.files.has(name)) throw new DOMException('gone', 'NotFoundError');
        return new Blob([dir.files.get(name)]);
      },
      async createWritable() {
        const parts = [];
        return {
          async write(b) { parts.push(new Uint8Array(b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength))); },
          async close() { dir.files.set(name, new Uint8Array(await new Blob(parts).arrayBuffer())); },
          async abort() {},
        };
      },
    };
  }
  async removeEntry(name) {
    if (!this.files.delete(name) && !this.dirs.delete(name)) throw new DOMException('gone', 'NotFoundError');
  }
  async *entries() {
    for (const name of [...this.files.keys()]) yield [name, await this.getFileHandle(name)];
    for (const [name, d] of [...this.dirs]) yield [name, d];
  }
}
class FakeFileHandle {}
FakeFileHandle.prototype.createWritable = function () {};

class MemoryCache {
  constructor() { this.map = new Map(); }
  async match(req) {
    const item = this.map.get(String(req.url || req));
    return item && new Response(item.body.slice(), { headers: item.headers });
  }
  async put(req, res) {
    this.map.set(String(req.url || req), { body: new Uint8Array(await res.arrayBuffer()), headers: Object.fromEntries(res.headers.entries()) });
  }
  async delete(req) { return this.map.delete(String(req.url || req)); }
  async keys() { return [...this.map.keys()].map((u) => new Request(u)); }
}
class MemoryCaches {
  constructor() { this.byName = new Map(); }
  async open(name) {
    assert.equal(name, 'xt-data-v1', 'storage name is unchanged');
    if (!this.byName.has(name)) this.byName.set(name, new MemoryCache());
    return this.byName.get(name);
  }
  async delete(name) { return this.byName.delete(name); }
}

/** A browser profile: what survives a reload. */
function profile({ opfs = true } = {}) {
  return { root: opfs ? new FakeDir() : null, caches: new MemoryCaches(), local: new Map() };
}
const opfsNames = (prof) => [...(prof.root.dirs.get('xt-data-v1') || new FakeDir()).files.keys()];

// ----------------------------------------------------------- fake data host

/** Files -> manifest (format 3) and blobs, served with Range support. */
function makeHost(fileList, { chunk = CHUNK } = {}) {
  const blobs = new Map();
  const files = fileList.map(({ name, path, bytes, group = 'core', stock = false, map, hosted = true, kind, priority }) => {
    const chunks = [];
    for (let off = 0; off < bytes.length; off += chunk) {
      const piece = bytes.subarray(off, off + chunk);
      const h = sha(piece);
      blobs.set(h, piece);
      chunks.push(h);
    }
    const f = { name, path: path || 'data/' + name, size: bytes.length, sha256: sha(bytes), rev: sha(bytes).slice(0, 16), group, chunks };
    if (stock) f.stock = true;
    if (map) f.map = map;
    if (kind) { f.kind = kind; f.priority = priority; }
    if (!hosted) { f.hosted = false; for (const h of chunks) blobs.delete(h); }
    return f;
  });
  const text = JSON.stringify({ format: 3, chunk, files });
  const id = sha(Buffer.from(text)).slice(0, 20);
  const host = {
    files, blobs, id, text, path: `m/${id}.json`,
    log: [],
    ignoreRange: false,
    corrupt: new Set(), // chunk hashes served damaged
    hold: null, // (hash, offset) -> true: stall the stream there until aborted
    extra: new Map(), // more manifests: path -> text (profiles)
    fetch: async (input, opts = {}) => {
      const url = new URL(String(input));
      host.log.push({ path: url.pathname, range: opts.headers && opts.headers.Range });
      if (url.pathname.endsWith('/manifest.json')) return new Response(JSON.stringify({ format: 2, manifest: host.path, files: [] }));
      if (url.pathname.endsWith(`/${host.path}`)) return new Response(host.text);
      for (const [p, t] of host.extra) if (url.pathname.endsWith('/' + p)) return new Response(t);
      const m = /\/blobs\/([0-9a-f]{64})\.bin$/.exec(url.pathname);
      if (!m || !blobs.has(m[1])) return new Response('missing', { status: 404 });
      let body = blobs.get(m[1]);
      if (host.corrupt.has(m[1])) { body = body.slice(); body[body.length >> 1] ^= 0xff; }
      let start = 0;
      let status = 200;
      const r = opts.headers && opts.headers.Range;
      if (r && !host.ignoreRange) { start = Number(/bytes=(\d+)-/.exec(r)[1]); status = 206; }
      const hash = m[1];
      const slice = body.subarray(start);
      const signal = opts.signal;
      // Stream in 1 MiB pieces so pause and abort can land mid-chunk.
      let off = 0;
      const stream = new ReadableStream({
        async pull(ctl) {
          if (host.hold && host.hold(hash, start + off)) {
            await new Promise((res, rej) => {
              if (signal.aborted) rej(new DOMException('aborted', 'AbortError'));
              signal.addEventListener('abort', () => rej(new DOMException('aborted', 'AbortError')), { once: true });
            });
          }
          if (off >= slice.length) { ctl.close(); return; }
          const n = Math.min(1 << 20, slice.length - off);
          ctl.enqueue(slice.slice(off, off + n));
          off += n;
        },
      });
      return new Response(stream, { status });
    },
  };
  return host;
}

/**
 * Several profiles on one host (format 4): {name: {files, keep, gpu, defaultMap}}.
 * Blobs are shared (same bytes, same chunk); unhosted files' blobs are not
 * served. Returns the host of the first profile, with host.profiles (the
 * page's config) and host.byProfile (name -> {id, files}).
 */
function makeProfiles(spec, { chunk = CHUNK } = {}) {
  const hosts = {};
  for (const [name, p] of Object.entries(spec)) hosts[name] = makeHost(p.files, { chunk });
  const names = Object.keys(spec);
  const host = hosts[names[0]];
  const config = { default: names[0], profiles: {} };
  host.byProfile = {};
  for (const name of names) {
    const h = hosts[name];
    const doc = { format: 4, chunk, profile: name, files: h.files };
    if (spec[name].defaultMap) doc.defaultMap = spec[name].defaultMap;
    if (spec[name].split) Object.assign(doc, spec[name].split, { format: 5 });
    const text = JSON.stringify(doc);
    const id = sha(Buffer.from(text)).slice(0, 20);
    host.extra.set(`m/${id}.json`, text);
    for (const [k, v] of h.blobs) host.blobs.set(k, v);
    config.profiles[name] = { manifest: `m/${id}.json` };
    if (spec[name].keep) config.profiles[name].keep = true;
    if (spec[name].gpu) config.profiles[name].gpu = spec[name].gpu;
    host.byProfile[name] = { id, files: h.files };
  }
  // Unhosted files of any profile are not served, whichever profile shares them.
  for (const h of Object.values(hosts)) for (const f of h.files) if (f.hosted === false) for (const c of f.chunks) host.blobs.delete(c);
  host.profiles = config;
  return host;
}

// -------------------------------------------------------------- page loader

class Element extends EventTarget {
  constructor(id) {
    super();
    this.id = id;
    this.hidden = false;
    this.disabled = false;
    this.checked = false;
    this.value = '';
    this.textContent = '';
    this.style = {};
    const classes = new Set();
    this.classList = { add: (c) => classes.add(c), remove: (c) => classes.delete(c), toggle: (c, on) => (on ? classes.add(c) : classes.delete(c)), contains: (c) => classes.has(c) };
    this.dataset = {};
  }
  setAttribute() {}
  getBoundingClientRect() { return { width: 1280, height: 720 }; }
  focus() {}
  closest() { return new Element(); }
  appendChild() {}
}
class Canvas extends Element {}
Canvas.prototype.getContext = () => ({ RENDERER: 1, MAX_TEXTURE_SIZE: 2, getParameter: (p) => (p === 1 ? 'Test GPU' : 8192), getExtension: () => null });

/**
 * Load web/store.js and web/shell.js the way the page does, in a fresh
 * context sharing `prof` (storage) with earlier loads: a reload.
 */
function loadPage(prof, host, { search = '', engine = true, persist = false, astc = false, persistHangs = false, build = '', ua = 'test', platform = '', relatedApps, lazyfs = null, lazy = false } = {}) {
  const elements = new Map();
  const element = (id) => {
    if (!elements.has(id)) elements.set(id, id === 'canvas' ? new Canvas(id) : new Element(id));
    return elements.get(id);
  };
  const config = { engine: 'a/godnotic.js', wasm: 'a/godnotic.wasm', manifest: host.path, dataUrl: '' };
  if (host.profiles) config.profiles = host.profiles;
  if (build) config.build = build;
  element('xt-config').textContent = JSON.stringify(config);
  const commands = [];
  const dataStatus = []; // "touch_data_status ..." lines, apart from the other commands
  const shown = [];
  const statuses = [];
  const mounted = new Map();
  const context = {
    URL, URLSearchParams, Request, Response, Headers, ReadableStream, TransformStream, Blob,
    AbortController, DOMException, TextEncoder, TextDecoder, Uint8Array, ArrayBuffer,
    crypto: globalThis.crypto,
    caches: prof.caches,
    fetch: (u, o) => host.fetch(u, o),
    FileSystemFileHandle: prof.root ? FakeFileHandle : undefined,
    performance: { now: () => Date.now(), mark() {} },
    requestAnimationFrame: (f) => setTimeout(f, 0),
    setTimeout: (f, ms) => setTimeout(f, Math.min(ms || 0, 5)), clearTimeout, setInterval: () => 0, clearInterval,
    matchMedia: () => ({ matches: false, addEventListener() {} }),
    localStorage: new Proxy({}, {
      get: (_, k) => ({ getItem: (n) => (prof.local.has(n) ? prof.local.get(n) : null), setItem: (n, v) => prof.local.set(n, String(v)), removeItem: (n) => prof.local.delete(n) })[k],
      ownKeys: () => [...prof.local.keys()],
      getOwnPropertyDescriptor: (_, k) => (prof.local.has(k) ? { enumerable: true, configurable: true, value: prof.local.get(k) } : undefined),
    }),
    navigator: {
      maxTouchPoints: 0, language: 'en', languages: ['en'], userAgent: ua, platform, sendBeacon() {},
      getInstalledRelatedApps: relatedApps ? async () => relatedApps : undefined,
      storage: {
        getDirectory: prof.root ? async () => prof.root : undefined,
        // The browser's answer: persist is what it grants when asked.
        persist: async () => { prof.persistAsked = (prof.persistAsked || 0) + 1; if (persistHangs) await new Promise(() => {}); if (persist) prof.persisted = true; return !!prof.persisted; },
        persisted: async () => !!prof.persisted,
        estimate: async () => ({ usage: 0, quota: 10e9 }),
      },
    },
    location: { href: 'https://game.example/web/' + search, pathname: '/web/', search, reload() {} },
    innerWidth: 1280, innerHeight: 720, devicePixelRatio: 1,
    screen: {},
    isSecureContext: true,
    HTMLCanvasElement: class extends Canvas {
      getContext() {
        return { RENDERER: 1, MAX_TEXTURE_SIZE: 2, getParameter: (p) => (p === 1 ? 'Test GPU' : 8192),
          getExtension: (n) => (n === 'WEBGL_compressed_texture_astc' && astc ? {} : null) };
      }
    },
    document: {
      getElementById: element,
      createElement: (name) => (name === 'canvas' ? new context.HTMLCanvasElement() : new Element()),
      addEventListener() {},
      documentElement: {},
      body: { appendChild() {} },
      activeElement: null,
      fullscreenElement: null,
    },
    addEventListener() {},
    AudioContext: null,
    console: { log() {}, warn() {}, error: console.error },
    confirm: () => true,
    WebAssembly: {},
  };
  // lazy: reads in place work here; a mounted file is its stored chunks.
  if (lazy) {
    lazyfs = {
      BLOCK: 1 << 20, stats: {}, cacheBytes: 0,
      probe: async () => true,
      create: (FS, path, size, blobs) => mounted.set(path, { size, blobs }),
      remove: (FS, path) => mounted.delete(path),
      prefetch: async () => 0, tail: async () => 0, trim() {}, takeTouched: () => ({}),
    };
  }
  if (lazyfs) context.XTLazyFS = lazyfs;
  context.window = context;
  context.self = context;
  vm.createContext(context);
  const shell = shellSource
    .replace('function show(id) {', 'function show(id) { globalThis.__shown.push(id);')
    .replace('function status(text, detail) {', 'function status(text, detail) { globalThis.__status.push(text);')
    .replace('function fail(text) {', 'function fail(text) { globalThis.__status.push("FAIL " + text);')
    // engine: false loads the scripts for their functions only: no main().
    .replace(startPoint, '  globalThis.__page = { main, state, cancelDownload, maps, needMap, mapsCommand, backgroundStatus };\n' + (engine ? startPoint : ''));
  context.__shown = shown;
  context.__status = statuses;
  vm.runInContext(storeSource, context, { filename: 'web/store.js' });
  context.XTStore.Store.prototype.pieceAfterMs = 0; // keep pieces at once, however fast
  vm.runInContext(shell, context, { filename: 'web/shell.js' });
  if (engine) {
    // The engine's runtime comes up: an in-memory FS for the data.
    context.Module.FS = {
      mkdir() {}, mount() {}, syncfs: (_, cb) => cb(null),
      writeFile: (p, b) => mounted.set(p, b),
      unlink: (p) => mounted.delete(p),
    };
    context.Module.IDBFS = {};
    context.Module.ccall = (name, ret, types, args) => {
      if (name !== 'xt_web_command') return;
      if (args[0].startsWith('touch_data_status ')) dataStatus.push(args[0]); else commands.push(args[0]);
    };
    context.Module.callMain = () => { context.__started = true; context.__startLog = host.log.length; };
    context.Module._xt_web_hook_maps = () => 2;
    context.Module._xt_web_refit = () => {};
    context.Module._xt_web_autosave = () => 0;
    context.Module._xt_web_save = () => {};
    setTimeout(() => context.Module.onRuntimeInitialized(), 0);
  }
  return { context, element, shown, statuses, mounted, commands, dataStatus, page: context.__page };
}

const until = async (cond, what, ms = 20000) => {
  const t0 = Date.now();
  while (!cond()) {
    if (Date.now() - t0 > ms) throw new Error('timed out waiting for ' + what);
    await new Promise((r) => setTimeout(r, 2));
  }
};
const failed = (p) => p.statuses.find((s) => s.startsWith('FAIL'));
const ready = (p, what) => until(() => { const f = failed(p); if (f) throw new Error(f); return p.page.state.timing.ready; }, what);
const store = (ctx) => ctx.XTStore.Store.open({ chunk: CHUNK });
// Non-repeating bytes: identical chunks would be stored (and fetched) once.
const fill = (n, seed) => {
  const b = new Uint8Array(n);
  let x = (seed * 2654435761) >>> 0 || 1;
  for (let i = 0; i < n; i++) { x ^= x << 13; x ^= x >>> 17; x ^= x << 5; b[i] = x & 0xff; }
  return b;
};
const blobFetches = (host) => host.log.filter((l) => l.path.includes('/blobs/'));
const dl = (s, list, host, signal = new AbortController().signal, extra = {}) =>
  s.download(list, Object.assign({ urlFor: (c) => `https://game.example/web/data/blobs/${c.h}.bin`, signal, onBytes() {}, fetchImpl: host.fetch }, extra));
let passed = 0;
const test = async (name, fn) => { await fn(); passed++; console.log('ok -', name); };

// The game's files in these tests: two small files, a 3-chunk "maps" pk3
// and a 2-chunk music pk3.
const big = fill(CHUNK * 2 + 12345, 7);
const baseFiles = () => [
  { name: 'zz-progs.pk3', bytes: fill(3000, 1) },
  { name: 'xonotic.cfg', bytes: fill(100, 2) },
  { name: 'maps.pk3', bytes: big, stock: true },
  { name: 'music.pk3', bytes: fill(CHUNK + 5, 9), group: 'music', stock: true },
];

// ------------------------------------------------------------------- tests

await test('first visit asks, downloads each chunk once, mounts; a reload makes no data requests', async () => {
  const prof = profile();
  const host = makeHost(baseFiles());
  const p = loadPage(prof, host);
  await until(() => p.shown.includes('xt-download'), 'download dialog');
  assert.equal(p.element('xt-dl-title').textContent, 'Download Godnotic');
  p.element('xt-dl-go').onclick();
  await ready(p, 'ready');
  assert.ok(p.statuses.includes('Starting…'), 'main reached "Starting…"');
  assert.equal(p.mounted.size, 4, 'all files mounted');
  assert.equal(sha(p.mounted.get('/game/data/maps.pk3')), host.files[2].sha256, 'assembled from chunks');
  assert.equal(blobFetches(host).length, 7, 'one request per chunk');
  assert.ok(!prof.persistAsked, 'the page never asks for persistent storage by itself');
  host.log.length = 0;
  const q = loadPage(prof, host);
  await ready(q, 'warm ready');
  assert.ok(!q.shown.includes('xt-download') && !q.shown.includes('xt-progress'), 'no dialog on reload');
  assert.equal(host.log.length, 0, 'warm start: zero data requests (manifest from storage too)');
  assert.ok(!prof.persistAsked, 'nor on a warm start');
});

await test('changed game code: only that chunk, fetched without asking; the old one is pruned', async () => {
  const prof = profile();
  const host = makeHost(baseFiles());
  const p = loadPage(prof, host);
  await until(() => p.shown.includes('xt-download'), 'dialog');
  p.element('xt-dl-go').onclick();
  await ready(p, 'ready');
  const files = baseFiles();
  files[0].bytes = fill(3000, 42); // new progs
  const host2 = makeHost(files);
  const q = loadPage(prof, host2);
  await ready(q, 'ready after update');
  assert.ok(!q.shown.includes('xt-download'), 'small update: no question');
  assert.ok(q.shown.includes('xt-progress'), 'small update: progress shown');
  assert.deepEqual(host2.log.map((l) => l.path.split('/').pop()), [host2.path.slice(2), host2.files[0].chunks[0] + '.bin'],
    'requests: the new manifest and the progs chunk');
  await until(() => q.page.state.pruned, 'prune');
  // A tab may still run the older release (reading its files in place): its
  // chunks stay two days, then go.
  assert.ok(opfsNames(prof).includes('c-' + host.files[0].chunks[0]), 'old progs chunk kept for a tab still running it');
  const dir = prof.root.dirs.get('xt-data-v1');
  for (const [n, b] of dir.files) {
    if (!n.startsWith('r-')) continue;
    const r = JSON.parse(new TextDecoder().decode(b));
    if (r.ids.includes(host.id)) { r.t -= 3 * 24 * 3600 * 1000; dir.files.set(n, new TextEncoder().encode(JSON.stringify(r))); }
  }
  const s3 = await store(loadPage(prof, host2, { engine: false }).context);
  await s3.prune('/web/', [host2.id]);
  const names = opfsNames(prof);
  assert.ok(!names.includes('c-' + host.files[0].chunks[0]), 'old progs chunk pruned two days later');
  assert.ok(names.includes('c-' + host2.files[0].chunks[0]), 'new progs chunk kept');
  assert.ok(names.includes('c-' + host.files[2].chunks[0]), 'unchanged chunks kept');
});

await test('a large update still asks', async () => {
  const prof = profile();
  const files = baseFiles();
  const host = makeHost(files.slice(0, 2));
  const p = loadPage(prof, host);
  await until(() => p.shown.includes('xt-download'), 'dialog');
  p.element('xt-dl-go').onclick();
  await ready(p, 'ready');
  files.push({ name: 'new-maps.pk3', bytes: fill(60e6, 3) }); // over the 50 MB line
  const host2 = makeHost(files.slice(0, 2).concat(files.slice(4)));
  const q = loadPage(prof, host2);
  await until(() => q.shown.includes('xt-download'), 'dialog for a large delta');
  assert.equal(q.element('xt-dl-title').textContent, 'Finish downloading Godnotic');
});

await test('pause keeps finished chunks and 4 MiB pieces; a reload resumes with Range', async () => {
  const prof = profile();
  const host = makeHost(baseFiles());
  const maps = host.files[2];
  host.hold = (h, off) => h === maps.chunks[1] && off >= 5 * 1024 * 1024; // after one piece
  const s = await store(loadPage(prof, host, { engine: false }).context);
  const ctl = new AbortController();
  const list = s.chunks(maps);
  const run = dl(s, list, host, ctl.signal, { concurrency: 1 });
  await until(() => opfsNames(prof).includes(`p-${maps.chunks[1]}-0`), 'piece written');
  ctl.abort();
  await assert.rejects(run, { name: 'AbortError' });
  assert.equal(await s.has(list[0]), true, 'chunk 0 committed');
  assert.equal(await s.has(list[1]), false, 'chunk 1 not committed');
  host.hold = null;
  host.log.length = 0;
  const s2 = await store(loadPage(prof, host, { engine: false }).context); // reload
  assert.equal(await s2.resumable(list[1]), PIECE, 'one piece to resume from');
  const missing = await s2.missing([maps]);
  assert.equal(missing.length, 2, 'chunks 1 and 2 missing');
  await dl(s2, missing, host);
  const reqs = blobFetches(host);
  assert.equal(reqs.length, 2);
  assert.equal(reqs.find((l) => l.path.includes(maps.chunks[1])).range, `bytes=${PIECE}-`, 'chunk 1 resumed from its piece');
  assert.equal(sha(await s2.read(maps)), maps.sha256, 'assembled file matches');
  assert.deepEqual(opfsNames(prof).filter((n) => n.startsWith('p-')), [], 'pieces removed after commit');
});

await test('pause and resume in the page keep going from the same bytes', async () => {
  const prof = profile();
  const host = makeHost(baseFiles());
  const maps = host.files[2];
  let stall = true;
  host.hold = (h, off) => stall && h === maps.chunks[0] && off >= 5 * 1024 * 1024;
  const p = loadPage(prof, host);
  await until(() => p.shown.includes('xt-download'), 'dialog');
  p.element('xt-dl-go').onclick();
  await until(() => opfsNames(prof).includes(`p-${maps.chunks[0]}-0`), 'piece');
  p.element('xt-dl-pause').onclick(); // pause
  await until(() => p.element('xt-dl-pause').textContent === 'Resume', 'paused');
  assert.equal(p.element('xt-progress-label').textContent, 'Paused');
  stall = false;
  p.element('xt-dl-pause').onclick(); // resume
  await ready(p, 'ready after resume');
  assert.ok(blobFetches(host).some((l) => l.path.includes(maps.chunks[0]) && l.range === `bytes=${PIECE}-`), 'resumed with Range');
});

await test('a server that ignores Range still gives a correct chunk', async () => {
  const prof = profile();
  const host = makeHost(baseFiles());
  const maps = host.files[2];
  host.hold = (h, off) => h === maps.chunks[0] && off >= 5 * 1024 * 1024;
  const s = await store(loadPage(prof, host, { engine: false }).context);
  const ctl = new AbortController();
  const run = dl(s, s.chunks(maps).slice(0, 1), host, ctl.signal);
  await until(() => opfsNames(prof).includes(`p-${maps.chunks[0]}-0`), 'piece');
  ctl.abort();
  await assert.rejects(run);
  host.hold = null;
  host.ignoreRange = true;
  const s2 = await store(loadPage(prof, host, { engine: false }).context);
  await dl(s2, s2.chunks(maps).slice(0, 1), host);
  assert.equal(sha(new Uint8Array(await (await s2.be.blob('c-' + maps.chunks[0])).arrayBuffer())), maps.chunks[0]);
});

await test('a damaged chunk is never stored: retried, then a clear error; Verify finds damage on disk', async () => {
  const prof = profile();
  const host = makeHost(baseFiles());
  const cfg = host.files[1];
  host.corrupt.add(cfg.chunks[0]);
  const s = await store(loadPage(prof, host, { engine: false }).context);
  await assert.rejects(dl(s, s.chunks(cfg), host), /damaged/);
  assert.equal(await s.has(s.chunks(cfg)[0]), false, 'nothing committed');
  assert.equal(blobFetches(host).length, 2, 'tried twice');
  host.corrupt.clear();
  await dl(s, s.chunks(cfg), host);
  assert.equal(await s.has(s.chunks(cfg)[0]), true);
  prof.root.dirs.get('xt-data-v1').files.get('c-' + cfg.chunks[0])[0] ^= 1;
  const r = await s.verify([cfg]);
  assert.equal(r.bad, 1);
  assert.equal(await s.has(s.chunks(cfg)[0]), false, 'damaged chunk removed');
});

await test('downloads from the older layout are adopted, not downloaded (OPFS files, Cache parts)', async () => {
  const prof = profile();
  const host = makeHost(baseFiles());
  const [progs, cfg, maps, music] = host.files;
  const legacyDir = await prof.root.getDirectoryHandle('xt-data-v1', { create: true });
  const legacyName = (f) => `${f.rev}-${f.path.replace(/[^A-Za-z0-9._-]/g, '_')}`;
  const src = Object.fromEntries(baseFiles().map((f) => [f.name, f.bytes]));
  // The older page, OPFS: whole files named "<rev>-<path>", and a stale one.
  legacyDir.files.set(legacyName(maps), src['maps.pk3'].slice());
  legacyDir.files.set(legacyName(cfg), src['xonotic.cfg'].slice());
  legacyDir.files.set('0123456789abcdef-data_old.pk3', new Uint8Array(10));
  // The older page, Cache API (Safari before OPFS was writable): parts keyed
  // by the data URL, on a host that has since been renamed.
  const cache = await prof.caches.open('xt-data-v1');
  const m = src['music.pk3'];
  for (let i = 0; i * CHUNK < m.length; i++) {
    await cache.put(`https://old-host.example/data/${music.path}?rev=${music.rev}&part=${i}`,
      new Response(m.slice(i * CHUNK, (i + 1) * CHUNK), { headers: { 'x-xt-size': String(m.length) } }));
  }
  const p = loadPage(prof, host);
  await ready(p, 'ready');
  assert.ok(!p.shown.includes('xt-download'), 'only progs (3 KB) was new: no dialog');
  assert.deepEqual(blobFetches(host).map((l) => l.path.split('/').pop()), [progs.chunks[0] + '.bin'], 'only the file the old store lacked');
  assert.equal(sha(p.mounted.get('/game/data/music.pk3')), music.sha256);
  await until(() => p.page.state.pruned, 'prune');
  const names = opfsNames(prof);
  assert.ok(!names.includes(legacyName(maps)), 'adopted legacy file removed');
  assert.ok(!names.includes('0123456789abcdef-data_old.pk3'), 'stale legacy file pruned');
  assert.equal((await cache.keys()).length, 0, 'adopted Cache parts removed after the move');
});

await test('the Cache API fallback: host-independent keys, adopts older parts, warm start offline', async () => {
  const prof = profile({ opfs: false });
  const host = makeHost(baseFiles());
  const cache = await prof.caches.open('xt-data-v1');
  const cfg = host.files[1];
  await cache.put(`https://xonotic-web.vercel.app/data/${cfg.path}?rev=${cfg.rev}&part=0`, new Response(fill(100, 2), { headers: { 'x-xt-size': '100' } }));
  const p = loadPage(prof, host);
  await ready(p, 'ready'); // under 50 MB left once the old part is adopted: no question
  assert.equal(p.page.state.store, 'cache');
  assert.ok(!blobFetches(host).some((l) => l.path.includes(cfg.chunks[0])), 'adopted part not downloaded');
  assert.ok((await cache.keys()).every((r) => r.url.startsWith('https://xt-store.invalid/')), 'keys carry no data host');
  host.log.length = 0;
  const q = loadPage(prof, host);
  await ready(q, 'warm');
  assert.equal(host.log.length, 0, 'warm start from the Cache API: zero requests');
});

await test('prune never deletes what another page path uses, and stops on an unreadable manifest', async () => {
  const prof = profile();
  const hostA = makeHost(baseFiles().slice(0, 2));
  const filesB = baseFiles().slice(0, 2);
  filesB[0].bytes = fill(3000, 77);
  const hostB = makeHost(filesB);
  const s = await store(loadPage(prof, hostA, { engine: false }).context);
  for (const h of [hostA, hostB]) {
    await s.saveManifest(h.id, h.text);
    await dl(s, await s.missing(h.files), h);
  }
  await s.ref('/Godnotic/web/', [hostB.id]);
  await s.prune('/Godnotic/web/', [hostA.id]);
  assert.equal((await s.missing(hostA.files)).length, 0, 'path A keeps its chunks');
  assert.equal((await s.missing(hostB.files)).length, 0, "path B's chunks survive A's prune");
  await s.ref('/elsewhere/', ['ffffffffffffffffffff']);
  const r = await s.prune('/Godnotic/web/', [hostA.id]);
  assert.equal(r.removed, 0);
  assert.match(r.kept, /unreadable/);
});

await test('a tab on a newer release, still downloading, keeps its chunks when an older tab prunes', async () => {
  const prof = profile();
  const hostA = makeHost(baseFiles().slice(0, 2));
  const filesB = baseFiles().slice(0, 2);
  filesB[0].bytes = fill(3000, 55);
  const hostB = makeHost(filesB);
  // Tab B (newer) opened, recorded its manifests and stored one new chunk;
  // it has not started (so has not pruned) yet.
  const b = loadPage(prof, hostB);
  await until(() => b.shown.includes('xt-download') || b.page.state.timing.ready, 'tab B going');
  const s = await store(loadPage(prof, hostA, { engine: false }).context);
  await s.saveManifest(hostA.id, hostA.text);
  await dl(s, await s.missing(hostA.files), hostA);
  await dl(s, s.chunks(hostB.files[0]), hostB);
  await s.prune('/web/', [hostA.id]); // tab A (older) prunes after its start
  assert.ok(opfsNames(prof).includes('c-' + hostB.files[0].chunks[0]), "tab B's new chunk survives");
});

await test('a cancelled start keeps what arrived and never prunes', async () => {
  const prof = profile();
  const host = makeHost(baseFiles());
  const maps = host.files[2];
  host.hold = (h, off) => h === maps.chunks[1] && off >= 1;
  const p = loadPage(prof, host);
  await until(() => p.shown.includes('xt-download'), 'dialog');
  p.element('xt-dl-go').onclick();
  await until(() => opfsNames(prof).includes('c-' + maps.chunks[0]), 'a chunk stored');
  p.page.cancelDownload();
  await until(() => p.shown.filter((x) => x === 'xt-download').length >= 2, 'dialog again');
  assert.equal(p.page.state.pruned, undefined, 'no prune');
  assert.ok(opfsNames(prof).includes('c-' + maps.chunks[0]), 'finished chunk kept');
  assert.equal(p.element('xt-dl-resumed').hidden, false, 'the dialog says what is already downloaded');
});


// ------------------------------------------------- maps on demand, profiles

// A split data set: core files, one pack per map, an extra pack, music.
const splitFiles = (seed = 0) => [
  { name: 'zz-progs.pk3', bytes: fill(3000, 1) },
  { name: 'xonotic-data.pk3', bytes: fill(CHUNK + 100, 21 + seed), stock: true },
  { name: 'maps-0core.pk3', bytes: fill(5000, 22 + seed), stock: true },
  { name: 'maps-stormkeep.pk3', bytes: fill(4000, 23 + seed), stock: true, group: 'map', map: 'stormkeep' },
  { name: 'maps-dance.pk3', bytes: fill(CHUNK + 7, 24 + seed), stock: true, group: 'map', map: 'dance' },
  { name: 'maps-boil.pk3', bytes: fill(6000, 25 + seed), stock: true, group: 'map', map: 'boil' },
  { name: 'maps-0extra.pk3', bytes: fill(7000, 26 + seed), stock: true, group: 'extra' },
  { name: 'music.pk3', bytes: fill(8000, 27 + seed), stock: true, group: 'music' },
];
const fileOf = (host, name) => host.files.find((f) => f.name === name);
const mbText = (n) => (n >= 1e9 ? (n / 1e9).toFixed(2) + ' GB' : Math.round(n / 1e6) + ' MB');
const fetchedNames = (host, upTo = Infinity) => blobFetches({ log: host.log.slice(0, upTo) }).map((l) => {
  const h = l.path.split('/').pop().slice(0, 64);
  for (const fs of Object.values(host.byProfile || { x: { files: host.files } })) for (const f of fs.files) if (f.chunks.includes(h)) return f.name;
  return h;
});
const firstVisit = async (p) => {
  await until(() => p.shown.includes('xt-download'), 'download dialog');
  p.element('xt-dl-go').onclick();
  await ready(p, 'ready');
};

await test('one download with everything; a map pack that went missing is fetched when played', async () => {
  const prof = profile();
  const host = makeProfiles({ web: { files: splitFiles(), defaultMap: 'stormkeep' } });
  const p = loadPage(prof, host);
  await until(() => p.shown.includes('xt-download'), 'dialog');
  const total = host.files.reduce((n, f) => n + f.size, 0);
  assert.equal(p.element('xt-dl-size').textContent, `${Math.round(total / 1e6)} MB`, 'one size: every file, music included');
  p.element('xt-dl-go').onclick();
  await ready(p, 'ready');
  await until(() => p.context.__started, 'the game starts without a Play button');
  assert.deepEqual([...new Set(fetchedNames(host))].sort(), host.files.map((f) => f.name).sort(), 'everything downloaded');
  assert.ok(!p.mounted.has('/game/data/maps-stormkeep.pk3'), 'in memory: map packs mount when played');
  // The engine asks for stormkeep: stored, so mounted, rescanned, run again.
  assert.equal(p.context.Module.xt.needMap('stormkeep', 'map "stormkeep"'), 0);
  await until(() => p.commands.length === 1, 'command again');
  assert.equal(p.commands[0], 'fs_rescan\nmap "stormkeep"');
  assert.ok(p.mounted.has('/game/data/maps-stormkeep.pk3'));
  assert.equal(p.context.Module.xt.needMap('maps/stormkeep.bsp', 'map "stormkeep"'), 1, 'then it goes ahead');
  // dance's pack vanished from storage: fetched with the map card, then run.
  const dance = fileOf(host, 'maps-dance.pk3');
  for (const h of dance.chunks) prof.root.dirs.get('xt-data-v1').files.delete('c-' + h);
  host.log.length = 0;
  assert.equal(p.context.Module.xt.needMap('dance', 'changelevel "dance"'), 0);
  await until(() => p.commands.length === 2, 'dance command');
  assert.equal(p.commands[1], 'fs_rescan\nchangelevel "dance"');
  assert.deepEqual([...new Set(fetchedNames(host))], ['maps-dance.pk3'], 'only the dance pack downloaded');
  assert.equal(p.element('xt-map').hidden, true, 'map card gone');
  assert.ok(!p.mounted.has('/game/data/maps-stormkeep.pk3'), 'in memory: one map pack at a time');
  assert.equal(p.context.Module.xt.needMap('custommap', 'map "custommap"'), 1, 'a map with no pack goes ahead');
  host.log.length = 0;
  const q = loadPage(prof, host);
  await ready(q, 'warm');
  assert.ok(!q.shown.includes('xt-download') && !q.shown.includes('xt-progress'), 'returning: no download UI');
  await until(() => q.context.__started, 'straight into the game');
  assert.equal(host.log.length, 0, 'no data requests');
});

await test('with lazy reads the first download is what the game needs to start; the rest follows while it runs', async () => {
  const prof = profile();
  const host = makeProfiles({ web: { files: splitFiles(), defaultMap: 'stormkeep' } });
  const created = new Set();
  const lazyfs = { probe: async () => true, create: (FS, path) => created.add(path), remove: (FS, path) => created.delete(path),
    takeTouched: () => ({}), prefetch: async () => {}, trim() {}, BLOCK: 1 << 20, stats: {}, cacheBytes: 0 };
  const p = loadPage(prof, host, { lazyfs });
  await until(() => p.shown.includes('xt-download'), 'dialog');
  const first = host.files.filter((f) => f.group === 'core' || f.map === 'stormkeep');
  const later = host.files.filter((f) => !first.includes(f));
  assert.ok(later.length > 1, 'the test data has other maps and music');
  assert.equal(p.element('xt-dl-size').textContent, `${Math.round(first.reduce((n, f) => n + f.size, 0) / 1e6)} MB`, 'the size is what the start needs');
  assert.match(p.element('xt-dl-what').textContent, /follow as you play/);
  p.element('xt-dl-go').onclick();
  await until(() => p.context.__started, 'the game starts');
  assert.deepEqual([...new Set(fetchedNames(host))].sort(), first.map((f) => f.name).sort(), 'only the core and the first map before the start');
  // (The test's timers are short: the background download starts at once.)
  await until(() => p.page.maps.bgDone, 'the rest arrives while the game runs');
  assert.deepEqual([...new Set(fetchedNames(host))].sort(), host.files.map((f) => f.name).sort(), 'everything downloaded');
  for (const f of later) assert.ok(created.has('/game/' + f.path), `${f.name} mounted when it arrived`);
  assert.equal(p.page.state.rescanPending, true, 'the engine sees them at the next map change');
  // The next visit: nothing to ask, nothing to fetch.
  host.log.length = 0;
  const q = loadPage(prof, host, { lazyfs });
  await until(() => q.context.__started, 'straight into the game');
  assert.ok(!q.shown.includes('xt-download') && !q.shown.includes('xt-progress'), 'no dialog');
  assert.equal(blobFetches(host).length, 0, 'no data requests');
  // Stopped before the rest arrived (a closed tab): the start does not wait for it.
  const prof2 = profile();
  const r = loadPage(prof2, host, { lazyfs, search: '?background=0' });
  await firstVisit(r);
  await until(() => r.context.__started, 'started');
  host.log.length = 0;
  const t = loadPage(prof2, host, { lazyfs });
  await until(() => t.context.__started, 'starts with the maps still missing');
  assert.ok(!t.shown.includes('xt-download') && !t.shown.includes('xt-progress'), 'no dialog for what follows in the background');
});

await test('a cancelled map download runs nothing and keeps what arrived', async () => {
  const prof = profile();
  const host = makeProfiles({ web: { files: splitFiles(), defaultMap: 'stormkeep' } });
  const p = loadPage(prof, host);
  await firstVisit(p);
  const dance = fileOf(host, 'maps-dance.pk3');
  for (const h of dance.chunks) prof.root.dirs.get('xt-data-v1').files.delete('c-' + h);
  host.hold = (h) => h === dance.chunks[1];
  assert.equal(p.context.Module.xt.needMap('dance', 'map "dance"'), 0);
  await until(() => opfsNames(prof).includes('c-' + dance.chunks[0]), 'first chunk stored');
  assert.equal(p.element('xt-map').hidden, false, 'map card shown');
  p.element('xt-map-cancel').onclick();
  await until(() => p.element('xt-map').hidden, 'card closed');
  assert.equal(p.commands.length, 0, 'the map command is not run');
  assert.ok(opfsNames(prof).includes('c-' + dance.chunks[0]), 'finished chunk kept');
});

await test('a page closed by the browser mid-download says so next time and carries on', async () => {
  const prof = profile();
  const host = makeProfiles({ web: { files: splitFiles(), defaultMap: 'stormkeep' } });
  const data = fileOf(host, 'xonotic-data.pk3');
  host.hold = (h) => h === data.chunks[1];
  const p = loadPage(prof, host);
  await until(() => p.shown.includes('xt-download'), 'dialog');
  p.element('xt-dl-go').onclick();
  await until(() => opfsNames(prof).includes('c-' + data.chunks[0]), 'a chunk stored');
  const c = JSON.parse(prof.local.get('xt.crumb'));
  assert.equal(c.phase, 'download');
  assert.ok(!c.closed, 'no normal close recorded: the process was killed');
  host.hold = null;
  host.log.length = 0;
  const q = loadPage(prof, host); // the reload after the kill
  await ready(q, 'ready'); // the rest is small here: it carries on without asking
  assert.match(q.page.state.problem, /closed this page while downloading \(what had arrived is kept/, 'the reason is kept for the Storage panel');
  assert.match(JSON.parse(prof.local.get('xt.problem')).text, /closed this page/);
  assert.ok(blobFetches(host).every((l) => !l.path.includes(data.chunks[0])), 'the stored chunk is not fetched again');
});

await test('no storage at all (private window): memory only, says why, still plays', async () => {
  const prof = profile({ opfs: false });
  prof.caches.open = async () => { throw new DOMException('The operation is insecure.', 'SecurityError'); };
  const host = makeProfiles({ web: { files: splitFiles(), defaultMap: 'stormkeep' } });
  const p = loadPage(prof, host);
  await until(() => p.shown.includes('xt-download'), 'dialog');
  assert.equal(p.page.state.store, 'memory');
  assert.match(p.element('xt-dl-reason').textContent, /downloads on every visit/);
  p.element('xt-dl-go').onclick();
  await ready(p, 'ready');
  await until(() => p.context.__started, 'plays');
});

await test('persistent storage is the player\'s switch: never asked by the page, the real answer shown', async () => {
  const flip = async (p, on) => {
    const sw = p.element('xt-opt-persist');
    sw.checked = on;
    p.page.state.persistAnswer = '';
    sw.onchange({ target: sw });
    if (on) await until(() => p.page.state.persistAnswer && p.page.state.persistAnswer !== 'pending', 'the browser answered');
    else await new Promise((r) => setTimeout(r, 20));
    return sw;
  };
  // A browser that says no (Chrome without engagement).
  const prof = profile();
  const host = makeProfiles({ web: { files: splitFiles(), defaultMap: 'stormkeep' } });
  const p = loadPage(prof, host, { persist: false });
  await firstVisit(p);
  await until(() => p.context.__started, 'started');
  assert.ok(!prof.persistAsked, 'download and start: persist() not called');
  let sw = await flip(p, true);
  assert.equal(prof.persistAsked, 1, 'asked once, by the switch');
  assert.equal(sw.checked, false, 'the switch shows the real answer: no');
  assert.match(p.element('xt-persist-text').textContent, /did not allow it/);
  // A browser that says yes.
  const q = loadPage(prof, host, { persist: true });
  await ready(q, 'warm');
  assert.equal(prof.persistAsked, 1, 'a reload does not ask');
  sw = await flip(q, true);
  assert.equal(sw.checked, true);
  assert.match(q.element('xt-persist-text').textContent, /keeps the game data until you delete it/);
  // It cannot be taken back from the page: the switch stays on and says so.
  sw = await flip(q, false);
  assert.equal(sw.checked, true, 'still persisted');
  assert.match(q.element('xt-persist-text').textContent, /Only the browser can turn this off/);
  // A prompt nobody answers (Firefox) blocks nothing.
  const prof2 = profile();
  const r = loadPage(prof2, host, { persistHangs: true });
  await firstVisit(r);
  await until(() => r.context.__started, 'started');
  const sw2 = r.element('xt-opt-persist');
  sw2.checked = true;
  sw2.onchange({ target: sw2 });
  await new Promise((res) => setTimeout(res, 20));
  assert.equal(sw2.disabled, true, 'waiting for the answer');
  assert.equal(r.page.state.started, true, 'the game goes on meanwhile');
});

await test('data the browser removed comes back by itself: only what is gone, no question', async () => {
  const prof = profile();
  const host = makeProfiles({ web: { files: splitFiles(), defaultMap: 'stormkeep' } });
  await firstVisit(loadPage(prof, host));
  // One pack evicted.
  const dance = fileOf(host, 'maps-dance.pk3');
  prof.root.dirs.get('xt-data-v1').files.delete('c-' + dance.chunks[1]);
  host.log.length = 0;
  const q = loadPage(prof, host);
  await ready(q, 'restored');
  assert.ok(!q.shown.includes('xt-download') && !failed(q), 'no question, no error');
  assert.deepEqual(blobFetches(host).map((l) => l.path.split('/').pop()), [dance.chunks[1] + '.bin'], 'only the missing chunk');
  assert.match(q.page.state.problem, /of the game data is gone from this browser: the browser removed it/);
  assert.doesNotMatch(q.page.state.problem, /An update/);
  // Everything evicted, the page's own notes (localStorage) still there.
  prof.root.dirs.delete('xt-data-v1');
  host.log.length = 0;
  const r = loadPage(prof, host);
  await ready(r, 'restored all');
  assert.ok(!r.shown.includes('xt-download') && r.shown.includes('xt-progress'), 'downloads again with progress, no question');
  assert.match(r.page.state.problem, /The game data is gone from this browser/);
  assert.equal(new Set(blobFetches(host).map((l) => l.path)).size, new Set(host.files.flatMap((f) => f.chunks)).size, 'every chunk once');
  // "Clear data" is the player's doing: the next visit asks, as a first one.
  await r.element('xt-clear').onclick();
  const t = loadPage(prof, host);
  await until(() => t.shown.includes('xt-download'), 'question after Clear data');
  assert.equal(t.element('xt-dl-title').textContent, 'Download Godnotic');
  assert.equal(t.element('xt-dl-reason').hidden, true, 'no "removed by the browser" after the player cleared it');
});

await test('a new deploy with the same packs downloads nothing, even under a new manifest', async () => {
  const prof = profile();
  const host = makeHost(baseFiles());
  const p = loadPage(prof, host, { build: '20261101-aaaaaaa' });
  await until(() => p.shown.includes('xt-download'), 'dialog');
  p.element('xt-dl-go').onclick();
  await ready(p, 'ready');
  host.log.length = 0;
  const q = loadPage(prof, host, { build: '20261114-bbbbbbb' });
  await ready(q, 'new build');
  assert.equal(host.log.length, 0, 'a new page build: nothing asked of the data host');
  // The same bytes listed by a new manifest (another order, a renamed file).
  const again = baseFiles().reverse();
  again[0].name = 'music-renamed.pk3';
  const host2 = makeHost(again);
  assert.notEqual(host2.id, host.id);
  const r = loadPage(prof, host2, { build: '20261115-ccccccc' });
  await ready(r, 'new manifest');
  assert.equal(blobFetches(host2).length, 0, 'chunks are named by their content: none fetched again');
  assert.ok(!r.shown.includes('xt-download'));
});

await test('a breadcrumb from a finished download, or from a tab still alive, is not reported as a kill', async () => {
  const prof = profile();
  const host = makeProfiles({ web: { files: splitFiles(), defaultMap: 'stormkeep' } });
  await firstVisit(loadPage(prof, host));
  prof.local.set('xt.crumb', JSON.stringify({ phase: 'download', t: Date.now(), id: 'other', done: 3, total: 9 }));
  prof.local.delete('xt.problem');
  const q = loadPage(prof, host);
  await ready(q, 'warm');
  assert.ok(!q.page.state.problem, 'everything is here: nothing to report');
});

await test('a visitor from when the music was optional gets it with "Updating…", no question', async () => {
  const prof = profile();
  const files = baseFiles();
  const host = makeHost(files);
  const s = await store(loadPage(prof, host, { engine: false }).context);
  await dl(s, await s.missing(host.files.filter((f) => f.group !== 'music')), host);
  prof.local.set('xt.started', '1');
  prof.local.set('xt.music', '0');
  host.log.length = 0;
  const p = loadPage(prof, host);
  await ready(p, 'ready');
  assert.ok(!p.shown.includes('xt-download'), 'no question');
  assert.ok(p.shown.includes('xt-progress'));
  assert.equal(blobFetches(host).length, 2, 'the music arrives');
  assert.ok(!prof.local.has('xt.music'), 'the old choice is forgotten');
  assert.ok(!shellSource.includes('onlyMusic') && !storeSource.includes('chosen or not'), 'no music choice left in the loader');
});

await test('the installed app: offered instead of a download where the browser reports it', async () => {
  const host = makeProfiles({ web: { files: splitFiles(), defaultMap: 'stormkeep' } });
  const android = 'Mozilla/5.0 (Linux; Android 14) AppleWebKit/537.36 Chrome/142.0.0.0 Mobile Safari/537.36';
  const p = loadPage(profile(), host, { ua: android, relatedApps: [{ platform: 'play', id: 'io.github.dixonsolutions.godnotic' }] });
  await until(() => p.shown.includes('xt-download'), 'dialog');
  assert.equal(p.element('xt-app').hidden, false);
  assert.equal(p.element('xt-app-text').hidden, false, 'says the app is installed');
  assert.equal(p.element('xt-app-open').textContent, 'Open the Godnotic app');
  assert.match(p.element('xt-app-open').href, /^intent:\/\/open#Intent;scheme=godnotic;package=io\.github\.dixonsolutions\.godnotic;S\.browser_fallback_url=https%3A%2F%2Fgame\.example%2Fweb%2F%3Fnoapp%3D1;end$/);
  assert.ok(!p.context.__started, 'nothing downloads or starts until the player chooses');
  // Chromium on Android without the app: a plain link, not a claim.
  const q = loadPage(profile(), host, { ua: android, relatedApps: [] });
  await until(() => q.shown.includes('xt-download'), 'dialog');
  assert.equal(q.element('xt-app-text').hidden, true);
  assert.equal(q.element('xt-app-open').textContent, 'Have the Godnotic app? Open it');
  // Back from a hand-over that found no app: no offer.
  const r = loadPage(profile(), host, { ua: android, relatedApps: [], search: '?noapp=1' });
  await until(() => r.shown.includes('xt-download'), 'dialog');
  assert.equal(r.element('xt-app').hidden, true);
  // Desktop Linux (no way to know): the godnotic: link. Windows: nothing.
  const l = loadPage(profile(), host, { ua: 'Mozilla/5.0 (X11; Linux x86_64; rv:140.0) Gecko/20100101 Firefox/140.0', platform: 'Linux x86_64' });
  await until(() => l.shown.includes('xt-download'), 'dialog');
  assert.equal(l.element('xt-app-open').href, 'godnotic://open');
  assert.equal(l.element('xt-app-text').hidden, true);
  const w = loadPage(profile(), host, { ua: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/142.0.0.0', platform: 'Win32' });
  await until(() => w.shown.includes('xt-download'), 'dialog');
  assert.equal(w.element('xt-app').hidden, true);
  // A returning visitor with data is never held up by any of it.
  const prof = profile();
  await firstVisit(loadPage(prof, host, { ua: android, relatedApps: [] }));
  const again = loadPage(prof, host, { ua: android, relatedApps: [{ platform: 'play', id: 'io.github.dixonsolutions.godnotic' }] });
  await ready(again, 'warm');
  assert.ok(!again.shown.includes('xt-download'));
});

// A minimal zip: one stored entry and the end record, so the central
// directory can be found and read.
const zip = (entry, body = fill(2000, 5)) => {
  const name = Buffer.from(entry);
  const local = Buffer.alloc(30); local.writeUInt32LE(0x04034b50, 0); local.writeUInt32LE(body.length, 18); local.writeUInt32LE(body.length, 22); local.writeUInt16LE(name.length, 26);
  const cd = Buffer.alloc(46); cd.writeUInt32LE(0x02014b50, 0); cd.writeUInt32LE(body.length, 20); cd.writeUInt32LE(body.length, 24); cd.writeUInt16LE(name.length, 28);
  const end = Buffer.alloc(22); end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(1, 8); end.writeUInt16LE(1, 10);
  end.writeUInt32LE(46 + name.length, 12); end.writeUInt32LE(30 + name.length + body.length, 16);
  return new Uint8Array(Buffer.concat([local, name, Buffer.from(body), cd, name, end]));
};

await test('an existing Godnotic folder: found below the picked folder, checked before it is used', async () => {
  const host = makeProfiles({ web: { files: splitFiles(), defaultMap: 'stormkeep' } });
  const p = loadPage(profile(), host, { engine: false, lazyfs: { probe: async () => true } });
  const { packKind, folderOpen } = p.context.xt.folder;
  assert.equal(packKind('xonotic-20261107-maps-0core.pk3'), 'maps');
  assert.equal(packKind('xonotic-20261107-maps-0core.pk3', true), '', 'a split pack is not a whole one');
  assert.equal(packKind('xonotic-0.8.6-data.pk3', true), 'data');
  assert.equal(packKind('font-unifont-20261107.pk3', true), 'font-unifont');
  assert.equal(packKind('zz-progs.pk3'), '');
  const root = new FakeDir(); root.name = 'io.github.dixonSolutions.Godnotic';
  const data = await (await (await root.getDirectoryHandle('data', { create: true })).getDirectoryHandle('godnotic', { create: true })).getDirectoryHandle('data', { create: true });
  await root.getDirectoryHandle('cache', { create: true });
  data.files.set('xonotic-20261107-data.pk3', zip('textures/a.tga'));
  data.files.set('xonotic-20261107-maps.pk3', zip('maps/stormkeep.bsp'));
  data.files.set('xonotic-20260101-maps.pk3', zip('maps/old.bsp'));
  data.files.set('xonotic-20261107-music.pk3', zip('sound/cdtracks/a.ogg'));
  data.files.set('zz-progs.pk3', zip('progs.dat'));
  data.files.set('config.cfg', new TextEncoder().encode('seta cl_name "x"\n'));
  const f = await folderOpen(root, { astc: false });
  assert.deepEqual([...f.files].map((x) => x.name).sort(), ['xonotic-20261107-data.pk3', 'xonotic-20261107-maps.pk3', 'xonotic-20261107-music.pk3'], 'the stock packs, the newest of each, nothing of the fork');
  assert.deepEqual([...f.kinds].sort(), ['data', 'maps', 'music']);
  assert.ok(f.config, 'its config.cfg is found');
  // The phone set on a GPU without ASTC.
  data.files.set('xonotic-20261107-data.pk3', zip('textures/a.ktx'));
  await assert.rejects(folderOpen(root, { astc: false }), /phone data set/);
  assert.ok(await folderOpen(root, { astc: true }), 'fine with ASTC');
  // A pack still being written by the app.
  data.files.set('xonotic-20261107-maps.pk3', fill(5000, 3));
  await assert.rejects(folderOpen(root, { astc: true }), /not complete/);
  // Some other folder.
  const other = new FakeDir(); other.name = 'Documents';
  other.files.set('notes.txt', new Uint8Array(3));
  await assert.rejects(folderOpen(other, { astc: true }), /No Godnotic game data in “Documents”.*\.var\/app/);
});

await test('a saved ASTC profile is dropped when the GPU no longer has ASTC', async () => {
  const prof = profile();
  const host = makeProfiles({ web: { files: splitFiles(0) }, astc: { files: splitFiles(100), gpu: 'astc' } });
  await firstVisit(loadPage(prof, host, { astc: true }));
  assert.equal(prof.local.get('xt.profile'), 'astc');
  const q = loadPage(prof, host, { astc: false });
  await ready(q, 'ready on the standard set'); // small here, so it downloads without a question
  assert.equal(q.page.state.profile, 'web');
  assert.ok(q.mounted.has('/game/data/xonotic-data.pk3'));
});

await test('profile by GPU: ASTC gets the slim set, others the standard one; the choice is kept', async () => {
  const spec = { web: { files: splitFiles(0), defaultMap: 'stormkeep' }, astc: { files: splitFiles(100), defaultMap: 'stormkeep', gpu: 'astc' } };
  const host = makeProfiles(spec);
  const prof = profile();
  const p = loadPage(prof, host, { astc: true });
  await firstVisit(p);
  assert.equal(p.page.state.profile, 'astc');
  assert.ok(fetchedNames(host).every((n) => host.byProfile.astc.files.some((f) => f.name === n)), 'astc files');
  assert.equal(sha(p.mounted.get('/game/data/xonotic-data.pk3')), host.byProfile.astc.files[1].sha256, 'the astc data pk3 mounted');
  const prof2 = profile();
  const q = loadPage(prof2, host, { astc: false });
  await firstVisit(q);
  assert.equal(q.page.state.profile, 'web');
  host.log.length = 0;
  const r = loadPage(prof, host, { astc: true });
  await ready(r, 'warm');
  assert.equal(r.page.state.profile, 'astc', 'kept');
  assert.equal(host.log.length, 0, 'warm start: zero requests ' + JSON.stringify(host.log));
});

await test('no forced migration: a visitor holding the full stock set keeps it and downloads nothing big', async () => {
  // The old layout: the stock maps pk3 whole (format 3), which the new host
  // no longer serves ("hosted": false in the full profile).
  const stockMaps = fill(CHUNK + 999, 31);
  const old = makeHost([
    { name: 'zz-progs.pk3', bytes: fill(3000, 1) },
    { name: 'xonotic-data.pk3', bytes: fill(CHUNK + 100, 21), stock: true },
    { name: 'xonotic-maps.pk3', bytes: stockMaps, stock: true },
  ]);
  const prof = profile();
  const p = loadPage(prof, old);
  await firstVisit(p);
  prof.local.delete('xt.profile'); // pages before profiles kept no choice
  const files = splitFiles();
  files[0].bytes = fill(3000, 99); // a new release: new game code
  const host = makeProfiles({
    web: { files, defaultMap: 'stormkeep' },
    full: { files: [files[0], files[1], { name: 'xonotic-maps.pk3', bytes: stockMaps, stock: true, hosted: false }], keep: true },
  });
  host.log.length = 0;
  const q = loadPage(prof, host);
  await ready(q, 'ready');
  assert.equal(q.page.state.profile, 'full', 'kept the full set');
  assert.ok(!q.shown.includes('xt-download'), 'no question');
  assert.deepEqual(fetchedNames(host), ['zz-progs.pk3'], 'only the new game code ' + JSON.stringify(fetchedNames(host)));
  assert.ok(q.mounted.has('/game/data/xonotic-maps.pk3'), 'the stock maps pk3 mounted');
  // A first-time visitor never gets the full set (it is not hosted).
  const r = loadPage(profile(), host);
  await firstVisit(r);
  assert.equal(r.page.state.profile, 'web');
});

await test('a full set missing its unhosted part falls back to the standard profile', async () => {
  const stockMaps = fill(CHUNK + 999, 32);
  const files = splitFiles();
  const host = makeProfiles({
    web: { files, defaultMap: 'stormkeep' },
    full: { files: [files[0], files[1], { name: 'xonotic-maps.pk3', bytes: stockMaps, stock: true, hosted: false }], keep: true },
  });
  const prof = profile();
  // Some data from an interrupted older download: the data pk3 only.
  const s = await store(loadPage(prof, host, { engine: false }).context);
  await dl(s, s.chunks(fileOf(host, 'xonotic-data.pk3')), host);
  host.log.length = 0;
  const p = loadPage(prof, host);
  await ready(p, 'ready'); // the rest is under 50 MB here: fetched without asking
  assert.equal(p.page.state.profile, 'web');
  assert.ok(!fetchedNames(host).includes('xonotic-data.pk3'), 'the data pk3 it had is reused');
  assert.ok(!host.log.some((l) => /404/.test(l.path)), 'never asks for unhosted data');
});

// ------------------------------------------------- the split set (progressive)

// A split set as scripts/stage-web-data.sh stages it (format 5): this
// build's files, a core pack, shared packs, one pack per map, music, detail
// and extras, with what a first start waits for and what each map needs.
const N = (n) => `godnotic-t-${n}.pk3`;
const setFiles = (seed = 0) => {
  const pack = (name, kind, priority, size, extra = {}) => Object.assign({
    name: N(name), bytes: fill(size, 40 + priority + seed), stock: true, kind, priority,
    group: { core: 'core', map: 'map', shared: 'map', music: 'music' }[kind] || 'extra',
  }, extra);
  return [
    { name: 'zz-progs.pk3', bytes: fill(3000, 1) },
    { name: 'xonotic.cfg', bytes: fill(100, 2) },
    pack('core', 'core', 0, 9000),
    pack('shared-1', 'shared', 1, 2000),
    pack('map-stormkeep', 'map', 2, 4000, { map: 'stormkeep' }),
    pack('music', 'music', 3, CHUNK + 5000),
    pack('shared-2', 'shared', 4, 2500),
    pack('map-boil', 'map', 5, 6000, { map: 'boil' }),
    pack('map-dance', 'map', 6, CHUNK + 7, { map: 'dance' }),
    pack('detail', 'detail', 7, 3000),
    pack('extras', 'extras', 8, 7000),
  ];
};
const setSpec = (seed = 0, files = setFiles(seed)) => ({
  files, defaultMap: 'stormkeep',
  split: {
    set: 'test-' + seed, firstPlay: [N('shared-1'), N('map-stormkeep')],
    maps: { stormkeep: [N('shared-1'), N('map-stormkeep')], boil: [N('shared-1'), N('shared-2'), N('map-boil')], dance: [N('shared-2'), N('map-dance')] },
  },
});
const FIRST = ['zz-progs.pk3', 'xonotic.cfg', N('core'), N('shared-1'), N('map-stormkeep')];
const REST = [N('music'), N('shared-2'), N('map-boil'), N('map-dance'), N('detail'), N('extras')];
const sizeOf = (host, names) => host.files.filter((f) => names.includes(f.name)).reduce((n, f) => n + f.size, 0);
const lastStatus = (p) => p.dataStatus[p.dataStatus.length - 1] || '';
const complete = (p) => until(() => / state complete /.test(lastStatus(p)), 'the whole set');
const mountpacks = (p) => p.commands.filter((c) => c.startsWith('fs_mountpack ')).map((c) => c.slice(13));
const STATUS = /^touch_data_status state (fetching|paused|waiting|error|complete|legacy)( set [A-Za-z0-9_.+-]+)? done_kb \d+ total_kb \d+( want [A-Za-z0-9_.+-]+ want_done_kb \d+ want_total_kb \d+)?( message "[^"]*")?$/;

await test('split set: the game starts after the first-play packs; the rest follows while it runs, announced with fs_mountpack', async () => {
  const prof = profile();
  const host = makeProfiles({ web: setSpec() });
  const p = loadPage(prof, host, { lazy: true });
  await until(() => p.shown.includes('xt-download'), 'dialog');
  assert.equal(p.element('xt-dl-size').textContent, mbText(sizeOf(host, FIRST)), 'the question names the first-play size');
  assert.equal(p.page.state.dlRest, sizeOf(host, REST), 'and knows what follows in the background');
  p.element('xt-dl-go').onclick();
  await until(() => p.context.__started, 'the game starts');
  const before = [...new Set(fetchedNames(host, p.context.__startLog))];
  assert.deepEqual(before.filter((n) => n.startsWith('godnotic') || n.startsWith('zz') || n.endsWith('.cfg')).sort(), FIRST.slice().sort(),
    'requested before the start: the first-play files and nothing else ' + JSON.stringify(before));
  assert.equal(fetchedNames(host, p.context.__startLog).length, FIRST.length, 'one request per first-play chunk');
  assert.ok(/ state fetching /.test(p.dataStatus[0]), 'the engine hears "fetching" at once: ' + p.dataStatus[0]);
  await complete(p);
  for (const l of p.dataStatus) assert.match(l, STATUS);
  assert.deepEqual(mountpacks(p), REST, 'each pack that landed was announced, in priority order (music first)');
  assert.ok(!p.commands.some((c) => c.startsWith('fs_rescan')), 'never fs_rescan');
  for (const n of FIRST.concat(REST)) assert.ok(p.mounted.has('/game/data/' + n), n + ' mounted');
  const total = Math.round(host.files.reduce((n, f) => n + f.size, 0) / 1024);
  assert.equal(lastStatus(p), `touch_data_status state complete set test-0 done_kb ${total} total_kb ${total}`, 'the set id goes with it');
  await until(() => p.page.state.pruned, 'prune, once the set is whole');
  // A reload: everything mounted before the engine starts, nothing asked.
  host.log.length = 0;
  const q = loadPage(prof, host, { lazy: true });
  await until(() => q.context.__started, 'warm start');
  assert.equal(host.log.length, 0, 'no requests');
  assert.equal(q.mounted.size, FIRST.length + REST.length, 'everything mounted before the engine starts');
  await until(() => q.dataStatus.length, 'status');
  assert.ok(/ state complete /.test(q.dataStatus[0]) && !q.commands.length, 'complete, no fs_mountpack');
});

await test('split set: a reload mid-download carries on; a map\'s own pack waits for the packs it needs', async () => {
  const prof = profile();
  const host = makeProfiles({ web: setSpec() });
  // The background stops at the dance pack's second chunk.
  const dance = fileOf(host, N('map-dance'));
  host.hold = (h) => h === dance.chunks[1];
  const p = loadPage(prof, host, { lazy: true });
  await firstVisit(p);
  await until(() => opfsNames(prof).includes('c-' + dance.chunks[0]), 'dance, first chunk');
  await until(() => mountpacks(p).length >= 3, 'the packs before it announced');
  assert.deepEqual(mountpacks(p), [N('music'), N('shared-2'), N('map-boil')], 'what landed so far');
  p.context.Module.xt.mapsCommand('pause'); // the page goes away
  await until(() => / state paused /.test(lastStatus(p)), 'paused');
  // The browser dropped a shared pack meanwhile: boil's own pack is here, shared-2 is not.
  prof.root.dirs.get('xt-data-v1').files.delete('c-' + fileOf(host, N('shared-2')).chunks[0]);
  host.hold = null;
  host.log.length = 0;
  const q = loadPage(prof, host, { lazy: true });
  await until(() => q.context.__started, 'starts with no question');
  assert.ok(!q.shown.includes('xt-download') && !q.shown.includes('xt-progress'), 'no download page: first play is here');
  assert.equal(host.log.slice(0, q.context.__startLog).length, 0, 'nothing fetched before the start');
  assert.ok(q.mounted.has('/game/data/' + N('music')), 'what was stored is mounted at the start');
  assert.ok(!q.mounted.has('/game/data/' + N('map-boil')), 'but not a map\'s own pack while a pack it needs is missing');
  await complete(q);
  assert.deepEqual([...new Set(fetchedNames(host))], [N('shared-2'), N('map-dance'), N('detail'), N('extras')], 'only what was missing');
  assert.deepEqual(mountpacks(q), [N('shared-2'), N('map-boil'), N('map-dance'), N('detail'), N('extras')], 'shared-2, then the maps that waited for it');
  assert.equal(blobFetches(host).filter((l) => l.path.includes(dance.chunks[0])).length, 0, 'the chunk that was stored is not fetched again');
});

await test('split set: a map asked for jumps the queue (needMap runs the command again; dataWant does not)', async () => {
  const prof = profile();
  const host = makeProfiles({ web: setSpec() });
  const music = fileOf(host, N('music'));
  host.hold = (h, off) => h === music.chunks[0] && off >= (1 << 20); // the background sits in the music
  const p = loadPage(prof, host, { lazy: true });
  await firstVisit(p);
  await until(() => blobFetches(host).some((l) => l.path.includes(music.chunks[0])), 'music started');
  host.log.length = 0;
  const xt = p.context.Module.xt;
  // stormkeep is here: warmed, then run.
  assert.equal(xt.needMap('stormkeep', 'map "stormkeep"'), 0);
  await until(() => p.commands.includes('map "stormkeep"'), 'stormkeep');
  assert.equal(xt.needMap('stormkeep', 'map "stormkeep"'), 1);
  assert.equal(host.log.length, 0, 'nothing fetched for a map that is here');
  // boil is not: its packs first, with the card, then the command again.
  assert.equal(xt.needMap('boil', 'changelevel "boil"'), 0);
  await until(() => p.commands.includes('changelevel "boil"'), 'boil');
  assert.deepEqual(p.commands.slice(-3), ['fs_mountpack ' + N('shared-2'), 'fs_mountpack ' + N('map-boil'), 'changelevel "boil"'], 'mounted with fs_mountpack, own pack last, then the command');
  assert.deepEqual([...new Set(fetchedNames(host))].slice(0, 2), [N('shared-2'), N('map-boil')], 'boil\'s packs were fetched next');
  assert.ok(p.dataStatus.some((l) => / want boil want_done_kb \d+ want_total_kb 8$/.test(l)), 'the engine heard about the wanted map ' + JSON.stringify(p.dataStatus));
  assert.ok(!/ want /.test(lastStatus(p)), 'and that it is done');
  assert.equal(p.element('xt-map').hidden, true, 'card gone');
  assert.equal(xt.needMap('boil', 'changelevel "boil"'), 1, 'then it goes ahead');
  // dance, asked for by QuakeC: fetched next, mounted, no command.
  const n = p.commands.length;
  await until(() => blobFetches(host).filter((l) => l.path.includes(music.chunks[0])).length >= 1, 'music again');
  assert.equal(xt.dataWant('dance'), 1);
  assert.equal(xt.dataWant('nosuchmap'), 0);
  await until(() => p.commands.includes('fs_mountpack ' + N('map-dance')), 'dance mounted');
  assert.deepEqual(p.commands.slice(n), ['fs_mountpack ' + N('map-dance')], 'only the mount: the engine runs its own pending command');
  assert.ok(p.dataStatus.some((l) => / want dance /.test(l)));
  assert.ok(!p.mounted.has('/game/data/' + N('extras')), 'the rest is still to come');
  // The rest goes on afterwards.
  xt.mapsCommand('pause');
  host.hold = null;
  xt.mapsCommand('resume');
  await complete(p);
  assert.deepEqual(mountpacks(p).sort(), REST.slice().sort());
});

await test('split set in memory (no reads in place): only the music follows by itself; a map\'s packs come when it is played', async () => {
  const prof = profile();
  const host = makeProfiles({ web: setSpec() });
  const p = loadPage(prof, host);
  await firstVisit(p);
  await until(() => p.context.__started, 'started');
  await until(() => / state paused /.test(lastStatus(p)), 'idle');
  assert.deepEqual([...new Set(fetchedNames(host))].sort(), FIRST.concat(N('music')).sort(), 'first play and the music, nothing else');
  assert.ok(p.mounted.has('/game/data/' + N('core')) && !p.mounted.has('/game/data/' + N('map-stormkeep')), 'in memory: map packs mount when played');
  const xt = p.context.Module.xt;
  assert.equal(xt.needMap('stormkeep', 'map "stormkeep"'), 0);
  await until(() => p.commands.includes('fs_rescan\nmap "stormkeep"'), 'stormkeep');
  assert.ok(p.mounted.has('/game/data/' + N('shared-1')) && p.mounted.has('/game/data/' + N('map-stormkeep')));
  assert.equal(xt.needMap('stormkeep', 'map "stormkeep"'), 1);
  assert.equal(xt.needMap('dance', 'map "dance"'), 0);
  await until(() => p.commands.includes('fs_rescan\nmap "dance"'), 'dance');
  assert.ok(p.mounted.has('/game/data/' + N('shared-2')) && p.mounted.has('/game/data/' + N('map-dance')), 'dance and what it needs');
  assert.ok(!p.mounted.has('/game/data/' + N('shared-1')) && !p.mounted.has('/game/data/' + N('map-stormkeep')), 'the last map\'s packs are dropped');
  assert.ok(!fetchedNames(host).includes(N('extras')) && !fetchedNames(host).includes(N('map-boil')), 'nothing else was fetched');
});

await test('a new set while the old layout is whole: it keeps playing, the new set arrives in the background, the next start uses it', async () => {
  // The layout before the split set: xonotic-data, a maps core, a pack per map.
  const old = makeProfiles({ web: { files: splitFiles(), defaultMap: 'stormkeep' } });
  const prof = profile();
  const first = loadPage(prof, old, { lazy: true });
  await firstVisit(first);
  await until(() => first.page.maps.bgDone, 'the old set whole');
  const oldChunks = old.files.filter((f) => f.stock).flatMap((f) => f.chunks);
  // A deploy with the split set (no chunk in common) and new game code.
  const files = setFiles();
  files[0].bytes = fill(3000, 77);
  const host = makeProfiles({ web: setSpec(0, files) });
  const p = loadPage(prof, host, { lazy: true, search: '?small=1000' });
  await until(() => p.context.__started, 'starts');
  assert.ok(!p.shown.includes('xt-download'), 'no question');
  assert.deepEqual(fetchedNames(host, p.context.__startLog), ['zz-progs.pk3', 'xonotic.cfg'], 'before the start: only this build\'s own files (new game code, a config)');
  assert.ok(p.mounted.has('/game/data/maps-dance.pk3') && p.mounted.has('/game/data/xonotic-data.pk3'), 'the old packs run');
  assert.ok(![...p.mounted.keys()].some((k) => k.includes('godnotic-t-')), 'none of the new set is mounted');
  assert.equal(p.context.Module.xt.needMap('dance', 'map "dance"'), 0);
  await until(() => p.commands.includes('map "dance"'), 'every map it had still plays');
  await complete(p);
  assert.equal(mountpacks(p).length, 0, 'the new set is stored, not mounted');
  assert.deepEqual([...new Set(fetchedNames(host))].sort(), FIRST.concat(REST).sort(), 'the whole new set arrived');
  await until(() => p.page.state.pruned, 'prune');
  for (const h of oldChunks) assert.ok(opfsNames(prof).includes('c-' + h), 'old chunks are kept while that set runs');
  // The next start: the new set, whole, with no request.
  host.log.length = 0;
  const q = loadPage(prof, host, { lazy: true, search: '?small=1000' });
  await until(() => q.context.__started, 'next start');
  assert.equal(host.log.length, 0, 'no requests');
  assert.ok(q.mounted.has('/game/data/' + N('map-dance')) && !q.mounted.has('/game/data/maps-dance.pk3'), 'the split set runs');
  // An old layout with a hole cannot run: the new set's first play, with the question.
  const prof2 = profile();
  const first2 = loadPage(prof2, old, { lazy: true });
  await firstVisit(first2);
  await until(() => first2.page.maps.bgDone, 'the old set whole');
  prof2.root.dirs.get('xt-data-v1').files.delete('c-' + fileOf(old, 'maps-boil.pk3').chunks[0]);
  const same = makeProfiles({ web: setSpec() }); // the game code it already has
  const r = loadPage(prof2, same, { lazy: true, search: '?small=1000' });
  await until(() => r.shown.includes('xt-download'), 'question');
  assert.equal(r.element('xt-dl-title').textContent, 'Finish downloading Godnotic');
  assert.equal(r.page.state.dlRest, sizeOf(same, REST), 'for the first-play packs; the rest follows');
});

await test('a folder holding the split layout: usable with every first-play pack whole; its packs are not downloaded', async () => {
  const host = makeProfiles({ web: setSpec() });
  const p = loadPage(profile(), host, { engine: false, lazyfs: { probe: async () => true } });
  const { folderOpen } = p.context.xt.folder;
  const packs = host.files.filter((f) => f.kind);
  const firstNames = [N('core'), N('shared-1'), N('map-stormkeep')];
  p.page.state.setPacks = { size: new Map(packs.map((f) => [f.name, f.size])), first: firstNames };
  const root = new FakeDir(); root.name = 'data';
  const put = (name, size) => root.files.set(name, fill(size, 5));
  for (const n of firstNames) put(n, fileOf(host, n).size);
  put(N('music'), fileOf(host, N('music')).size);
  put(N('map-boil'), 100); // still downloading in the app: not the manifest's size
  put('godnotic-t-other.pk3', 50); // not a pack of this set
  const f = await folderOpen(root, { astc: false });
  assert.deepEqual([...f.kinds], ['set']);
  assert.deepEqual([...f.files].map((x) => x.name).sort(), firstNames.concat(N('music')).sort(), 'the whole packs of the set, nothing else');
  root.files.set(N('shared-1'), fill(10, 5));
  await assert.rejects(folderOpen(root, { astc: false }), /not all complete/);
  root.files.delete(N('shared-1'));
  await assert.rejects(folderOpen(root, { astc: false }), /No Godnotic game data/);
});

console.log(`web cache: ${passed} tests passed`);
