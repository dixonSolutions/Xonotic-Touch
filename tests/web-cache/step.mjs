// One step in a browser that tests/web-cache/browser.sh started: Chromium
// over CDP or Firefox over WebDriver BiDi. Prints the step's result as one
// JSON line; exits 1 when --expect does not hold.
//
//   node step.mjs --engine chromium|firefox --port P [--url URL] [--init FILE]
//        [--until EXPR] [--eval EXPR] [--expect PREDICATE] [--label NAME]
//        [--grant durableStorage] [--timeout SEC] [--no-navigate]
//
// --init FILE     a script run in the page before any of its own (mocks)
// --until EXPR    polled in the page until truthy (a string result starting
//                 with "FAIL" ends the step at once)
// --eval EXPR     evaluated in the page (awaited); its value is the result
// --expect PRED   JavaScript over `r` (the result), in node: must be truthy
// --grant NAME    Chromium: grant a permission to the page's origin first
import fs from 'node:fs';

const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf('--' + n); return i < 0 ? d : args[i + 1]; };
const flag = (n) => args.includes('--' + n);
const engine = opt('engine');
const port = Number(opt('port'));
const url = opt('url');
const label = opt('label', 'step');
const timeout = Number(opt('timeout', '120')) * 1000;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const init = opt('init') ? fs.readFileSync(opt('init'), 'utf8') : '';

async function chromium() {
  let ws = null;
  for (let k = 0; k < 150 && !ws; k++) {
    try {
      const v = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json();
      ws = new WebSocket(v.webSocketDebuggerUrl);
      await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
    } catch (e) { ws = null; await sleep(100); }
  }
  if (!ws) throw new Error('no Chromium on port ' + port);
  let id = 0;
  const pend = new Map();
  ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  const send = (method, params = {}, session) => new Promise((resolve) => {
    const i = ++id;
    pend.set(i, resolve);
    ws.send(JSON.stringify(Object.assign({ id: i, method, params }, session ? { sessionId: session } : {})));
  });
  const targets = (await send('Target.getTargets')).result.targetInfos.filter((t) => t.type === 'page');
  const target = targets[0] ? targets[0].targetId : (await send('Target.createTarget', { url: 'about:blank' })).result.targetId;
  const s = (await send('Target.attachToTarget', { targetId: target, flatten: true })).result.sessionId;
  await send('Page.enable', {}, s);
  await send('Emulation.setFocusEmulationEnabled', { enabled: true }, s);
  if (opt('grant') && url) await send('Browser.grantPermissions', { origin: new URL(url).origin, permissions: [opt('grant')] });
  if (init) await send('Page.addScriptToEvaluateOnNewDocument', { source: init }, s);
  return {
    navigate: (u) => send('Page.navigate', { url: u }, s),
    async eval(expr) {
      const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true, userGesture: true }, s);
      if (r.result && r.result.exceptionDetails) return 'FAIL ' + (r.result.exceptionDetails.exception && r.result.exceptionDetails.exception.description || r.result.exceptionDetails.text);
      return r.result && r.result.result ? r.result.result.value : undefined;
    },
    close() { ws.close(); },
  };
}

async function firefox() {
  let ws = null;
  for (let k = 0; k < 200 && !ws; k++) {
    try {
      const w = new WebSocket(`ws://127.0.0.1:${port}/session`);
      await new Promise((r, j) => { w.onopen = r; w.onerror = j; });
      ws = w;
    } catch (e) { await sleep(200); }
  }
  if (!ws) throw new Error('no Firefox on port ' + port);
  let id = 0;
  const pend = new Map();
  ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  const send = (method, params = {}) => new Promise((resolve) => {
    const i = ++id;
    pend.set(i, resolve);
    ws.send(JSON.stringify({ id: i, method, params }));
  });
  let s;
  for (let k = 0; k < 20; k++) { s = await send('session.new', { capabilities: {} }); if (!s.error) break; await sleep(1000); }
  if (s.error) throw new Error(JSON.stringify(s));
  const ctx = (await send('browsingContext.getTree', {})).result.contexts[0].context;
  if (init) await send('script.addPreloadScript', { functionDeclaration: `() => { ${init} }` });
  return {
    navigate: (u) => send('browsingContext.navigate', { context: ctx, url: u, wait: 'none' }),
    async eval(expr) {
      // JSON in, JSON out: BiDi's own serialization is deep and typed.
      const r = await send('script.evaluate', {
        expression: `(async () => JSON.stringify(await (${expr})))().catch((e) => JSON.stringify('FAIL ' + (e && e.message || e)))`,
        target: { context: ctx }, awaitPromise: true, resultOwnership: 'none', userActivation: true,
      });
      const v = r.result && r.result.result;
      if (!v || v.type !== 'string') return v && v.type === 'undefined' ? undefined : 'FAIL ' + JSON.stringify(r).slice(0, 300);
      return JSON.parse(v.value);
    },
    async close() { await send('session.end', {}); ws.close(); },
  };
}

const page = await (engine === 'firefox' ? firefox() : chromium());
let result;
let ok = true;
try {
  if (url && !flag('no-navigate')) await page.navigate(url);
  const t0 = Date.now();
  if (opt('until')) {
    for (;;) {
      let v;
      try { v = await page.eval(`(() => { try { return ${opt('until')}; } catch (e) { return false; } })()`); } catch (e) { v = false; }
      if (typeof v === 'string' && v.startsWith('FAIL')) { result = v; ok = false; break; }
      if (v) break;
      if (Date.now() - t0 > timeout) { result = 'FAIL timed out waiting for: ' + opt('until'); ok = false; break; }
      await sleep(250);
    }
  }
  if (ok && opt('eval')) {
    // The page may still be loading when there is no --until.
    for (let k = 0; ; k++) {
      result = await page.eval(opt('eval'));
      if (!(typeof result === 'string' && /FAIL.*(is not defined|undefined)/.test(result)) || k > 40) break;
      await sleep(250);
    }
    if (typeof result === 'string' && result.startsWith('FAIL')) ok = false;
  }
  if (ok && opt('expect')) ok = !!(new Function('r', 'return ' + opt('expect')))(result);
} catch (e) {
  result = 'FAIL ' + (e && e.message || e);
  ok = false;
}
console.log(JSON.stringify({ label, ok, result }));
try { await page.close(); } catch (e) { /* gone */ }
process.exit(ok ? 0 : 1);
