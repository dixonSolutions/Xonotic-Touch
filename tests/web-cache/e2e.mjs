// End-to-end check of the web loader in a real headless Chrome (CDP).
//
//   node tests/web-cache/e2e.mjs --chrome PATH --url URL --profile DIR [--label NAME]
//        [--no-click] [--timeout SEC] [--throttle-bps N] [--latency MS] [--stop-after-bytes N]
//        [--map NAME] [--map2 NAME] [--expect-fetch] [--bots N] [--settle SEC] [--cpu N]
//        [--scan] [--wait-complete] [--map3 NAME] [--bg-frames SEC]
//
// --map: once the menu is up, start a bot match on NAME ("map NAME" from
// the console, bot_number N, default 4) and wait until it is playable (the
// client QuakeC loaded, then 3 s of frames). --map2: then change to a second
// map the same way (a map not downloaded yet: maps on demand fetches its
// pack first; --expect-fetch fails the run when it needed none). Each
// step's time, data bytes and peak heap go in the JSON. A map change on
// demand, from a fresh profile:
//   tests/web-cache/e2e.sh /tmp/p 'http://localhost:8080/?autoplay&allmaps=0' first \
//       --map stormkeep --map2 dance --expect-fetch
//
// Opens URL in a Chrome profile kept in DIR (so a second run is a reload with
// the same storage), clicks Download if the page asks, waits until the game's
// menu is up (the menu QuakeC has started and frames are drawn) and prints
// one JSON line: times since navigation (ms), every request under data/, and
// the page's own timing marks. Options go on the command line, not in the
// environment (the node wrapper on the dev machine drops environment).
//
// The split set (progressive download): --scan (with ?debug in the URL)
// keeps every engine console line that looks like a missing file or an
// error; --wait-complete waits, after the maps, until the background
// download has stored and mounted everything; --map3 then plays a map whose
// packs were mounted while the game ran. Each map step also samples the
// frames for 3 s: frames a second, the longest gap, gaps over 50 ms.
//
// It drives Chrome with the GPU (ANGLE on EGL); without it the engine runs
// on SwiftShader and every evaluation times out.
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';

const args = process.argv.slice(2);
const opt = (name, d) => { const i = args.indexOf('--' + name); return i < 0 ? d : args[i + 1]; };
const flag = (name) => args.includes('--' + name);
const chrome = opt('chrome');
const url = opt('url');
const profile = opt('profile');
const label = opt('label', 'run');
const profileDir = profile;
const timeout = Number(opt('timeout', '600')) * 1000;
const throttle = Number(opt('throttle-bps', '0'));
const latency = Number(opt('latency', '0'));
const stopAfter = Number(opt('stop-after-bytes', '0'));
const map1 = opt('map', '');
const map2 = opt('map2', '');
const bots = Number(opt('bots', '4'));
if ((!chrome && !opt('port')) || !url || (!profile && !opt('port'))) { console.error('usage: e2e.mjs --chrome PATH --url URL --profile DIR'); process.exit(2); }

const freePort = () => new Promise((r) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => r(p)); }); });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// --port: attach to a Chrome started by tests/web-cache/e2e.sh (on the dev
// machine node runs in a container whose fontconfig crashes Chrome).
// Otherwise start one here.
let port = Number(opt('port', '0'));
let proc = null;
let chromeErr = '';
if (!port) {
  port = await freePort();
  fs.mkdirSync(profile, { recursive: true });
  proc = spawn(chrome, [
    '--headless=new', '--use-angle=gl-egl', '--enable-gpu', '--ignore-gpu-blocklist',
    '--disable-breakpad', '--disable-crash-reporter', '--no-first-run', '--no-default-browser-check',
    '--autoplay-policy=no-user-gesture-required', '--window-size=1280,720',
    `--user-data-dir=${profile}`, `--remote-debugging-port=${port}`, 'about:blank',
  ], { stdio: ['ignore', 'ignore', 'pipe'] });
  proc.stderr.on('data', (d) => { chromeErr += d; if (chromeErr.length > 20000) chromeErr = chromeErr.slice(-10000); });
}

let ws;
for (let i = 0; i < 100; i++) {
  try {
    const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    const page = list.find((t) => t.type === 'page');
    if (page) { ws = new WebSocket(page.webSocketDebuggerUrl); break; }
  } catch (e) { /* not up yet */ }
  await sleep(100);
}
await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
let id = 0;
const scan = flag('scan');
const SUSPECT = /not found|couldn't|could not|can't|cannot|missing|failed|unknown command|error|unable/i;
const problems = [];
const pending = new Map();
const requests = [];
const byId = new Map();
let dataBytes = 0;
ws.onmessage = (ev) => {
  const m = JSON.parse(ev.data);
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); return; }
  if (m.method === 'Network.requestWillBeSent') {
    const u = new URL(m.params.request.url);
    if (u.pathname.includes('/data/')) {
      const r = { path: u.pathname.replace(/^.*\/data\//, 'data/'), range: m.params.request.headers.Range || m.params.request.headers.range || '' };
      requests.push(r);
      byId.set(m.params.requestId, r);
    }
  } else if (m.method === 'Network.responseReceived') {
    const r = byId.get(m.params.requestId);
    if (r) { r.status = m.params.response.status; r.cached = !!(m.params.response.fromDiskCache || m.params.response.fromMemoryCache); }
  } else if (m.method === 'Network.dataReceived') {
    if (byId.has(m.params.requestId)) dataBytes += m.params.dataLength;
  } else if (m.method === 'Runtime.consoleAPICalled' && scan) {
    const text = m.params.args.map((a) => a.value === undefined ? a.description || '' : String(a.value)).join(' ').replace(/\x1b\[[0-9;]*m|\^[0-9]|\^x[0-9a-f]{3}/gi, '');
    if (SUSPECT.test(text) && !problems.includes(text) && problems.length < 80) problems.push(text);
  }
};
// A page busy with a long task (the engine loading a map) answers late:
// give up on that poll, not on the run.
const send = (method, params = {}, ms = 20000) => new Promise((resolve) => {
  const i = ++id;
  const t = setTimeout(() => { pending.delete(i); resolve({ timeout: true }); }, ms);
  pending.set(i, (m) => { clearTimeout(t); resolve(m); });
  ws.send(JSON.stringify({ id: i, method, params }));
});
ws.onclose = () => { console.log(JSON.stringify({ label, error: 'devtools connection closed', chrome: chromeErr.slice(-2000) })); process.exit(1); };
const evaluate = async (expr) => {
  const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true, userGesture: true });
  if (r.result && r.result.exceptionDetails) return { error: r.result.exceptionDetails.text };
  return r.result && r.result.result ? r.result.result.value : undefined;
};
// Heap: V8's own heap plus ArrayBuffer backing stores (the game data copied
// into memory, the engine's wasm memory), sampled at every poll.
// Also the page's renderer process (the largest renderer under this
// Chrome; Linux /proc): RssAnon is its private memory, which includes the
// engine's wasm memory, and is what gets a tab killed on a phone.
let peak = { js: 0, backing: 0, total: 0, renderer: 0 };
const procStatus = (pid) => { try { return fs.readFileSync(`/proc/${pid}/status`, 'utf8'); } catch (e) { return ''; } };
const procField = (st, k) => { const m = new RegExp('^' + k + ':\\s+(\\d+)', 'm').exec(st); return m ? Number(m[1]) : 0; };
const rendererMB = () => {
  if (!fs.existsSync('/proc/self/status')) return null;
  const pids = fs.readdirSync('/proc').filter((x) => /^\d+$/.test(x));
  const cmd = (pid) => { try { return fs.readFileSync(`/proc/${pid}/cmdline`, 'utf8'); } catch (e) { return ''; } };
  const browser = pids.find((pid) => { const c = cmd(pid); return c.includes(`--remote-debugging-port=${port}`) && !c.includes('--type='); });
  if (!browser) return null;
  const under = (pid) => { for (let i = 0; i < 6 && pid && pid !== '1'; i++) { if (pid === browser) return true; pid = String(procField(procStatus(pid), 'PPid')); } return false; };
  let best = 0;
  for (const pid of pids) {
    if (!cmd(pid).includes('--type=renderer') || !under(pid)) continue;
    best = Math.max(best, procField(procStatus(pid), 'RssAnon'));
  }
  return Math.round(best / 1024);
};
const heap = async () => {
  const r = await send('Runtime.getHeapUsage', {}, 5000);
  if (!r.result) return null;
  const h = { js: Math.round(r.result.usedSize / 1e6), jsTotal: Math.round(r.result.totalSize / 1e6), backing: Math.round((r.result.backingStorageSize || 0) / 1e6), renderer: rendererMB() };
  h.total = h.js + h.backing;
  if (h.total > peak.total) Object.assign(peak, { js: h.js, backing: h.backing, total: h.total });
  if (h.renderer > peak.renderer) peak.renderer = h.renderer;
  return h;
};
await send('Network.enable');
await send('Runtime.enable');
await send('Page.enable');
await send('Emulation.setFocusEmulationEnabled', { enabled: true });
// --cpu N: N times slower CPU (a phone stand-in).
if (Number(opt('cpu', '0')) > 1) await send('Emulation.setCPUThrottlingRate', { rate: Number(opt('cpu')) });
if (throttle || latency) {
  await send('Network.emulateNetworkConditions', { offline: false, latency, downloadThroughput: throttle || -1, uploadThroughput: -1 });
}

const t0 = Date.now();
await send('Page.navigate', { url });
const out = { label, url, clicked: false, requests, events: {} };
const note = (k) => { if (!(k in out.events)) out.events[k] = Date.now() - t0; };
let done = false;
let stopped = false;
while (!done && Date.now() - t0 < timeout) {
  await sleep(100);
  const s = await evaluate(`(() => {
    const vis = (id) => { const e = document.getElementById(id); return !!e && !e.hidden; };
    const log = window.xt && xt.log ? xt.log() : [];
    return {
      download: vis('xt-download'), progress: vis('xt-progress'), ready: vis('xt-ready'), failed: vis('xt-error'),
      errorText: (document.getElementById('xt-error-text') || {}).textContent || '',
      status: (document.getElementById('xt-status-text') || {}).textContent || '',
      gone: document.getElementById('xt-overlay').classList.contains('xt-gone'),
      timing: window.xt && xt.state && xt.state.timing,
      menu: log.some((l) => /menu/i.test(l) && /loaded|program/i.test(l)),
      lines: log.length, tail: log.slice(-3),
    };
  })()`);
  if (!s || s.error) continue;
  if (s.failed) { out.error = s.errorText; break; }
  if (s.download) {
    note('asked');
    if (!out.clicked && !flag('no-click')) { out.clicked = true; await evaluate(`document.getElementById('xt-dl-go').click()`); }
    if (flag('no-click')) break;
  }
  if (s.progress) note('downloading');
  if (stopAfter && dataBytes >= stopAfter && !stopped) {
    stopped = true;
    out.stoppedAt = dataBytes;
    break; // Chrome is killed below, mid-download: the next run is a reload
  }
  if (s.ready) note('ready');
  if (s.gone) note('playing');
  if (s.menu) { note('menu'); out.tail = s.tail; out.timing = s.timing; done = true; console.error(`${(Date.now() - t0) / 1000}s: menu`); }
  await heap();
}
if (!done && !out.error && !stopped) out.error = 'timeout';
// Let the engine run a few seconds after the menu: a load is done when
// frames keep coming, not at the first one.
if (done) {
  const f0 = await evaluate('(() => new Promise((r) => { let n = 0; const t = performance.now(); const step = () => { n++; if (performance.now() - t < 2000) requestAnimationFrame(step); else r(n); }; requestAnimationFrame(step); }))()');
  out.fps2s = f0 / 2;
  out.memory = await evaluate('performance.memory ? Math.round(performance.memory.usedJSHeapSize / 1e6) : null');
  out.pruned = await evaluate('window.xt && xt.state && xt.state.pruned');
  out.heapMenu = await heap();
  out.peakMenu = Object.assign({}, peak);
  out.mode = await evaluate('window.xt && xt.state && { lazy: xt.state.lazy, profile: xt.state.profile, mounted: xt.mounts ? xt.mounts.size : null }');
  out.bytesMenu = dataBytes;
}

// Start a match on `name` and wait until it is playable.
const playMap = async (name, key) => {
  console.error(`${(Date.now() - t0) / 1000}s: ${key} ${name}`);
  const before = dataBytes;
  // Lines so far (pages before state.logLines: the log's length, which
  // stops growing at 200).
  const lines0 = await evaluate('xt.state.logLines || xt.log().length');
  const t = Date.now();
  await evaluate(`xt.command('bot_number ${bots}; map ${name}')`);
  let ok = false;
  let fetched = false;
  while (Date.now() - t < timeout) {
    await sleep(200);
    const s = await evaluate(`(() => { const l = xt.log(); const fresh = Math.min(l.length, (xt.state.logLines || l.length) - ${lines0}); return { loaded: l.slice(l.length - fresh).some((x) => /client: program loaded|CSQC.*loaded/i.test(x)),
      failed: !document.getElementById('xt-error').hidden, err: document.getElementById('xt-error-text').textContent,
      fetching: !!document.getElementById('xt-map') && !document.getElementById('xt-map').hidden, tail: l.slice(-4) }; })()`);
    await heap();
    if (!s || s.error) continue;
    if (s.fetching && !fetched) { fetched = true; out.events[key + '-fetch'] = Date.now() - t0; }
    if (s.failed) { out.error = s.err; break; }
    if (s.loaded) { ok = true; out[key + 'Tail'] = s.tail; break; }
  }
  if (!ok) { out.error = out.error || key + ' timeout'; return; }
  console.error(`${(Date.now() - t0) / 1000}s: ${key} loaded`);
  out.events[key] = Date.now() - t0;
  out[key + 'Ms'] = Date.now() - t;
  const f = await frames(3000);
  out[key + 'Fps'] = f.n / 3;
  out[key + 'Frames'] = f;
  out[key + 'Data'] = await dataState();
  out[key + 'Bytes'] = dataBytes - before;
  out[key + 'Heap'] = await heap();
  out[key + 'Lazy'] = await evaluate('window.xt && xt.lazyStats ? xt.lazyStats() : null');
};
// Frames over `ms`: how many, the longest gap between two, gaps over 50 ms.
const frames = async (ms) => (await evaluate(`(() => new Promise((r) => { let n = 0, max = 0, over = 0, last = performance.now(); const t = last; const step = () => { const now = performance.now(); const d = now - last; last = now; n++; if (d > max) max = d; if (d > 50) over++; if (now - t < ${ms}) requestAnimationFrame(step); else r({ n, maxGapMs: Math.round(max), over50: over }); }; requestAnimationFrame(step); }))()`)) || {};
// Where the background download stands.
const dataState = () => evaluate('window.xt && xt.bg ? { state: xt.bg.state, have: xt.prog.have.size, of: xt.prog.counted.length, mounted: xt.mounts.size, sent: xt.bg.sent } : null');
if (done) { out.bytesAtMenu = dataBytes; out.dataAtMenu = await dataState(); }
if (done && map1) await playMap(map1, 'map1');
// --bg-frames SEC: in the match, the frames over SEC seconds with the
// background download running, then with it paused ("web_maps pause").
const bgFrames = Number(opt('bg-frames', '0'));
if (done && bgFrames && !out.error) {
  await sleep(6000); // the load settles
  out.bgRunning = Object.assign(await frames(bgFrames * 1000), { data: await dataState() });
  await evaluate(`xt.command('web_maps pause')`);
  await sleep(1500);
  out.bgPaused = Object.assign(await frames(bgFrames * 1000), { data: await dataState() });
  await evaluate(`xt.command('web_maps resume')`);
}
if (done && map2 && !out.error) await playMap(map2, 'map2');
// --expect-fetch: the second map must have been downloaded on demand.
if (done && map2 && flag('expect-fetch') && !out.error && !(out.map2Bytes > 0)) out.error = `${map2} was not fetched on demand`;
if (done && flag('wait-complete') && !out.error) {
  const t = Date.now();
  let st = null;
  while (Date.now() - t < timeout) {
    st = await dataState();
    if (st && (st.state === 'complete' || st.state === 'error')) break;
    await sleep(500);
  }
  out.events.complete = Date.now() - t0;
  out.dataAtEnd = st;
  out.framesAfter = await frames(3000);
  // The music's track list, run again when its pack landed.
  await evaluate(`xt.command('echo XTREMAP $g_cdtracks_remaplist')`);
  await sleep(500);
  out.remap = await evaluate(`(xt.log().filter((l) => l.includes('XTREMAP')).pop() || '')`);
  if (!st || st.state !== 'complete') out.error = 'the background download did not complete: ' + JSON.stringify(st);
  else if (opt('map3')) await playMap(opt('map3'), 'map3');
}
if (scan) out.problems = problems;
out.peak = peak;
// --settle SEC: keep playing, then sample again (the page gives read-ahead
// memory back about 15 s after a load).
const settle = Number(opt('settle', '0'));
if (done && settle && !out.error) {
  await sleep(settle * 1000);
  out.settled = await heap();
  // What is left once the garbage is collected (a phone short of memory
  // collects it; a desktop may never bother).
  await send('HeapProfiler.collectGarbage', {}, 20000);
  out.settledGC = await heap();
  out.settledLazy = await evaluate('window.xt && xt.lazyStats ? (({xhr, ms, cacheBytes}) => ({xhr, ms, cacheBytes}))(xt.lazyStats()) : null');
}
out.dataBytes = dataBytes;
out.blobRequests = requests.filter((r) => /blobs\/|\.part\d\d|\.pk3$|\.cfg$|\.txt$/.test(r.path)).length;
if (proc) proc.kill('SIGKILL');
await sleep(300);
console.log(JSON.stringify(out));
process.exit(out.error && !stopped ? 1 : 0);
