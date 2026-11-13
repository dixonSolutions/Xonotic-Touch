// End-to-end check of the web loader in a real headless Chrome (CDP).
//
//   node tests/web-cache/e2e.mjs --chrome PATH --url URL --profile DIR [--label NAME]
//        [--no-click] [--timeout SEC] [--throttle-bps N] [--latency MS] [--stop-after-bytes N]
//
// Opens URL in a Chrome profile kept in DIR (so a second run is a reload with
// the same storage), clicks Download if the page asks, waits until the game's
// menu is up (the menu QuakeC has started and frames are drawn) and prints
// one JSON line: times since navigation (ms), every request under data/, and
// the page's own timing marks. Options go on the command line, not in the
// environment (the node wrapper on the dev machine drops environment).
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
await send('Network.enable');
await send('Runtime.enable');
await send('Page.enable');
await send('Emulation.setFocusEmulationEnabled', { enabled: true });
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
  if (s.menu) { note('menu'); out.tail = s.tail; out.timing = s.timing; done = true; }
}
if (!done && !out.error && !stopped) out.error = 'timeout';
// Let the engine run a few seconds after the menu: a load is done when
// frames keep coming, not at the first one.
if (done) {
  const f0 = await evaluate('(() => new Promise((r) => { let n = 0; const t = performance.now(); const step = () => { n++; if (performance.now() - t < 2000) requestAnimationFrame(step); else r(n); }; requestAnimationFrame(step); }))()');
  out.fps2s = f0 / 2;
  out.memory = await evaluate('performance.memory ? Math.round(performance.memory.usedJSHeapSize / 1e6) : null');
  out.pruned = await evaluate('window.xt && xt.state && xt.state.pruned');
}
out.dataBytes = dataBytes;
out.blobRequests = requests.filter((r) => /blobs\/|\.part\d\d|\.pk3$|\.cfg$|\.txt$/.test(r.path)).length;
if (proc) proc.kill('SIGKILL');
await sleep(300);
console.log(JSON.stringify(out));
process.exit(0);
