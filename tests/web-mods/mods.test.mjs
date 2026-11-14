// Unit tests for web/mods.js: the page's half of "Pick file" in the Mods tab
// and drop-a-zip, against a small in-memory stand-in for Emscripten's FS.
import { createRequire } from 'node:module';
import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';
const require = createRequire(import.meta.url);
const root = new URL('../../', import.meta.url);
const M = require(new URL('web/mods.js', root).pathname);

class FakeFS {
  constructor() { this.files = new Map(); this.dirs = new Set(['/']); }
  mkdir(p) { if (this.dirs.has(p)) throw new Error('EEXIST'); this.dirs.add(p); }
  stat(p) { if (!this.files.has(p) && !this.dirs.has(p)) throw new Error('ENOENT'); return {}; }
  writeFile(p, d) {
    if (!this.dirs.has(p.replace(/\/[^/]+$/, '') || '/')) throw new Error('ENOENT ' + p);
    this.files.set(p, typeof d === 'string' ? Buffer.from(d) : Buffer.from(d));
  }
  readdir(p) { return ['.', '..', ...[...this.files.keys()].filter((f) => f.replace(/\/[^/]+$/, '') === p).map((f) => f.slice(p.length + 1))]; }
  unlink(p) { if (!this.files.delete(p)) throw new Error('ENOENT'); }
  text(p) { return this.files.get(p)?.toString(); }
}

let passed = 0;
function test(name, fn) { fn(); passed++; console.log('ok - ' + name); }
function setup() {
  const FS = new FakeFS();
  const cmds = [];
  let syncs = 0;
  const mods = M.make({ FS, command: (t) => cmds.push(t), now: () => 1234, sync: () => syncs++ });
  return { FS, cmds, mods, syncs: () => syncs };
}

test('the status file exists once the page is ready, so the menu shows Pick file', () => {
  const { FS, mods } = setup();
  mods.publish(' ');
  assert.equal(FS.text(M.STATUS), ' \n');
});

test('paths are the ones the menu and engine use', () => {
  // QuakeC fopen("touch/...") lands in <userdir>/data/data/touch on every platform
  assert.equal(M.STATUS, '/xt-user/data/data/touch/mod-import-status.txt');
  assert.equal(M.REQUEST, '/xt-user/data/data/touch/mod-import-request.txt');
  assert.equal(M.INCOMING, '/xt-user/luastate/incoming');
  const qh = readFileSync(new URL('engine/data/xonotic-data.pk3dir/qcsrc/menu/xonotic/dialog_touch_mods.qh', root), 'utf8');
  assert.match(qh, /TOUCH_MODS_IMPORT_REQUEST_FILE = "touch\/mod-import-request\.txt"/);
  assert.match(qh, /TOUCH_MODS_IMPORT_STATUS_FILE = "touch\/mod-import-status\.txt"/);
  const shell = readFileSync(new URL('web/shell.js', root), 'utf8');
  assert.match(shell, /const USERDIR = '\/xt-user';/, 'shell.js mounts the userdir where mods.js looks');
});

test('a request is consumed once', () => {
  const { FS, mods } = setup();
  assert.equal(mods.takeRequest(), false);
  mods.publish(' ');
  FS.writeFile(M.REQUEST, '1\n');
  assert.equal(mods.takeRequest(), true);
  assert.equal(mods.takeRequest(), false);
});

test('a picked zip is copied unchanged to incoming/ and the menu is told where', () => {
  const { FS, mods, syncs } = setup();
  const bytes = new Uint8Array([0x50, 0x4b, 3, 4, 9, 9]);
  const path = mods.picked('My Mod (2).zip', bytes);
  assert.equal(path, '/xt-user/luastate/incoming/My_Mod__2_.zip');
  assert.deepEqual([...FS.files.get(path)], [...bytes]);
  assert.equal(FS.text(M.STATUS), `picked 1234 ${path}\n`);
  assert.equal(syncs(), 1, 'saved to IndexedDB');
});

test('only the latest pick waits in incoming/', () => {
  const { FS, mods } = setup();
  mods.picked('a.zip', new Uint8Array([1]));
  mods.picked('b.zip', new Uint8Array([2]));
  assert.deepEqual(FS.readdir(M.INCOMING).filter((f) => f[0] !== '.'), ['b.zip']);
});

test('names cannot leave incoming/', () => {
  assert.equal(M.stem('../../etc/passwd.zip'), 'passwd');
  assert.equal(M.stem('..zip'), 'imported_mod');
  assert.equal(M.stem('C:\\Users\\x\\evil;rm -rf.zip'), 'evil_rm_-rf');
  assert.equal(M.stem(''), 'imported_mod');
  assert.ok(M.stem('x'.repeat(300)).length <= 48);
});

test('an empty or oversized file is refused before anything is written', () => {
  const { FS, mods } = setup();
  assert.equal(mods.picked('e.zip', new Uint8Array(0)), null);
  assert.equal(FS.text(M.STATUS), 'The file is empty\n');
  assert.equal(mods.picked('big.zip', new Uint8Array(M.MAX + 1)), null);
  assert.match(FS.text(M.STATUS), /over 8 MB/);
  assert.equal([...FS.files.keys()].filter((f) => f.startsWith(M.INCOMING)).length, 0);
});

test('a dropped zip opens the Mods tab and goes to the engine review, quoted', () => {
  const { cmds, mods } = setup();
  const path = mods.dropped('hello.zip', new Uint8Array([1, 2]));
  assert.equal(cmds.length, 1);
  assert.match(cmds[0], /menu_cmd directmenu touchmods/);
  assert.ok(cmds[0].endsWith(`lua_mod_install "${path}"`));
  assert.ok(!/lua_install_confirm|lua_mod_enable|grant/.test(cmds[0]), 'nothing is confirmed or granted for the player');
});

console.log(`web mods: ${passed} tests passed`);
