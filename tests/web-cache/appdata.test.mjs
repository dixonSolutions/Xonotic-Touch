// web/appdata.js: which picked files make an install's game data.
// node tests/web-cache/appdata.test.mjs
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const ctx = { navigator: {}, document: {} };
ctx.globalThis = ctx;
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(new URL('../../web/appdata.js', import.meta.url), 'utf8'), ctx);
const { check } = ctx.XTAppData;

const file = (name, size = 10) => ({ name, size });
const packKind = (n, whole) => {
  const m = /^xonotic-[^-]+-(data|maps|music|xoncompat)(-[0-9a-z_.]+)?\.pk3$/i.exec(n);
  return m ? (whole && m[2] ? '' : m[1].toLowerCase()) : '';
};
const tools = (over = {}) => Object.assign({ packKind, zipDirectory: async () => new Uint8Array(4), hasBytes: () => false, gpu: { astc: false } }, over);
let n = 0;
const test = async (name, fn) => { await fn(); n++; console.log('ok -', name); };

await test('data and maps are needed; the newest of each kind wins; config.cfg is kept apart', async () => {
  const r = await check([file('xonotic-20260101-data.pk3'), file('xonotic-20261107-data.pk3'), file('xonotic-20261107-maps.pk3'),
    file('config.cfg'), file('notes.txt'), file('xonotic-20261107-maps-dance.pk3')], tools());
  assert.deepEqual(Array.from(r.packs, (p) => p.name).sort(), ['xonotic-20261107-data.pk3', 'xonotic-20261107-maps.pk3']);
  assert.equal(r.config.name, 'config.cfg');
});
await test('a pick without the maps pack says what is missing', async () => {
  await assert.rejects(check([file('xonotic-20261107-data.pk3')], tools()), /needs xonotic-…-data\.pk3 and xonotic-…-maps\.pk3/);
});
await test('a pack still downloading (no zip directory) is refused', async () => {
  await assert.rejects(check([file('xonotic-20261107-data.pk3'), file('xonotic-20261107-maps.pk3')], tools({ zipDirectory: async () => null })), /not complete/);
});
await test('the phone set needs ASTC', async () => {
  const phone = tools({ hasBytes: () => true });
  await assert.rejects(check([file('xonotic-20261107-data.pk3'), file('xonotic-20261107-maps.pk3')], phone), /ASTC/);
  phone.gpu = { astc: true };
  assert.equal((await check([file('xonotic-20261107-data.pk3'), file('xonotic-20261107-maps.pk3')], phone)).packs.length, 2);
});
console.log(`appdata: ${n} tests passed`);
