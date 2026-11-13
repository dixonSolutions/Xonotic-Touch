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
    for (const name of [...this.files.keys()]) yield [name, { kind: 'file' }];
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
  const files = fileList.map(({ name, path, bytes, group = 'core', stock = false }) => {
    const chunks = [];
    for (let off = 0; off < bytes.length; off += chunk) {
      const piece = bytes.subarray(off, off + chunk);
      const h = sha(piece);
      blobs.set(h, piece);
      chunks.push(h);
    }
    const f = { name, path: path || 'data/' + name, size: bytes.length, sha256: sha(bytes), rev: sha(bytes).slice(0, 16), group, chunks };
    if (stock) f.stock = true;
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
    fetch: async (input, opts = {}) => {
      const url = new URL(String(input));
      host.log.push({ path: url.pathname, range: opts.headers && opts.headers.Range });
      if (url.pathname.endsWith('/manifest.json')) return new Response(JSON.stringify({ format: 2, manifest: host.path, files: [] }));
      if (url.pathname.endsWith(`/${host.path}`)) return new Response(host.text);
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
function loadPage(prof, host, { search = '', engine = true, persist = false } = {}) {
  const elements = new Map();
  const element = (id) => {
    if (!elements.has(id)) elements.set(id, id === 'canvas' ? new Canvas(id) : new Element(id));
    return elements.get(id);
  };
  element('xt-config').textContent = JSON.stringify({ engine: 'a/godnotic.js', wasm: 'a/godnotic.wasm', manifest: host.path, dataUrl: '' });
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
    setTimeout: (f, ms) => setTimeout(f, Math.min(ms || 0, 5)), clearTimeout, setInterval: () => 0, clearInterval,
    matchMedia: () => ({ matches: false, addEventListener() {} }),
    localStorage: { getItem: (k) => (prof.local.has(k) ? prof.local.get(k) : null), setItem: (k, v) => prof.local.set(k, String(v)) },
    navigator: {
      maxTouchPoints: 0, language: 'en', languages: ['en'], userAgent: 'test', sendBeacon() {},
      storage: {
        getDirectory: prof.root ? async () => prof.root : undefined,
        persist: async () => { prof.persistAsked = (prof.persistAsked || 0) + 1; return persist; },
        persisted: async () => persist,
        estimate: async () => ({ usage: 0, quota: 10e9 }),
      },
    },
    location: { href: 'https://game.example/web/' + search, pathname: '/web/', search, reload() {} },
    screen: {},
    isSecureContext: true,
    HTMLCanvasElement: Canvas,
    document: {
      getElementById: element,
      createElement: (name) => (name === 'canvas' ? new Canvas() : new Element()),
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
  context.window = context;
  context.self = context;
  vm.createContext(context);
  const shell = shellSource
    .replace('function show(id) {', 'function show(id) { globalThis.__shown.push(id);')
    .replace('function status(text, detail) {', 'function status(text, detail) { globalThis.__status.push(text);')
    .replace('function fail(text) {', 'function fail(text) { globalThis.__status.push("FAIL " + text);')
    // engine: false loads the scripts for their functions only: no main().
    .replace(startPoint, '  globalThis.__page = { main, state, cancelDownload };\n' + (engine ? startPoint : ''));
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
    };
    context.Module.IDBFS = {};
    setTimeout(() => context.Module.onRuntimeInitialized(), 0);
  }
  return { context, element, shown, statuses, mounted, page: context.__page };
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
  assert.equal(p.element('xt-dl-title').textContent, 'Download the game data');
  p.element('xt-dl-go').onclick();
  await ready(p, 'ready');
  assert.ok(p.statuses.includes('Starting the engine…'), 'main reached "Starting the engine…"');
  assert.equal(p.mounted.size, 4, 'all files mounted');
  assert.equal(sha(p.mounted.get('/game/data/maps.pk3')), host.files[2].sha256, 'assembled from chunks');
  assert.equal(blobFetches(host).length, 7, 'one request per chunk');
  assert.ok(prof.persistAsked >= 1, 'persistent storage requested after the download');
  host.log.length = 0;
  const q = loadPage(prof, host);
  await ready(q, 'warm ready');
  assert.ok(!q.shown.includes('xt-download') && !q.shown.includes('xt-progress'), 'no dialog on reload');
  assert.equal(host.log.length, 0, 'warm start: zero data requests (manifest from storage too)');
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
  const names = opfsNames(prof);
  assert.ok(!names.includes('c-' + host.files[0].chunks[0]), 'old progs chunk pruned');
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
  assert.equal(q.element('xt-dl-title').textContent, 'Update the game data');
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
  await s.prune('/Xonotic-Touch/web/', [hostA.id]);
  assert.equal((await s.missing(hostA.files)).length, 0, 'path A keeps its chunks');
  assert.equal((await s.missing(hostB.files)).length, 0, "path B's chunks survive A's prune");
  await s.ref('/elsewhere/', ['ffffffffffffffffffff']);
  const r = await s.prune('/Xonotic-Touch/web/', [hostA.id]);
  assert.equal(r.removed, 0);
  assert.match(r.kept, /unreadable/);
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

console.log(`web cache: ${passed} tests passed`);
