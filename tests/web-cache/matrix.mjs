// One visit of the web page in a browser that tests/web-cache/matrix.sh
// started, over its remote protocol: Chromium (CDP) or Firefox (WebDriver
// BiDi). Prints one JSON line.
//
//   node matrix.mjs --engine chromium|firefox --port P --url URL --server S
//        [--click] [--new-tab] [--two-tabs] [--kill-at BYTES] [--kill-at-start]
//        [--cpu N] [--mobile] [--quota BYTES] [--timeout SEC]
//
// --server: the matrix server (http://127.0.0.1:PORT), whose /__stats
// counts blob requests and bytes for this visit.
// --kill-at BYTES: exit 3 as soon as the server has sent that many blob
// bytes (matrix.sh then kills the browser: a process killed mid-download).
// --kill-at-start: exit 3 once the engine is starting (killed during start).
// The visit is done when the game has started (the shell is gone and the
// engine's menu loaded) or an error shows.
const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf('--' + n); return i < 0 ? d : args[i + 1]; };
const flag = (n) => args.includes('--' + n);
const engine = opt('engine');
const port = Number(opt('port'));
const url = opt('url');
const server = opt('server');
const timeout = Number(opt('timeout', '600')) * 1000;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const stats = async () => { try { return await (await fetch(server + '/__stats')).json(); } catch (e) { return null; } };

// ---------------------------------------------------------------- drivers
function chromium() {
  let ws, id = 0;
  const pend = new Map();
  const send = (method, params = {}, ms = 20000, session) => new Promise((resolve) => {
    const i = ++id;
    const t = setTimeout(() => { pend.delete(i); resolve({ timeout: true }); }, ms);
    pend.set(i, (m) => { clearTimeout(t); resolve(m); });
    ws.send(JSON.stringify(Object.assign({ id: i, method, params }, session ? { sessionId: session } : {})));
  });
  let session = null;
  return {
    async open() {
      for (let k = 0; k < 100 && !ws; k++) {
        try {
          const v = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json();
          ws = new WebSocket(v.webSocketDebuggerUrl);
        } catch (e) { await sleep(100); }
      }
      await new Promise((r) => { ws.onopen = r; });
      ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
    },
    async tab(newTab) {
      const targets = (await send('Target.getTargets')).result.targetInfos.filter((t) => t.type === 'page');
      let target = targets[0] && targets[0].targetId;
      if (newTab || !target) target = (await send('Target.createTarget', { url: 'about:blank' })).result.targetId;
      const s = (await send('Target.attachToTarget', { targetId: target, flatten: true })).result.sessionId;
      const page = {
        send: (m, p, ms) => send(m, p, ms, s),
        async eval(expr) {
          const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true, userGesture: true }, 20000, s);
          return r.result && r.result.result ? r.result.result.value : undefined;
        },
        navigate: (u) => send('Page.navigate', { url: u }, 20000, s),
      };
      await page.send('Page.enable');
      await page.send('Emulation.setFocusEmulationEnabled', { enabled: true });
      if (flag('mobile')) {
        await page.send('Emulation.setDeviceMetricsOverride', { width: 915, height: 412, deviceScaleFactor: 2.6, mobile: true });
        await page.send('Emulation.setTouchEmulationEnabled', { enabled: true, maxTouchPoints: 5 });
        await page.send('Emulation.setUserAgentOverride', { userAgent: 'Mozilla/5.0 (Linux; Android 16; SM-X306B) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Mobile Safari/537.36' });
      }
      if (opt('cpu')) await page.send('Emulation.setCPUThrottlingRate', { rate: Number(opt('cpu')) });
      if (opt('quota')) {
        const origin = new URL(url).origin;
        await page.send('Storage.overrideQuotaForOrigin', { origin, quotaSize: Number(opt('quota')) });
      }
      return page;
    },
  };
}

function firefox() {
  let ws, id = 0;
  const pend = new Map();
  const send = (method, params = {}, ms = 20000) => new Promise((resolve) => {
    const i = ++id;
    const t = setTimeout(() => { pend.delete(i); resolve({ timeout: true }); }, ms);
    pend.set(i, (m) => { clearTimeout(t); resolve(m); });
    ws.send(JSON.stringify({ id: i, method, params }));
  });
  const conv = (v) => !v ? v : v.type === 'object' ? Object.fromEntries(v.value.map(([k, x]) => [k, conv(x)])) : v.type === 'array' ? v.value.map(conv) : v.value;
  return {
    async open() {
      for (let k = 0; k < 150 && !ws; k++) {
        try {
          const w = new WebSocket(`ws://127.0.0.1:${port}/session`);
          await new Promise((r, j) => { w.onopen = r; w.onerror = j; });
          ws = w;
        } catch (e) { await sleep(200); }
      }
      ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
      const s = await send('session.new', { capabilities: {} });
      if (s.error) throw new Error(JSON.stringify(s));
    },
    async tab(newTab) {
      let ctx = (await send('browsingContext.getTree', {})).result.contexts[0].context;
      if (newTab) ctx = (await send('browsingContext.create', { type: 'tab' })).result.context;
      return {
        async eval(expr) {
          const r = await send('script.evaluate', { expression: expr, target: { context: ctx }, awaitPromise: true, resultOwnership: 'none', userActivation: true }, 20000);
          return r.result && r.result.result ? conv(r.result.result) : undefined;
        },
        navigate: (u) => send('browsingContext.navigate', { context: ctx, url: u, wait: 'none' }),
      };
    },
    // Firefox takes one BiDi session at a time: end it, or the next visit
    // cannot connect.
    close: () => send('session.end', {}, 3000),
  };
}

// ------------------------------------------------------------------ visit
const STATE = `JSON.stringify((() => {
  const vis = (i) => { const e = document.getElementById(i); return !!e && !e.hidden; };
  const ov = document.getElementById('xt-overlay');
  const st = window.xt && xt.state || {};
  const log = window.xt && xt.log ? xt.log() : [];
  return { download: vis('xt-download'), progress: vis('xt-progress'), error: vis('xt-error'),
    errorText: (document.getElementById('xt-error-text') || {}).textContent || '',
    reason: (document.getElementById('xt-dl-reason') || {}).textContent || '',
    gone: !!ov && ov.classList.contains('xt-gone'),
    menu: log.some((l) => /menu/i.test(l) && /loaded|program/i.test(l)),
    store: st.store, profile: st.profile, lazy: st.lazy, problem: st.problem || '', timing: st.timing || {},
    crumb: st.crumb && st.crumb.phase };
})())`;

async function visit(page, out) {
  const t0 = Date.now();
  await page.navigate(url);
  let clicked = false;
  while (Date.now() - t0 < timeout) {
    await sleep(250);
    const raw = await page.eval(STATE);
    if (!raw) continue;
    const s = JSON.parse(raw);
    out.shown = out.shown || [];
    for (const k of ['download', 'progress', 'error']) if (s[k] && !out.shown.includes(k)) out.shown.push(k);
    Object.assign(out, { store: s.store, profile: s.profile, lazy: s.lazy, reason: s.reason || out.reason, problem: s.problem || out.problem });
    if (s.error) { out.error = s.errorText.slice(0, 300); return; }
    if (s.download && !clicked && flag('click')) { clicked = true; await page.eval(`document.getElementById('xt-dl-go').click()`); }
    if (s.download && !flag('click')) { out.asked = true; return; }
    const st = await stats();
    if (opt('kill-at') && st && st.blobBytes >= Number(opt('kill-at'))) { out.killedAt = st.blobBytes; return 'kill'; }
    if (flag('kill-at-start') && s.timing['engine-start'] && !s.menu) { out.killedAt = 'start'; return 'kill'; }
    if (s.gone && s.menu) { out.started = Date.now() - t0; out.timing = s.timing; return; }
  }
  out.error = 'timeout';
}

const drv = engine === 'firefox' ? firefox() : chromium();
await drv.open();
await fetch(server + '/__ctl?reset=1');
const out = { engine, url };
const page = await drv.tab(flag('new-tab'));
let r;
if (flag('two-tabs')) {
  const second = await drv.tab(true);
  const o2 = {};
  const [a, b] = await Promise.all([visit(page, out), (async () => { await sleep(1500); return visit(second, o2); })()]);
  r = a || b;
  out.second = o2;
} else {
  r = await visit(page, out);
}
const st = await stats();
if (st) Object.assign(out, { blobRequests: st.blobRequests, blobBytes: st.blobBytes });
console.log(JSON.stringify(out));
if (drv.close) await drv.close();
process.exit(r === 'kill' ? 3 : out.error ? 1 : 0);
