// Lua mods in the real web build, in headless Chromium over CDP:
//   install from a link (fetch, CORS allowed), a refused link (no CORS),
//   Pick file in the Mods tab (the browser's file chooser), a dropped .zip,
//   and xt.http from a running mod.
// Run by browser.sh, which starts Chromium, the page and the two mod hosts.
//   node browser.mjs --port CDP --url PAGE --cors URL --nocors URL --zip FILE --httpzip FILE
import fs from 'node:fs';

const args = process.argv.slice(2);
const opt = (name, d) => { const i = args.indexOf('--' + name); return i < 0 ? d : args[i + 1]; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const port = Number(opt('port'));
const url = opt('url');
const cors = opt('cors');
const nocors = opt('nocors');
const zip = opt('zip');
const httpzip = opt('httpzip');
const verbose = args.includes('--verbose');

let ws;
for (let i = 0; i < 100 && !ws; i++) {
  try {
    const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    const page = list.find((t) => t.type === 'page');
    if (page) ws = new WebSocket(page.webSocketDebuggerUrl);
  } catch (e) { /* not up yet */ }
  if (!ws) await sleep(100);
}
await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
let id = 0;
const pending = new Map();
const listeners = [];
ws.onmessage = (ev) => {
  const m = JSON.parse(ev.data);
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); return; }
  for (const l of listeners) l(m);
};
const send = (method, params = {}, ms = 20000) => new Promise((resolve) => {
  const i = ++id;
  const t = setTimeout(() => { pending.delete(i); resolve({ timeout: true }); }, ms);
  pending.set(i, (m) => { clearTimeout(t); resolve(m); });
  ws.send(JSON.stringify({ id: i, method, params }));
});
const evaluate = async (expr, gesture = false) => {
  const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true, userGesture: gesture });
  if (r.result && r.result.exceptionDetails) return { error: r.result.exceptionDetails.text + ' ' + JSON.stringify(r.result.exceptionDetails.exception || '') };
  return r.result && r.result.result ? r.result.result.value : undefined;
};

const results = [];
let failed = 0;
const check = (name, ok, detail) => {
  results.push({ name, ok, detail });
  if (!ok) failed++;
  console.log(`${ok ? 'ok' : 'FAIL'} - ${name}${ok && !verbose ? '' : ': ' + JSON.stringify(detail).slice(0, 600)}`);
};

await send('Page.enable');
await send('Runtime.enable');
await send('Emulation.setFocusEmulationEnabled', { enabled: true });
await send('Page.navigate', { url });

// Up to the menu (first visit: the page copies the game data in first).
const t0 = Date.now();
let up = false, clicked = false;
while (Date.now() - t0 < 600000) {
  await sleep(250);
  const s = await evaluate(`(() => {
    const vis = (id) => { const e = document.getElementById(id); return !!e && !e.hidden; };
    const log = window.xt && xt.log ? xt.log() : [];
    return { download: vis('xt-download'), failed: vis('xt-error'),
      errorText: (document.getElementById('xt-error-text') || {}).textContent || '',
      menu: log.some((l) => /menu/i.test(l) && /loaded|program/i.test(l)) };
  })()`);
  if (!s || s.error) continue;
  if (s.failed) { check('page starts', false, s.errorText); break; }
  if (s.download && !clicked) { clicked = true; await evaluate(`document.getElementById('xt-dl-go').click()`, true); }
  if (s.menu) { up = true; break; }
}
if (!up) { check('page reaches the menu', false, 'timeout'); finish(); }
await sleep(3000);

// Runs console text and returns the engine's new lines once `until` matches
// one of them (or the time is up).
async function run(text, until, ms = 20000) {
  const before = await evaluate('xt.state.logLines || 0');
  await evaluate(`xt.command(${JSON.stringify(text)})`);
  const t = Date.now();
  let lines = [];
  while (Date.now() - t < ms) {
    await sleep(200);
    lines = await evaluate(`(() => { const l = xt.log(); const n = Math.min(l.length, (xt.state.logLines || 0) - ${before}); return l.slice(l.length - n); })()`) || [];
    if (!until || lines.some((l) => until.test(l))) break;
  }
  return lines;
}
// 0. First start: the welcome asks for a name before anything else. The
// bridge snapshot says what is open; the name goes in with real clicks and
// keys, where a player puts them (positions for this 1280x720 window).
const modal = async () => { const l = await run('uib_dump', null, 2500); const m = l.join('').match(/"modal":\[([^\]]*)\]/); return m ? m[1] : '?'; };
const click = async (x, y) => {
  await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x, y });
  await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
  await sleep(60);
  await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
  await sleep(400);
};
const typeText = async (text) => {
  for (const ch of text) {
    await send('Input.dispatchKeyEvent', { type: 'keyDown', key: ch, text: ch, unmodifiedText: ch });
    await send('Input.dispatchKeyEvent', { type: 'keyUp', key: ch });
    await sleep(40);
  }
};
check('a fresh browser opens on the welcome dialog', (await modal()) === '"welcome"', await modal());
const shot = async (name) => { const r = await send('Page.captureScreenshot', { format: 'png' }); if (r.result && opt('shots')) fs.writeFileSync(`${opt('shots')}/${name}.png`, Buffer.from(r.result.data, 'base64')); };
await shot('welcome-empty');
await click(828, 366);   // Continue, off while there is no name
check('Continue does nothing without a name', (await modal()) === '"welcome"', await modal());
await click(640, 304);   // the name field
await typeText('Web Player');
await sleep(500);
await shot('welcome-typed');
await click(828, 366);   // Continue
await sleep(1000);
let named = await run('_cl_name', /_cl_name/, 3000);
check('typing a name and Continue saves it', named.some((l) => /Web Player/.test(l)), named);
check('the welcome is gone after that', (await modal()) === '', await modal());
await run('touch_profile_done', /touch_profile_done/, 3000).then((l) => check('the welcome is marked done', l.some((x) => /"1"/.test(x)), l));

check('mods.js is loaded and answering the menu', await evaluate(`(() => { try { Module.FS.stat(XTMods.STATUS); return true; } catch (e) { return false; } })()`), 'the status file that shows Pick file');

// 1. A link on a host that allows web pages: downloaded with fetch(), reviewed.
let lines = await run(`lua_install_insecure_localhost 1; lua_mod_install ${cors}/hello.zip`, /review|Refused|failed|lua_install_confirm/i);
check('link install (CORS allowed) reaches the review', lines.some((l) => /lua_install_confirm|review/i.test(l)) && !lines.some((l) => /not available in this build|failed/i.test(l)), lines);
lines = await run('lua_install_confirm', /installed|Refused|failed/i);
check('the reviewed link install is installed', lines.some((l) => /installed/i.test(l)), lines);
lines = await run('lua_mods', /hello/i);
check('the installed mod is listed', lines.some((l) => /hello/.test(l)), lines);

// 2. A host that does not allow web pages: refused, and the message says why.
lines = await run(`lua_mod_install ${nocors}/hello.zip`, /CORS|failed|Refused/i);
check('link install without CORS says why', lines.some((l) => /CORS/.test(l) && /Pick file/.test(l)), lines);

// 3. Pick file: the menu's request opens the browser's file chooser.
await run('lua_mod_uninstall hello', /uninstall|removed|hello/i);
await send('Page.setInterceptFileChooserDialog', { enabled: true });
const chooser = new Promise((r) => listeners.push((m) => { if (m.method === 'Page.fileChooserOpened') r(m.params); }));
// a press in the game is the user activation the picker needs
await send('Input.dispatchMouseEvent', { type: 'mousePressed', x: 5, y: 5, button: 'left', clickCount: 1 });
await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: 5, y: 5, button: 'left', clickCount: 1 });
await evaluate(`(() => { const FS = Module.FS; const d = XTMods.REQUEST.replace(/\\/[^/]+$/, ''); FS.writeFile(XTMods.REQUEST, '1\\n'); return d; })()`);
const opened = await Promise.race([chooser, sleep(5000).then(() => null)]);
check('Pick file opens the file chooser', !!opened, opened ? 'Page.fileChooserOpened' : 'no Page.fileChooserOpened');
if (opened) {
  await send('DOM.enable');
  await send('DOM.setFileInputFiles', { files: [zip], backendNodeId: opened.backendNodeId });
  let st = '';
  for (let i = 0; i < 30 && !/^picked /.test(st); i++) {
    await sleep(200);
    st = await evaluate(`new TextDecoder().decode(Module.FS.readFile(XTMods.STATUS))`);
  }
  check('the picked zip is handed to the menu', /^picked \d+ \/xt-user\/luastate\/incoming\/hello\.zip/.test(st), st);
  lines = await run(`lua_mod_install "${(st.trim().split(' ')[2] || '')}"`, /lua_install_confirm|Refused|failed/i);
  check('the picked zip reaches the review', lines.some((l) => /lua_install_confirm|review/i.test(l)), lines);
  lines = await run('lua_install_confirm', /installed|Refused|failed/i);
  check('the picked zip is installed', lines.some((l) => /installed/i.test(l)), lines);
}

// 4. A dropped zip goes the same way.
await run('lua_mod_uninstall hello', /uninstall|removed|hello/i);
const b64 = fs.readFileSync(zip).toString('base64');
const before = await evaluate('xt.state.logLines || 0');
await evaluate(`(() => {
  const bytes = Uint8Array.from(atob(${JSON.stringify(b64)}), (c) => c.charCodeAt(0));
  const dt = new DataTransfer();
  dt.items.add(new File([bytes], 'hello.zip', { type: 'application/zip' }));
  document.dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true, cancelable: true }));
})()`);
let dropLines = [];
for (let i = 0; i < 50; i++) {
  await sleep(200);
  dropLines = await evaluate(`(() => { const l = xt.log(); const n = Math.min(l.length, (xt.state.logLines || 0) - ${before}); return l.slice(l.length - n); })()`) || [];
  if (dropLines.some((l) => /lua_install_confirm|Refused|failed/i.test(l))) break;
}
check('a dropped zip reaches the review', dropLines.some((l) => /lua_install_confirm|review/i.test(l)), dropLines);
lines = await run('lua_install_confirm', /installed|Refused|failed/i);
check('the dropped zip is installed (switched off)', lines.some((l) => /installed/i.test(l)), lines);

// 5. xt.http from a running mod, to the CORS host.
const hb64 = fs.readFileSync(httpzip).toString('base64');
await evaluate(`(() => { const FS = Module.FS; try { FS.mkdir('/xt-user/luastate'); } catch (e) {} try { FS.mkdir('/xt-user/luastate/incoming'); } catch (e) {}
  FS.writeFile('/xt-user/luastate/incoming/webcheck.zip', Uint8Array.from(atob(${JSON.stringify(hb64)}), (c) => c.charCodeAt(0))); })()`);
lines = await run('lua_mod_install /xt-user/luastate/incoming/webcheck.zip', /lua_install_confirm|Refused|failed/i);
lines = lines.concat(await run('lua_install_confirm', /installed|Refused|failed/i));
lines = lines.concat(await run('lua_mod_permission webcheck grant all; lua_mod_enable webcheck', /webcheck/i, 5000));
check('the http mod is installed and on', lines.some((l) => /installed/i.test(l)), lines);
lines = await run(`lua_webcheck_has`, /webcheck has http/, 5000);
check('xt.game.has("http") is true on the web', lines.some((l) => /webcheck has http true/.test(l)), lines);
lines = await run(`lua_webcheck_get ${cors}/answer.txt`, /webcheck answer/, 15000);
check('xt.http.get gets the answer', lines.some((l) => /webcheck answer 200 hello from the test host/.test(l)), lines);
lines = await run(`lua_webcheck_post ${cors}/echo`, /webcheck answer/, 15000);
check('xt.http.post sends a body', lines.some((l) => /webcheck answer 200 posted:ping/.test(l)), lines);
lines = await run(`lua_webcheck_get ${nocors}/answer.txt`, /webcheck answer/, 15000);
check('xt.http to a host without CORS fails cleanly', lines.some((l) => /webcheck answer -1 $/.test(l) || /webcheck answer -1/.test(l)), lines);

// 6. Everything above lives in the browser: a reload (no settings changed,
// so config.cfg is not rewritten) still has the mods, their grants and state.
await sleep(2500); // the page's save is debounced; a player does not reload in the same second
await send('Page.reload', { ignoreCache: false });
const t1 = Date.now();
let back = false;
while (Date.now() - t1 < 300000) {
  await sleep(250);
  const s = await evaluate(`(() => { const log = window.xt && xt.log ? xt.log() : []; return log.some((l) => /menu/i.test(l) && /loaded|program/i.test(l)); })()`);
  if (s === true) { back = true; break; }
}
check('the page comes back after a reload', back, 'timeout');
await sleep(2000);
check('the welcome does not come back after a reload', (await modal()) === '', await modal());
named = await run('_cl_name', /_cl_name/, 3000);
check('the name is still there after a reload', named.some((l) => /Web Player/.test(l)), named);
lines = await run('lua_mods', /webcheck/i);
check('installed mods are still there after a reload', lines.some((l) => /^\s*hello\b/.test(l)) && lines.some((l) => /^\s*webcheck\s+(on|running)\b/.test(l)), lines);
lines = await run('lua_webcheck_has', /webcheck has http/, 8000);
check('the enabled mod runs again after a reload', lines.some((l) => /webcheck has http true/.test(l)), lines);

finish();

function finish() {
  console.log(JSON.stringify({ passed: results.filter((r) => r.ok).length, failed }));
  try { ws.close(); } catch (e) { /* gone */ }
  process.exit(failed ? 1 : 0);
}
