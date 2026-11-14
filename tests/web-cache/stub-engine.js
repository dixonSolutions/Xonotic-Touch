// A stand-in for the game engine (godnotic.js) in tests/web-cache/split.sh:
// what web/shell.js needs of the Emscripten module, and of the engine just
// the parts the data loader talks to. It runs web/shell.js, web/store.js
// and web/lazyfs.js unchanged in a real browser, with a made-up data set a
// real engine could not start on.
//
//   FS             a small in-memory file system with the calls shell.js
//                  and lazyfs.js make (a lazy file's read() is lazyfs.js's:
//                  the synchronous reads in place are the real ones)
//   callMain       "starts": lists /game/data/*.pk3 in name order, as the
//                  engine does (a later name wins), and reads each pack's
//                  directory
//   xt_web_command fs_mountpack NAME (adds the pack at the END of the search
//                  path, lowest priority: touch_fs_mount.h; the file is
//                  looked up through a listing of the data directory read
//                  again at most once a second, like fs.c's FS_DirMayHave,
//                  so a file that just appeared is "not found"), fs_rescan,
//                  map / changelevel (asks Module.xt.needMap first, as
//                  sys_wasm.c does; then loads maps/<map>.bsp and every file
//                  it lists), touch_data_status (kept, with the time)
//
// window.__engine is what the test reads.
'use strict';

(() => {
  const M = window.Module;
  const nodes = new Map(); // path -> node
  const dirs = new Set(['/']);
  class ErrnoError extends Error { constructor(errno) { super('errno ' + errno); this.errno = errno; } }
  const FS = {
    ErrnoError,
    mkdir(path) { if (dirs.has(path)) throw new ErrnoError(20); dirs.add(path); },
    mount() {},
    syncfs(populate, cb) { cb(null); },
    createFile(dir, name) {
      const path = dir + '/' + name;
      if (nodes.has(path)) throw new ErrnoError(20);
      const node = { name, stream_ops: {}, contents: new Uint8Array(0), usedBytes: 0 };
      nodes.set(path, node);
      return node;
    },
    writeFile(path, data) {
      const bytes = typeof data === 'string' ? new TextEncoder().encode(data) : data;
      nodes.set(path, { name: path.slice(path.lastIndexOf('/') + 1), stream_ops: {}, contents: bytes, usedBytes: bytes.length });
    },
    lookupPath(path) {
      if (!nodes.has(path)) throw new ErrnoError(44);
      return { node: nodes.get(path) };
    },
    unlink(path) { if (!nodes.delete(path)) throw new ErrnoError(44); },
  };

  const E = window.__engine = {
    started: false,
    startPacks: [], // the packs found at start, in name order
    startRequests: [], // data requests finished before the start
    path: [], // the search path, highest priority first: [{name, entries}]
    mountpacks: [], // fs_mountpack arguments, in order
    execs: [], // "exec FILE": [FILE, was it found]
    commands: [], // every other command (but the config words: set, alias, cd...)
    statuses: [], // [ms, "touch_data_status ..."]
    loaded: [], // maps loaded
    errors: [],
    reads: 0,
  };

  /** len bytes at pos of a file, through its read() where it has one (a lazy file). */
  function pread(path, pos, len) {
    const node = nodes.get(path);
    if (!node) throw new Error('no file ' + path);
    const size = node.usedBytes;
    const n = Math.max(0, Math.min(len, size - pos));
    const out = new Uint8Array(n);
    if (node.stream_ops.read) {
      E.reads++;
      const got = node.stream_ops.read({ node }, out, 0, n, pos);
      if (got !== n) throw new Error(`short read of ${path}: ${got} of ${n}`);
    } else out.set(node.contents.subarray(pos, pos + n));
    return out;
  }

  /** A pk3's entries: name -> {off, size} of its (stored) data. */
  function zipEntries(path) {
    const size = nodes.get(path).usedBytes;
    const tailAt = Math.max(0, size - 65557);
    const tail = pread(path, tailAt, size - tailAt);
    let e = -1;
    for (let i = tail.length - 22; i >= 0; i--) {
      if (tail[i] === 0x50 && tail[i + 1] === 0x4b && tail[i + 2] === 5 && tail[i + 3] === 6) { e = i; break; }
    }
    if (e < 0) throw new Error(path + ' is not a zip');
    const tv = new DataView(tail.buffer, tail.byteOffset);
    const count = tv.getUint16(e + 10, true);
    const dir = pread(path, tv.getUint32(e + 16, true), tv.getUint32(e + 12, true));
    const dv = new DataView(dir.buffer, dir.byteOffset);
    const entries = new Map();
    let p = 0;
    for (let k = 0; k < count; k++) {
      if (dv.getUint32(p, true) !== 0x02014b50) throw new Error(path + ': bad directory');
      const method = dv.getUint16(p + 10, true);
      const csize = dv.getUint32(p + 20, true);
      const nameLen = dv.getUint16(p + 28, true);
      const local = dv.getUint32(p + 42, true);
      const name = new TextDecoder().decode(dir.subarray(p + 46, p + 46 + nameLen));
      entries.set(name, { local, size: csize, method });
      p += 46 + nameLen + dv.getUint16(p + 30, true) + dv.getUint16(p + 32, true);
    }
    return entries;
  }
  function entryBytes(pack, name) {
    const path = '/game/data/' + pack;
    const en = E.path.find((x) => x.name === pack).entries.get(name);
    const head = pread(path, en.local, 30);
    const hv = new DataView(head.buffer);
    return pread(path, en.local + 30 + hv.getUint16(26, true) + hv.getUint16(28, true), en.size);
  }
  /** The pack a file comes from: the first in the search path that has it. */
  const packOf = (name) => { const s = E.path.find((x) => x.entries.has(name)); return s ? s.name : null; };
  // The engine's view of its data directory (fs.c, FS_DirMayHave): a
  // listing it reads again when it is over a second old. The game looks
  // files up all the time; here a timer does.
  const listing = { at: -1e9, names: new Set() };
  const dirHas = (name) => {
    const now = performance.now();
    if (now - listing.at > 1000) { listing.at = now; listing.names = new Set(dataPacks()); }
    return listing.names.has(name);
  };
  setInterval(() => dirHas(''), 300);
  const dataPacks = () => [...nodes.keys()].filter((p) => /^\/game\/data\/[^/]+\.pk3$/i.test(p)).map((p) => p.slice(11)).sort();

  function scan() {
    // Name order, each new pack in front: the last name has the highest priority.
    E.path = [];
    for (const name of dataPacks()) E.path.unshift({ name, entries: zipEntries('/game/data/' + name) });
  }
  function loadMap(map) {
    const bsp = `maps/${map}.bsp`;
    const pack = packOf(bsp);
    if (!pack) { E.errors.push(`map ${map}: ${bsp} not found`); return; }
    const bytes = entryBytes(pack, bsp);
    if (!bytes.length) { E.errors.push(`map ${map}: ${bsp} in ${pack} is empty`); return; }
    // The test's own .bsp lists the files the map loads; any other is taken as it is.
    const text = new TextDecoder('latin1').decode(bytes.subarray(0, 2000));
    if (text.startsWith('bsp ')) {
      for (const f of text.split('\n').slice(1)) {
        if (!/^[a-z0-9_\/.-]+\.[a-z0-9]+$/.test(f)) break;
        const p = packOf(f);
        if (!p) E.errors.push(`map ${map}: ${f} not found`);
        else entryBytes(p, f);
      }
    }
    E.loaded.push(map);
  }
  function run(line) {
    const m = /^(\S+)\s*(.*)$/.exec(line.trim());
    if (!m) return;
    const arg = m[2].replace(/^"(.*)"$/, '$1');
    if (m[1] === 'touch_data_status') E.statuses.push([Math.round(performance.now()), line]);
    else if (m[1] === 'fs_mountpack') {
      E.mountpacks.push(arg);
      const path = '/game/data/' + arg;
      if (!dirHas(arg) || !nodes.has(path)) E.errors.push(`fs_mountpack: could not find "${arg}"`);
      else if (E.path.some((x) => x.name === arg)) E.errors.push(`fs_mountpack: ${arg} is already mounted`);
      else {
        try { E.path.push({ name: arg, entries: zipEntries(path) }); } catch (e) { E.errors.push(`fs_mountpack: ${arg}: ${e.message}`); }
      }
    } else if (m[1] === 'fs_rescan') { E.commands.push(line); scan(); }
    else if (m[1] === 'map' || m[1] === 'changelevel') {
      E.commands.push(line);
      if (M.xt.needMap(arg, `${m[1]} "${arg}"`)) loadMap(arg);
    } else if (m[1] === 'exec') E.execs.push([arg, !!packOf(arg)]);
    else if (!/^(set|seta|unset|alias|unalias|cd)$/.test(m[1])) E.commands.push(line);
  }

  Object.assign(M, {
    FS,
    IDBFS: {},
    ccall(name, ret, types, args) {
      // Like the engine's command buffer: run at the next frame, in order.
      if (name === 'xt_web_command') for (const line of String(args[0]).split('\n')) setTimeout(() => { try { run(line); } catch (e) { E.errors.push(`${line}: ${e.message}`); } }, 0);
    },
    callMain() {
      E.startRequests = performance.getEntriesByType('resource').map((r) => r.name).filter((u) => u.includes('/data/blobs/'));
      scan();
      E.startPacks = dataPacks();
      E.started = true;
    },
    _xt_web_hook_maps: () => 2,
    _xt_web_refit() {},
    _xt_web_save() {},
    _xt_web_autosave: () => 0,
    _xt_web_fullscreen_changed() {},
    _xt_web_key() {},
  });

  // For the test: every file of every mounted pack, read through the search
  // path and hashed; {files, bad: [names whose sha256 is not the expected one]}.
  E.verify = async (expected) => {
    const bad = [];
    let files = 0;
    const hex = (b) => Array.from(new Uint8Array(b), (x) => x.toString(16).padStart(2, '0')).join('');
    for (const [name, want] of Object.entries(expected)) {
      const pack = packOf(name);
      if (!pack) { bad.push(name + ' (not found)'); continue; }
      files++;
      if (hex(await crypto.subtle.digest('SHA-256', entryBytes(pack, name))) !== want) bad.push(name + ' (wrong bytes from ' + pack + ')');
    }
    return { files, bad };
  };
  E.entries = (pack) => { const s = E.path.find((x) => x.name === pack); return s ? [...s.entries.keys()] : null; };
  E.packOf = packOf;

  setTimeout(() => M.onRuntimeInitialized(), 0);
})();
