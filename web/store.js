// Godnotic in the browser: the game data store.
//
// The server side (scripts/stage-web-data.sh) cuts every data file into
// 32 MiB chunks and serves each as data/blobs/<sha256>.bin. This file keeps
// those chunks in the browser, one entry per chunk, named by its hash:
//
//   c-<sha256>        a whole chunk, checked against its hash before it is
//                     written (crypto.subtle.digest)
//   p-<sha256>-<n>    4 MiB pieces of a chunk still downloading, so a
//                     download cut off by a reload or a dropped connection
//                     carries on from the last piece (HTTP Range)
//   m-<id>.json       manifests seen, so a start needs no network
//   r-<page path>     which manifests a page path uses, so pruning from one
//                     path never deletes what another path (or an older
//                     tab) still needs
//
// Where: the origin private file system (OPFS) where it is writable (from the
// page, or from a worker where only that can write), else the Cache API (keyed by name, never by the data host, so moving the data to
// another host keeps everything). Both live under "xt-data-v1", the name the
// older layout used: pages from before this layout kept whole files there
// ("<rev>-<path>" in OPFS, "<data url>?rev=&part=" in the Cache API), and
// adopt() turns those into chunks once, checking each against its hash,
// instead of downloading them again.
'use strict';

(function (global) {
  // Do not rename: downloads of every earlier page version live under it.
  const NAME = 'xt-data-v1';
  const PIECE = 4 * 1024 * 1024;
  const KEY_BASE = 'https://xt-store.invalid/';
  const REF_MAX_AGE = 60 * 24 * 3600 * 1000;
  const REF_SUPERSEDED = 2 * 24 * 3600 * 1000;
  const LEGACY = /^([0-9a-f]{16})-(.+)$/;

  const hex = (buf) => Array.from(new Uint8Array(buf), (b) => b.toString(16).padStart(2, '0')).join('');
  const sha256 = async (bytes) => hex(await global.crypto.subtle.digest('SHA-256', bytes));
  const legacyName = (f) => `${f.rev}-${f.path.replace(/[^A-Za-z0-9._-]/g, '_')}`;
  const isAbort = (e) => e && (e.name === 'AbortError' || e.name === 'QuotaExceededError' || e.fatal);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  // ------------------------------------------------------------ backends
  // A flat name -> bytes store: list, size (-1: absent), blob, write, remove.

  // Browsers whose OPFS has no createWritable() on the main thread (Safari
  // before 26) can still write it from a worker, with
  // createSyncAccessHandle(). The worker is made from this text, so the page
  // needs no extra file. It writes in place (truncate, write, flush): a
  // write cut short leaves a file of the wrong size, which the store treats
  // as absent (has() compares sizes) and downloads again.
  const WORKER_SOURCE = `
    let dirp = null;
    onmessage = async (e) => {
      const { id, dir, name, bytes } = e.data;
      try {
        dirp = dirp || navigator.storage.getDirectory().then((r) => r.getDirectoryHandle(dir, { create: true }));
        const fh = await (await dirp).getFileHandle(name, { create: true });
        const h = await fh.createSyncAccessHandle();
        try {
          h.truncate(0);
          let off = 0;
          while (off < bytes.byteLength) {
            const n = h.write(bytes.subarray(off), { at: off });
            if (!n) throw new Error('short write');
            off += n;
          }
          h.flush();
        } finally {
          h.close();
        }
        postMessage({ id });
      } catch (err) {
        postMessage({ id, error: { name: err && err.name || 'Error', message: err && err.message || String(err) } });
      }
    };`;

  function workerWriter() {
    let worker = null;
    let next = 0;
    const waiting = new Map();
    const start = () => {
      const url = global.URL.createObjectURL(new global.Blob([WORKER_SOURCE], { type: 'text/javascript' }));
      worker = new global.Worker(url);
      worker.onmessage = (e) => {
        const w = waiting.get(e.data.id);
        if (!w) return;
        waiting.delete(e.data.id);
        if (e.data.error) {
          const err = new Error(e.data.error.message);
          err.name = e.data.error.name;
          w.reject(err);
        } else w.resolve();
      };
      worker.onerror = (e) => {
        for (const w of waiting.values()) w.reject(new Error('storage worker failed: ' + (e && e.message || 'error')));
        waiting.clear();
        worker = null;
      };
    };
    return (name, bytes) => new Promise((resolve, reject) => {
      if (!worker) start();
      const id = ++next;
      waiting.set(id, { resolve, reject });
      // A copy: the caller reuses its buffer for the next chunk.
      const copy = bytes.slice();
      worker.postMessage({ id, dir: NAME, name, bytes: copy }, [copy.buffer]);
    });
  }

  /**
   * The origin private file system. writer: 'auto' writes with
   * createWritable() on the main thread where it exists (Chrome, Edge,
   * Firefox, Safari 26+) and from a worker with createSyncAccessHandle()
   * otherwise (Safari 15.2-18); 'worker' forces the latter (tests).
   */
  async function opfsBackend(why = [], writer = 'auto') {
    const nav = global.navigator;
    if (!nav.storage || !nav.storage.getDirectory) { why.push('OPFS: not offered'); return null; }
    const FH = global.FileSystemFileHandle;
    const mainWrites = !!(FH && 'createWritable' in FH.prototype);
    const workerWrites = !!(FH && 'createSyncAccessHandle' in FH.prototype && global.Worker && global.URL && global.URL.createObjectURL);
    const useWorker = writer === 'worker' ? workerWrites : !mainWrites && workerWrites;
    if (!mainWrites && !useWorker) { why.push('OPFS: no way to write files here'); return null; }
    let dir;
    try {
      dir = await (await nav.storage.getDirectory()).getDirectoryHandle(NAME, { create: true });
    } catch (e) {
      why.push('OPFS: ' + (e && (e.name + ': ' + e.message) || e)); // Firefox private windows: SecurityError
      return null;
    }
    const files = new Map();
    const viaWorker = useWorker ? workerWriter() : null;
    return {
      kind: 'opfs',
      writer: useWorker ? 'worker' : 'main',
      async list() {
        const names = [];
        for await (const [name, h] of dir.entries()) if (h.kind === 'file') names.push(name);
        return names;
      },
      async size(name) {
        // Not there is -1. Any other failure (the file is locked by a write
        // in another tab, the disk hiccuped) is tried again before the
        // chunk is taken as absent: then it is fetched again and written
        // over, never deleted, and the Storage panel shows the error.
        for (let attempt = 0; ; attempt++) {
          try {
            // Chunks never change once written: keep the File (a snapshot
            // that reads from disk) for the mount that follows the check.
            const f = await (await dir.getFileHandle(name)).getFile();
            if (name.startsWith('c-')) files.set(name, f);
            return f.size;
          } catch (e) {
            if (e && (e.name === 'NotFoundError' || e.name === 'TypeMismatchError')) return -1;
            if (attempt < 2) { await sleep(40 * (attempt + 1)); continue; }
            this.lastError = `${name.slice(0, 14)}…: ${e && e.name}: ${e && e.message}`;
            return -1;
          }
        }
      },
      async blob(name) {
        const f = files.get(name);
        if (f) return f;
        return (await dir.getFileHandle(name)).getFile();
      },
      async write(name, bytes) {
        files.delete(name);
        if (viaWorker) return viaWorker(name, bytes);
        // createWritable() writes a swap file that replaces the entry on
        // close(): a reader never sees half a chunk.
        const w = await (await dir.getFileHandle(name, { create: true })).createWritable();
        try {
          await w.write(bytes);
          await w.close();
        } catch (e) {
          try { await w.abort(); } catch (e2) { /* gone */ }
          throw e;
        }
      },
      async remove(name) { files.delete(name); try { await dir.removeEntry(name); } catch (e) { /* gone */ } },
    };
  }

  async function cacheBackend() {
    if (!global.caches) return null;
    const cache = await global.caches.open(NAME);
    const key = (name) => KEY_BASE + name;
    return {
      kind: 'cache',
      cache,
      async list() {
        return (await cache.keys()).map((r) => r.url).filter((u) => u.startsWith(KEY_BASE)).map((u) => u.slice(KEY_BASE.length));
      },
      async size(name) {
        const r = await cache.match(key(name));
        return r ? Number(r.headers.get('x-xt-size')) : -1;
      },
      async blob(name) {
        const r = await cache.match(key(name));
        if (!r) throw new Error(`${name} is missing from the browser cache`);
        return r.blob();
      },
      async write(name, bytes) {
        await cache.put(key(name), new Response(bytes, {
          headers: { 'content-type': 'application/octet-stream', 'x-xt-size': String(bytes.byteLength) },
        }));
      },
      async remove(name) { await cache.delete(key(name)); },
    };
  }

  /**
   * Nothing persistent works (Firefox and Safari private windows, storage
   * blocked for the site): keep the chunks in memory for this visit only, so
   * the game still runs, and say so.
   */
  function memoryBackend() {
    const m = new Map();
    return {
      kind: 'memory',
      async list() { return [...m.keys()]; },
      async size(name) { return m.has(name) ? m.get(name).size : -1; },
      async blob(name) { if (!m.has(name)) throw new Error(`${name} is missing`); return m.get(name); },
      async write(name, bytes) { m.set(name, new Blob([bytes])); },
      async remove(name) { m.delete(name); },
    };
  }

  // A tiny write and read back: getDirectory() and caches.open() succeed in
  // some places where writing then fails (private windows, a full disk).
  async function writable(be) {
    const name = 'probe-' + Math.random().toString(36).slice(2);
    try {
      await be.write(name, new Uint8Array([1, 2, 3]));
      const ok = (await be.size(name)) === 3;
      await be.remove(name);
      return ok ? null : 'a test write read back wrong';
    } catch (e) {
      try { await be.remove(name); } catch (e2) { /* gone */ }
      return (e && (e.name + ': ' + e.message)) || String(e);
    }
  }

  // ---------------------------------------------------------------- store

  /** Chunk list of a manifest file: [{h, size, index, file}]. */
  function chunksOf(f, chunk) {
    return f.chunks.map((h, i) => ({ h, size: Math.max(0, Math.min(chunk, f.size - i * chunk)), index: i, file: f }));
  }

  class Store {
    constructor(be, other, chunk) {
      this.be = be;
      this.kind = be.kind;
      this.other = other; // the backend not in use, adopted from once
      this.chunk = chunk;
    }

    /**
     * OPFS where writable, else the Cache API, else memory (this visit only).
     * preferCache: test the fallback. probe: check with a test write (done
     * when nothing is stored yet, that is before a first download).
     * store.why lists why a better backend was not used.
     */
    static async open({ preferCache = false, chunk = 32 * 1024 * 1024, probe = 'auto', memoryOk = true, writer = 'auto' } = {}) {
      const why = [];
      let opfs = null;
      if (!preferCache) {
        try { opfs = await opfsBackend(why, writer); } catch (e) { why.push('OPFS: ' + (e && e.message || e)); }
      }
      let cache = null;
      try { cache = await cacheBackend(); } catch (e) { why.push('Cache API: ' + (e && (e.name + ': ' + e.message) || e)); }
      if (!cache && !global.caches) why.push('Cache API: not offered');
      for (const be of [opfs, cache]) {
        if (!be) continue;
        if (probe === true || (probe === 'auto' && !(await be.list()).some((n) => n.startsWith('c-')))) {
          const err = await writable(be);
          if (err) { why.push(`${be.kind === 'opfs' ? 'OPFS' : 'Cache API'}: ${err}`); continue; }
        }
        const s = new Store(be, be === opfs ? cache : null, chunk);
        s.why = why;
        return s;
      }
      if (!memoryOk) throw new Error('This browser offers no storage for the game data (private window?).');
      const s = new Store(memoryBackend(), null, chunk);
      s.why = why;
      return s;
    }

    chunks(f) { return chunksOf(f, this.chunk); }

    async has(c) { return (await this.be.size('c-' + c.h)) === c.size; }

    /** Chunks of these files not stored yet (existence and size only: fast). */
    async missing(files) {
      const list = [];
      const seen = new Set();
      for (const f of files) {
        for (const c of this.chunks(f)) {
          if (seen.has(c.h)) continue;
          seen.add(c.h);
          list.push(c);
        }
      }
      // All at once: a warm start checks 15-50 chunks.
      const here = await Promise.all(list.map((c) => this.has(c)));
      return list.filter((c, i) => !here[i]);
    }

    /** Bytes of this chunk already downloaded in pieces (resumable). */
    async resumable(c) {
      let n = 0;
      for (let k = 0; k * PIECE < c.size; k++) {
        const want = Math.min(PIECE, c.size - k * PIECE);
        if (want !== PIECE || (await this.be.size(`p-${c.h}-${k}`)) !== PIECE) break;
        n += PIECE;
      }
      return n;
    }

    async dropPieces(c) {
      for (let k = 0; k * PIECE < c.size; k++) await this.be.remove(`p-${c.h}-${k}`);
    }

    /**
     * Download one chunk: resume from its stored pieces with a Range request,
     * keep a piece every 4 MiB, check the whole chunk's hash, then commit it.
     * onBytes(c, n): n bytes of this chunk are in hand (it can go down when
     * bytes had to be dropped).
     */
    async fetchChunk(c, url, signal, onBytes, fetchImpl = global.fetch, scratch = null) {
      // scratch: a buffer to reuse (one per download worker), so a long
      // download does not leave a 32 MiB buffer per chunk for the GC.
      const buf = scratch && scratch.byteLength >= c.size ? scratch.subarray(0, c.size) : new Uint8Array(c.size);
      let have = await this.resumable(c);
      for (let k = 0; k * PIECE < have; k++) {
        buf.set(new Uint8Array(await (await this.be.blob(`p-${c.h}-${k}`)).arrayBuffer()), k * PIECE);
      }
      const report = () => onBytes(c, have);
      if (have) report();
      let mismatches = 0;
      let lastErr = null;
      for (let attempt = 0; attempt < 6; attempt++) {
        if (signal.aborted) throw new DOMException('Download paused', 'AbortError');
        try {
          if (have < c.size) {
            const opts = { cache: 'no-store', signal };
            if (have) opts.headers = { Range: `bytes=${have}-` };
            const res = await fetchImpl(url, opts);
            if (res.status === 404 || res.status === 410) {
              const e = new Error('The game data on the server has changed since this page loaded. Reload the page.');
              e.fatal = true;
              throw e;
            }
            if (!res.ok || !res.body) throw new Error(`HTTP ${res.status}`);
            if (have && res.status !== 206) { // the server sent the whole chunk
              have = 0;
              report();
            }
            const reader = res.body.getReader();
            let flushed = Math.floor(have / PIECE);
            // Pieces cost a second write of the bytes: keep them only on a
            // connection slow enough that losing the chunk would hurt (a
            // chunk takes over 1.5 s), not on a fast one.
            let lastFlush = Date.now();
            for (;;) {
              const { done, value } = await reader.read();
              if (done) break;
              if (have + value.byteLength > c.size) throw new Error('the server sent more than expected');
              buf.set(value, have);
              have += value.byteLength;
              report();
              if (Date.now() - lastFlush < this.pieceAfterMs) continue;
              while ((flushed + 1) * PIECE <= have && (flushed + 1) * PIECE < c.size) {
                await this.be.write(`p-${c.h}-${flushed}`, buf.subarray(flushed * PIECE, (flushed + 1) * PIECE));
                flushed++;
                lastFlush = Date.now();
              }
            }
            if (have !== c.size) throw new Error(`arrived incomplete (${have} of ${c.size} bytes)`);
          }
          if ((await sha256(buf)) !== c.h) {
            // Damaged in transit or on the server: never store it.
            await this.dropPieces(c);
            have = 0;
            report();
            if (++mismatches >= 2) {
              const e = new Error(`${c.file.name} arrived damaged twice (its checksum does not match). Try again later.`);
              e.fatal = true;
              throw e;
            }
            continue;
          }
          await this.be.write('c-' + c.h, buf);
          await this.dropPieces(c);
          return;
        } catch (e) {
          if (signal.aborted) throw new DOMException('Download paused', 'AbortError');
          if (isAbort(e)) throw e;
          lastErr = e;
          await sleep(Math.min(8000, 1000 * 2 ** attempt));
        }
      }
      throw new Error(`Download of ${c.file.name} failed: ${lastErr && lastErr.message || lastErr}`);
    }

    /**
     * Download chunks, `concurrency` at a time. urlFor(c) gives the URL.
     * Throws AbortError when signal aborts; what finished stays stored.
     */
    async download(list, { urlFor, signal, onBytes, concurrency = 2, fetchImpl }) {
      const queue = list.slice();
      let failure = null;
      const size = list.reduce((n, c) => Math.max(n, c.size), 0);
      const worker = async () => {
        const scratch = new Uint8Array(size);
        while (queue.length && !failure) {
          const c = queue.shift();
          try {
            await this.fetchChunk(c, urlFor(c), signal, onBytes, fetchImpl, scratch);
          } catch (e) {
            failure = failure || e;
          }
        }
      };
      await Promise.all(Array.from({ length: Math.max(1, concurrency) }, worker));
      if (failure) throw failure;
    }

    /** One file's bytes, assembled from its chunks without extra copies. */
    async read(f) {
      const parts = [];
      for (const c of this.chunks(f)) {
        let b;
        try {
          b = await this.be.blob('c-' + c.h);
        } catch (e) {
          // Not readable right now is not proof of damage: keep it.
          throw new Error(`${f.name} could not be read from the browser's storage (${e && e.name}: ${e && e.message}). Reload to try again.`);
        }
        if (b.size !== c.size) {
          await this.be.remove('c-' + c.h); // the wrong size can never be right
          throw new Error(`${f.name} is incomplete in the browser's storage. Reload to download the missing part again.`);
        }
        parts.push(b);
      }
      const blob = parts.length === 1 ? parts[0] : new Blob(parts);
      return new Uint8Array(await blob.arrayBuffer());
    }

    /** Hash every stored chunk of these files; drop the damaged ones. */
    async verify(files, onProgress) {
      const list = [];
      const seen = new Set();
      for (const f of files) for (const c of this.chunks(f)) if (!seen.has(c.h)) { seen.add(c.h); list.push(c); }
      let bad = 0;
      let missing = 0;
      let done = 0;
      for (const c of list) {
        if (!(await this.has(c))) missing++;
        else if ((await sha256(await (await this.be.blob('c-' + c.h)).arrayBuffer())) !== c.h) {
          await this.be.remove('c-' + c.h);
          bad++;
        }
        done++;
        if (onProgress) onProgress(done, list.length);
      }
      return { checked: list.length, bad, missing };
    }

    // -------------------------------------------------- manifests and refs

    async saveManifest(id, text) {
      if ((await this.be.size(`m-${id}.json`)) < 0) await this.be.write(`m-${id}.json`, new TextEncoder().encode(text));
    }

    async loadManifest(id) {
      if ((await this.be.size(`m-${id}.json`)) < 0) return null;
      try { return JSON.parse(await (await this.be.blob(`m-${id}.json`)).text()); } catch (e) { return null; }
    }

    /**
     * Which manifests a page path started with. One record per path and set
     * of manifests: a tab still running an older release (whose files are
     * read in place) keeps its chunks while its record is younger than
     * REF_SUPERSEDED, even after another tab started a newer one.
     */
    async ref(page, ids) {
      const key = page.replace(/[^A-Za-z0-9]/g, '_') + '-' + ids.join('_').replace(/[^A-Za-z0-9_]/g, '').slice(0, 64);
      await this.be.write('r-' + key, new TextEncoder().encode(JSON.stringify({ page, ids, t: Date.now() })));
    }

    // ------------------------------------------------------------- adopting

    /**
     * Adopt downloads the older layout made, and chunks the other backend
     * holds, for these files' missing chunks: each is read, checked against
     * its hash and written as a chunk; the old copy goes once its file is
     * whole. Nothing is downloaded. Returns the bytes adopted.
     */
    async adopt(files, onProgress) {
      const names = new Set(await this.be.list());
      const legacyOPFS = this.kind === 'opfs' && [...names].some((n) => LEGACY.test(n));
      const oldCache = this.kind === 'cache' ? this.be.cache : this.other && this.other.cache;
      let cacheKeys = [];
      if (oldCache) {
        try { cacheKeys = (await oldCache.keys()).map((r) => r.url); } catch (e) { cacheKeys = []; }
      }
      if (!legacyOPFS && !cacheKeys.length) return 0;
      let adopted = 0;
      const total = files.reduce((n, f) => n + f.size, 0);
      const take = async (c, bytes) => {
        if (!bytes || bytes.byteLength !== c.size || (await sha256(bytes)) !== c.h) return false;
        await this.be.write('c-' + c.h, bytes);
        adopted += c.size;
        if (onProgress) onProgress(adopted, total);
        return true;
      };
      for (const f of files) {
        const want = [];
        for (const c of this.chunks(f)) if (!(await this.has(c))) want.push(c);
        // 1. A whole file in OPFS from the older page.
        const ln = legacyName(f);
        if (want.length && this.kind === 'opfs' && names.has(ln)) {
          try {
            const file = await this.be.blob(ln);
            if (file.size === f.size) {
              for (const c of want.slice()) {
                const off = c.index * this.chunk;
                if (await take(c, new Uint8Array(await file.slice(off, off + c.size).arrayBuffer()))) want.splice(want.indexOf(c), 1);
              }
            }
          } catch (e) {
            if (e && e.name === 'QuotaExceededError') throw e;
          }
        }
        if (this.kind === 'opfs' && names.has(ln) && !want.length) await this.be.remove(ln);
        // 2. Cache API entries: older parts (by path, rev and part number,
        // whatever the host was) and chunks stored under the new names.
        if (oldCache) {
          for (const c of want.slice()) {
            const hits = cacheKeys.filter((u) => {
              if (u === KEY_BASE + 'c-' + c.h) return true;
              try {
                const url = new URL(u);
                return url.pathname.endsWith('/' + f.path) && url.searchParams.get('rev') === f.rev && url.searchParams.get('part') === String(c.index);
              } catch (e) { return false; }
            });
            for (const u of hits) {
              const r = await oldCache.match(u);
              const ok = r && await take(c, new Uint8Array(await r.arrayBuffer()));
              if (ok || this.kind !== 'cache' || !u.startsWith(KEY_BASE)) await oldCache.delete(u);
              if (ok) { want.splice(want.indexOf(c), 1); break; }
            }
          }
        }
      }
      return adopted;
    }

    // -------------------------------------------------------------- pruning

    /**
     * Delete what no live page path needs. Call only after a start that had
     * every chunk it needed. Every path that started within REF_MAX_AGE keeps
     * all chunks of its manifests; a manifest this
     * store cannot read stops the prune altogether.
     */
    async prune(page, ids) {
      await this.ref(page, ids);
      const names = await this.be.list();
      const live = new Set();
      const keepChunks = new Set();
      const keepRevs = new Set();
      const now = Date.now();
      const refs = [];
      for (const n of names) {
        if (!n.startsWith('r-')) continue;
        let r;
        try { r = JSON.parse(await (await this.be.blob(n)).text()); } catch (e) { return { removed: 0, kept: 'unreadable record ' + n }; }
        if (r) refs.push([n, r]);
      }
      // The newest record of each path lives REF_MAX_AGE; older ones of the
      // same path (an older release in another tab) REF_SUPERSEDED.
      const newest = new Map();
      for (const [, r] of refs) newest.set(r.page, Math.max(newest.get(r.page) || 0, r.t));
      for (const [n, r] of refs) {
        const limit = r.t === newest.get(r.page) ? REF_MAX_AGE : REF_SUPERSEDED;
        if (now - r.t > limit) { await this.be.remove(n); continue; }
        for (const id of r.ids) live.add(id);
      }
      for (const id of live) {
        const m = await this.loadManifest(id);
        if (!m) return { removed: 0, kept: 'unreadable manifest ' + id };
        for (const f of m.files) {
          keepRevs.add(f.rev);
          for (const h of f.chunks) keepChunks.add(h);
        }
      }
      let removed = 0;
      for (const n of names) {
        let drop = false;
        if (n.startsWith('c-')) drop = !keepChunks.has(n.slice(2));
        else if (n.startsWith('p-')) drop = !keepChunks.has(n.slice(2, 66));
        else if (n.startsWith('m-')) drop = !live.has(n.slice(2, -5));
        else if (LEGACY.test(n)) drop = !keepRevs.has(LEGACY.exec(n)[1]);
        if (drop) { await this.be.remove(n); removed++; }
      }
      // Older Cache API parts whose file is no longer in any live manifest.
      const oldCache = this.kind === 'cache' ? this.be.cache : this.other && this.other.cache;
      if (oldCache) {
        for (const req of await oldCache.keys()) {
          if (req.url.startsWith(KEY_BASE)) continue;
          let rev = null;
          try { rev = new URL(req.url).searchParams.get('rev'); } catch (e) { /* not ours */ }
          if (!rev || !keepRevs.has(rev)) { await oldCache.delete(req); removed++; }
        }
      }
      return { removed };
    }

    /** Is anything of ours stored here (this layout or an older one)? */
    async anything() {
      if ((await this.be.list()).some((n) => n.startsWith('c-') || n.startsWith('p-') || LEGACY.test(n))) return true;
      const oldCache = this.kind === 'cache' ? this.be.cache : this.other && this.other.cache;
      try { return !!(oldCache && (await oldCache.keys()).length); } catch (e) { return false; }
    }

    /** Bytes stored for these files (whole chunks only). */
    async storedBytes(files) {
      let n = 0;
      for (const f of files) for (const c of this.chunks(f)) if (await this.has(c)) n += c.size;
      return n;
    }
  }

  // A chunk that takes longer than this to arrive is kept in pieces as it
  // downloads (tests set 0).
  Store.prototype.pieceAfterMs = 1500;

  global.XTStore = { Store, NAME, PIECE, sha256, legacyName };
})(typeof window !== 'undefined' ? window : globalThis);
