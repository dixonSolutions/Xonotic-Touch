import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const source = fs.readFileSync(new URL('../../web/shell.js', import.meta.url), 'utf8');
const exportPoint = "  main().catch((e) => fail(e && e.message || String(e)));";
assert.ok(source.includes(exportPoint), 'shell startup export point exists');

const storage = new Map();
class MemoryCache {
  async match(request) {
    const item = storage.get(String(request));
    return item && new Response(item.body.slice(), { headers: item.headers });
  }
  async put(request, response) {
    storage.set(String(request), {
      body: new Uint8Array(await response.arrayBuffer()),
      headers: Object.fromEntries(response.headers.entries()),
    });
  }
  async delete(request) { return storage.delete(String(request)); }
  async keys() { return [...storage.keys()].map((url) => new Request(url)); }
}
const cache = new MemoryCache();
const caches = {
  async open(name) { assert.equal(name, 'xt-data-v1'); return cache; },
  async delete() { storage.clear(); return true; },
};

class Element extends EventTarget {
  constructor() {
    super();
    this.hidden = false;
    this.disabled = false;
    this.checked = false;
    this.value = '';
    this.textContent = '';
    this.style = {};
    this.classList = { add() {}, remove() {} };
    this.dataset = {};
  }
  setAttribute() {}
  focus() {}
  closest() { return new Element(); }
  appendChild() {}
}
class Canvas extends Element {}
Canvas.prototype.getContext = () => null;
const elements = new Map();
const element = (id) => {
  if (!elements.has(id)) elements.set(id, id === 'canvas' ? new Canvas() : new Element());
  return elements.get(id);
};

function loadShell(fetchImpl) {
  const context = {
    URL, URLSearchParams, Request, Response, ReadableStream, TransformStream,
    AbortController, DOMException, TextEncoder, TextDecoder, Uint8Array,
    Map, Set, Math, Number, String, Object, Array, Promise, Error,
    caches,
    fetch: fetchImpl,
    performance: { now: (() => { let t = 0; return () => ++t; })() },
    setTimeout, clearTimeout, setInterval, clearInterval,
    matchMedia: () => ({ matches: false, addEventListener() {} }),
    localStorage: { getItem: () => null, setItem() {} },
    navigator: { maxTouchPoints: 0, language: 'en', languages: ['en'], sendBeacon() {} },
    location: { href: 'https://game.example/web/', search: '?store=cache', reload() {} },
    screen: {},
    HTMLCanvasElement: Canvas,
    document: {
      getElementById: element,
      createElement: (name) => name === 'canvas' ? new Canvas() : new Element(),
      addEventListener() {},
      documentElement: {},
      body: new Element(),
      activeElement: null,
      fullscreenElement: null,
    },
    window: {
      addEventListener() {},
      AudioContext: null,
      XT_CONFIG: {},
    },
    console,
    confirm: () => true,
  };
  context.window.window = context.window;
  const testSource = source.replace(exportPoint,
    '  globalThis.__xtTest = { openCacheStore, openStore, missingFiles, download, cancelDownload, state };');
  vm.runInNewContext(testSource, context, { filename: 'web/shell.js' });
  return context.__xtTest;
}

const PART = 32 * 1024 * 1024;
const bytes0 = new Uint8Array(PART);
bytes0.fill(0x41);
const bytes1 = new Uint8Array([0x42]);
const file = {
  name: 'large.pk3', path: 'data/large.pk3', rev: '0123456789abcdef', size: PART + 1,
  group: 'core', base: 'https://game.example/data/',
  parts: [
    { url: 'large.part00', size: PART },
    { url: 'large.part01', size: 1 },
  ],
};

let firstRequests = 0;
let secondRequests = 0;
let holdSecond = false;
let secondResolve;
let secondSignal;
const fetchPart = async (request, options = {}) => {
  const url = String(request);
  if (url.endsWith('large.part00')) {
    firstRequests++;
    return new Response(bytes0.slice());
  }
  secondRequests++;
  if (!holdSecond) return new Response(bytes1.slice());
  secondSignal = options.signal;
  return await new Promise((resolve, reject) => {
    secondResolve = resolve;
    options.signal.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')), { once: true });
  });
};

const firstPage = loadShell(fetchPart);
const cacheStore = await firstPage.openStore();
assert.equal(cacheStore.kind, 'cache');
assert.equal((await firstPage.missingFiles(cacheStore, [file]))[0], file);

// Simulate a cancel after part 0 committed. The partial file remains missing,
// but its complete first part is retained for the next attempt.
holdSecond = true;
const stopped = firstPage.download(cacheStore, [file]);
while (!secondSignal) await new Promise((resolve) => setTimeout(resolve, 0));
firstPage.cancelDownload();
await assert.rejects(stopped, { name: 'AbortError' });
assert.equal(await cacheStore.hasPart(file, 0), true);
assert.equal(await cacheStore.hasPart(file, 1), false);

// Resume without fetching the already cached part, then simulate a page reload.
holdSecond = false;
await firstPage.download(cacheStore, [file]);
assert.equal(firstRequests, 1, 'resume skips the cached source chunk');
assert.equal(secondRequests, 2, 'resume refetches only the missing source chunk');
const reloadedPage = loadShell(fetchPart);
const reloadedStore = await reloadedPage.openStore();
assert.equal((await reloadedPage.missingFiles(reloadedStore, [file])).length, 0, 'completed game data survives page reload');
assert.equal(firstRequests, 1, 'warm cache hit performs no network request');
const restored = await reloadedStore.get(file);
assert.equal(restored.length, PART + 1);
assert.equal(restored[0], 0x41);
assert.equal(restored[PART], 0x42);

// A truncated entry can retain its old whole-file marker. Detect it on read,
// remove the bad piece, and make the next setup offer a repair download.
const lastKey = new URL(`${file.path}?rev=${file.rev}&part=1`, file.base).href;
storage.set(lastKey, { body: new Uint8Array(0), headers: { 'x-xt-size': String(file.size) } });
await assert.rejects(reloadedStore.get(file), /incomplete part/);
assert.equal(await reloadedStore.hasPart(file, 1), false);
assert.equal((await reloadedPage.missingFiles(reloadedStore, [file]))[0], file);
console.log('web cache: cancellation resumes missing chunks, reload is a cache hit, truncated entries self-repair');
