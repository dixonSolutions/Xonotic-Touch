// Unit tests for web/userdata.js: any change in the user folder asks for a
// save to IndexedDB, nothing else does.
import { createRequire } from 'node:module';
import assert from 'node:assert/strict';
const require = createRequire(import.meta.url);
const U = require(new URL('../../web/userdata.js', import.meta.url).pathname);

// files: path -> {size, mtime}; dirs: set of paths (the FS calls userdata.js uses)
class FakeFS {
  constructor() { this.files = new Map(); this.dirs = new Set(['/', '/xt-user']); this.clock = 1000; }
  mkdir(p) { this.dirs.add(p); }
  write(p, size) { this.files.set(p, { size, mtime: new Date(this.clock += 1000) }); }
  rm(p) { this.files.delete(p); }
  readdir(d) {
    if (!this.dirs.has(d)) throw new Error('ENOENT');
    const kids = new Set();
    for (const p of [...this.files.keys(), ...this.dirs]) {
      if (p !== d && p.startsWith(d + '/') && !p.slice(d.length + 1).includes('/')) kids.add(p.slice(d.length + 1));
    }
    return ['.', '..', ...kids];
  }
  lstat(p) {
    if (this.dirs.has(p)) return { mode: 0o40755, size: 4096, mtime: new Date(0) };
    const f = this.files.get(p);
    if (!f) throw new Error('ENOENT');
    return { mode: 0o100644, size: f.size, mtime: f.mtime };
  }
  isDir(mode) { return (mode & 0o170000) === 0o40000; }
}

let passed = 0;
const test = (name, fn) => { fn(); passed++; console.log('ok - ' + name); };

test('the first look only takes the starting point', () => {
  const FS = new FakeFS(); FS.write('/xt-user/data/config.cfg', 10);
  const w = U.watcher(FS);
  assert.equal(w.tick(), false);
  assert.equal(w.tick(), false, 'nothing changed');
});

test('a mod installed, turned on or saving its own files asks for a save, once', () => {
  const FS = new FakeFS();
  const w = U.watcher(FS); w.tick();
  FS.mkdir('/xt-user/mods'); FS.mkdir('/xt-user/mods/hello'); FS.write('/xt-user/mods/hello/init.lua', 50);
  assert.equal(w.tick(), true, 'installed');
  assert.equal(w.tick(), false, 'and only once');
  FS.write('/xt-user/mods/enabled.txt', 6);
  assert.equal(w.tick(), true, 'turned on');
  FS.write('/xt-user/mods/enabled.txt', 6);
  assert.equal(w.tick(), true, 'rewritten with the same size: the time says so');
  FS.mkdir('/xt-user/luastate'); FS.write('/xt-user/luastate/hello.dat', 3);
  assert.equal(w.tick(), true, 'a mod saved data');
});

test('a file removed asks for a save (uninstall)', () => {
  const FS = new FakeFS(); FS.mkdir('/xt-user/mods'); FS.write('/xt-user/mods/a.txt', 1);
  const w = U.watcher(FS); w.tick();
  FS.rm('/xt-user/mods/a.txt');
  assert.equal(w.tick(), true);
});

test('no user folder yet: no save, and no error', () => {
  const FS = new FakeFS(); FS.dirs.delete('/xt-user');
  const w = U.watcher(FS);
  assert.equal(w.tick(), false);
  assert.equal(U.signature(FS, '/xt-user'), null);
});

console.log(`web user data: ${passed} tests passed`);
