// Godnotic in the browser: game data files the engine reads straight from the
// browser's storage, without copying them into memory first.
//
// The engine reads pk3s with plain synchronous read() calls into Emscripten's
// in-memory filesystem (MEMFS). Copying every pk3 into the JS heap at start-up
// cost about 1 GB of memory and half a second per start. Instead, each pk3
// becomes a MEMFS file whose read() fetches the bytes it needs from the
// stored chunks (store.js: OPFS files or Cache API blobs, which live on disk,
// not in memory):
//
//   - a synchronous XMLHttpRequest for a blob: URL of just the bytes wanted
//     (a slice of the stored chunk). Browsers still allow synchronous XHR on the main
//     thread; with the "x-user-defined" charset its text is the bytes. It
//     needs no worker, no SharedArrayBuffer (so no COOP/COEP headers, which
//     GitHub Pages cannot send), no JSPI and no Asyncify.
//   - 1 MiB blocks, the last 48 MiB of them kept (least recently used out
//     first): the engine's pk3 reader asks for 16 KiB at a time.
//   - reads of a whole block or more go straight into the engine's memory.
//
// probe() checks that the browser answers such a request correctly (the
// right bytes); where it does not, the page copies the files
// into memory as before.
'use strict';

(function (global) {
  const BLOCK = 1 << 20;
  const stats = { xhr: 0, bytes: 0, ms: 0, hits: 0, misses: 0, files: 0, byFile: {}, blocksSeen: 0 };
  const seen = new Set();
  let cacheLimit = 48 << 20;
  const cache = new Map(); // "file:block" -> Uint8Array, oldest first
  let cacheBytes = 0;
  let nextId = 0;
  const files = new Map(); // path -> {id, blobs, urls, size, chunk}
  let touched = new Set(); // "path\tblock" read since takeTouched()

  // Text decoding sniffs a byte order mark before it looks at the charset:
  // a response starting with FF FE, FE FF or EF BB BF comes back as UTF-16
  // or UTF-8, not as the bytes. So every read goes through a blob of its
  // own: PAD zero bytes, then exactly the bytes wanted (a slice refers to
  // the stored chunk, nothing is copied). A response always starts with
  // zeros, whatever the data, and no Range header is involved.
  const PAD = 3;
  const ZEROS = new Uint8Array(PAD);

  /** len bytes at off of blob, into dst at dstOff. Throws on any failure. */
  function rangeRead(blob, off, len, dst, dstOff) {
    const t = global.performance.now();
    const url = global.URL.createObjectURL(new global.Blob([ZEROS, blob.slice(off, off + len)]));
    try {
      const x = new global.XMLHttpRequest();
      x.open('GET', url, false);
      x.overrideMimeType('text/plain; charset=x-user-defined');
      x.send();
      if (x.status !== 200) throw new Error(`storage read failed (HTTP ${x.status})`);
      const s = x.responseText;
      if (s.length !== len + PAD) throw new Error(`storage read failed (${s.length - PAD} of ${len} bytes at ${off})`);
      for (let i = 0; i < len; i++) {
        const c = s.charCodeAt(PAD + i);
        if (c > 0x7f && (c < 0xf780 || c > 0xf7ff)) throw new Error(`storage read failed (not bytes at ${off + i})`);
        dst[dstOff + i] = c;
      }
    } finally {
      global.URL.revokeObjectURL(url);
    }
    stats.xhr++;
    stats.bytes += len;
    stats.ms += global.performance.now() - t;
  }

  function cachePut(key, bytes) {
    cache.set(key, bytes);
    cacheBytes += bytes.length;
    for (const [k, v] of cache) {
      if (cacheBytes <= cacheLimit) break;
      cache.delete(k);
      cacheBytes -= v.length;
    }
  }

  /**
   * A read-only MEMFS file at `path` whose bytes are the chunks in `blobs`
   * (Blob or File, in order, each `chunk` bytes but the last). Returns the node.
   */
  function create(FS, path, size, blobs, chunk) {
    if (chunk % BLOCK) throw new Error('chunk size must be a multiple of ' + BLOCK);
    const id = ++nextId;
    files.set(path, { id, blobs, size, chunk });
    const slash = path.lastIndexOf('/');
    const dir = path.slice(0, slash);
    let cur = '';
    for (const part of dir.split('/')) {
      if (!part) continue;
      cur += '/' + part;
      try { FS.mkdir(cur); } catch (e) { /* exists */ }
    }
    const node = FS.createFile(dir, path.slice(slash + 1), {}, true, false);
    node.contents = null;
    Object.defineProperty(node, 'usedBytes', { get: () => size, configurable: true });
    const blockOf = (k) => {
      const key = id + ':' + k;
      let b = cache.get(key);
      // Every block read is remembered, served from the cache or not: the
      // page reads the same ones ahead next time (takeTouched).
      touched.add(path + '\t' + k);
      if (b) {
        cache.delete(key); // most recently used goes last
        cache.set(key, b);
        stats.hits++;
        return b;
      }
      stats.misses++;
      if (!seen.has(key)) { seen.add(key); stats.blocksSeen++; }
      stats.byFile[path] = (stats.byFile[path] || 0) + 1;
      touched.add(path + '\t' + k);
      const pos = k * BLOCK;
      const c = Math.floor(pos / chunk);
      b = new Uint8Array(Math.min(BLOCK, size - pos));
      rangeRead(blobs[c], pos - c * chunk, b.length, b, 0);
      cachePut(key, b);
      return b;
    };
    const ops = Object.assign({}, node.stream_ops);
    // A read the browser's storage fails (the chunk was removed under the
    // page, a picked folder's file changed) is an I/O error to the engine,
    // not an exception that stops the game.
    const EIO = 29;
    const readAll = (stream, buffer, offset, length, position) => {
      try {
        return read(stream, buffer, offset, length, position);
      } catch (e) {
        stats.errors = (stats.errors || 0) + 1;
        stats.lastError = `${path}: ${e && e.message || e}`;
        if (global.console) global.console.warn('Godnotic: ' + stats.lastError);
        throw new FS.ErrnoError(EIO);
      }
    };
    const read = (stream, buffer, offset, length, position) => {
      if (position >= size) return 0;
      const n = Math.min(length, size - position);
      const dst = new Uint8Array(buffer.buffer, buffer.byteOffset + offset, n);
      let done = 0;
      while (done < n) {
        const pos = position + done;
        const k = Math.floor(pos / BLOCK);
        const inBlock = pos - k * BLOCK;
        const want = n - done;
        if (inBlock === 0 && want >= BLOCK && !cache.has(id + ':' + k)) {
          // A big read (a whole .bsp): straight into the engine's memory, up
          // to the end of this chunk, bypassing the cache.
          const c = Math.floor(pos / chunk);
          const len = Math.min(want, (c + 1) * chunk - pos);
          for (let j = k; j * BLOCK < pos + len; j++) touched.add(path + '\t' + j);
          rangeRead(blobs[c], pos - c * chunk, len, dst, done);
          stats.byFile[path + ' (direct MB)'] = (stats.byFile[path + ' (direct MB)'] || 0) + len / 1e6;
          done += len;
          continue;
        }
        const b = blockOf(k);
        const take = Math.min(want, b.length - inBlock);
        dst.set(b.subarray(inBlock, inBlock + take), done);
        done += take;
      }
      return n;
    };
    ops.read = readAll;
    ops.write = () => { throw new FS.ErrnoError(2); };
    ops.mmap = () => { throw new FS.ErrnoError(2); };
    node.stream_ops = ops;
    node.xtLazy = { id, size };
    stats.files++;
    return node;
  }

  /** Remove a file made by create() and let go of its blob URLs. */
  function remove(FS, path) {
    try {
      const lazy = FS.lookupPath(path).node.xtLazy;
      if (lazy) {
        for (const [k, v] of [...cache]) if (k.startsWith(lazy.id + ':')) { cache.delete(k); cacheBytes -= v.length; }
      }
    } catch (e) { /* gone */ }
    try { FS.unlink(path); } catch (e) { /* gone */ }
    files.delete(path);
  }

  /**
   * Read blocks into the cache ahead of time, asynchronously
   * (Blob.slice().arrayBuffer(), in runs of up to one chunk, four at a time):
   * far cheaper than the same bytes read synchronously. order: [[path,
   * block], ...], most wanted first; the first `max` bytes of it are read.
   * The cache grows to hold them and what the load reads besides (64 MiB);
   * trim() shrinks it again.
   */
  async function prefetch(order, max) {
    const t = global.performance.now();
    const want = [];
    const seenKeys = new Set();
    for (const [path, k] of order) {
      if (want.length * BLOCK >= max) break;
      const f = files.get(path);
      if (!f || k * BLOCK >= f.size) continue;
      const key = f.id + ':' + k;
      if (seenKeys.has(key)) continue;
      seenKeys.add(key);
      if (cache.has(key)) { const v = cache.get(key); cache.delete(key); cache.set(key, v); continue; }
      want.push([f, k]);
    }
    if (cacheLimit < want.length * BLOCK + cacheBytes + (64 << 20)) cacheLimit = want.length * BLOCK + cacheBytes + (64 << 20);
    want.sort((a, b) => a[0].id - b[0].id || a[1] - b[1]);
    const runs = [];
    for (let i = 0; i < want.length;) {
      const [f, k0] = want[i];
      const c = Math.floor(k0 * BLOCK / f.chunk);
      let j = i + 1;
      while (j < want.length && want[j][0] === f && want[j][1] === want[j - 1][1] + 1 &&
        Math.floor(want[j][1] * BLOCK / f.chunk) === c) j++;
      runs.push([f, c, k0, want[j - 1][1]]);
      i = j;
    }
    let n = 0;
    const one = async ([f, c, k0, k1]) => {
      const start = k0 * BLOCK;
      const end = Math.min(f.size, (k1 + 1) * BLOCK);
      const buf = new Uint8Array(await f.blobs[c].slice(start - c * f.chunk, end - c * f.chunk).arrayBuffer());
      for (let k = k0; k <= k1; k++) {
        const off = (k - k0) * BLOCK;
        // A copy, not a view: a view would keep the whole run alive after
        // trim() dropped the rest of it.
        cachePut(f.id + ':' + k, buf.slice(off, Math.min(off + BLOCK, buf.length)));
        n++;
      }
    };
    const queue = runs.slice();
    await Promise.all(Array.from({ length: 4 }, async () => { while (queue.length) await one(queue.shift()); }));
    stats.prefetched = (stats.prefetched || 0) + n;
    stats.prefetchMs = (stats.prefetchMs || 0) + (global.performance.now() - t);
    return n;
  }

  /**
   * Read the last `bytes` of a file (a pk3's directory, which the engine
   * reads when it mounts the pack) into the cache, asynchronously and
   * within the cache's limit. Returns the blocks read.
   */
  async function tail(path, bytes) {
    const f = files.get(path);
    if (!f || !f.size) return 0;
    const last = Math.floor((f.size - 1) / BLOCK);
    let n = 0;
    for (let k = Math.max(0, Math.floor((f.size - bytes) / BLOCK)); k <= last; k++) {
      const key = f.id + ':' + k;
      if (cache.has(key)) continue;
      const c = Math.floor(k * BLOCK / f.chunk);
      const start = k * BLOCK - c * f.chunk;
      const end = Math.min(start + BLOCK, f.size - c * f.chunk);
      const buf = new Uint8Array(await f.blobs[c].slice(start, end).arrayBuffer());
      if (files.get(path) !== f) return n; // removed meanwhile
      cachePut(key, buf);
      n++;
    }
    return n;
  }

  /** Shrink the cache to `limit` bytes (least recently used out first). */
  function trim(limit) {
    cacheLimit = limit;
    for (const [k, v] of cache) {
      if (cacheBytes <= cacheLimit) break;
      cache.delete(k);
      cacheBytes -= v.length;
    }
  }

  /** The blocks read since the last call: {path: [blocks]}. */
  function takeTouched() {
    const out = {};
    for (const t of touched) {
      const [p, k] = t.split('\t');
      (out[p] = out[p] || []).push(Number(k));
    }
    touched = new Set();
    return out;
  }

  /**
   * Does a synchronous ranged read of a blob: URL work here? Checks 16 bytes
   * from the middle of `blob` against blob.slice().
   */
  async function probe(blob) {
    if (!global.XMLHttpRequest || !global.URL || !global.URL.createObjectURL || !blob || blob.size < 64) return false;
    try {
      const off = Math.floor(blob.size / 2);
      const want = new Uint8Array(await blob.slice(off, off + 16).arrayBuffer());
      const got = new Uint8Array(16);
      rangeRead(blob, off, 16, got, 0);
      for (let i = 0; i < 16; i++) if (got[i] !== want[i]) return false;
      return true;
    } catch (e) {
      return false;
    }
  }

  global.XTLazyFS = {
    create, remove, probe, prefetch, tail, trim, takeTouched, stats, BLOCK,
    get cacheBytes() { return cacheBytes; },
    setCacheLimit(n) { cacheLimit = n; },
  };
})(typeof window !== 'undefined' ? window : globalThis);
