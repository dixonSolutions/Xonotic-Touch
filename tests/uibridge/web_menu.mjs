// SPDX-License-Identifier: GPL-3.0-or-later
// The menu of the web build in a real headless Chrome (CDP): the page loads its
// data, the engine starts with its default configuration, and the menu that
// comes up must be the described one (Home with three cards), drawn by the
// QuakeC renderer. Then real pointer and key events go through the browser:
// a click on a card, a click on a map tile, Escape, the classic dialog.
//
//   tests/web-cache/e2e.sh-style Chrome on --port N, then
//   node tests/uibridge/web_menu.mjs --port N --url http://127.0.0.1:8080/ --out DIR [--width 1280 --height 720] [--touch]
//
// Prints one "ok"/"FAIL" line per check; screenshots go to DIR.
import fs from 'node:fs';

const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf('--' + n); return i < 0 ? d : args[i + 1]; };
const port = Number(opt('port'));
const url = opt('url');
const out = opt('out', '.');
const W = Number(opt('width', '1280')), H = Number(opt('height', '720'));
const touch = args.includes('--touch');
fs.mkdirSync(out, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let ws;
for (let i = 0; i < 100 && !ws; i++) {
  try {
    const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    const page = list.find((t) => t.type === 'page');
    if (page) ws = new WebSocket(page.webSocketDebuggerUrl);
  } catch (e) { await sleep(100); }
}
await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
let id = 0;
const pending = new Map();
ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } };
const send = (method, params = {}, ms = 30000) => new Promise((resolve) => {
  const i = ++id;
  const t = setTimeout(() => { pending.delete(i); resolve({ timeout: true }); }, ms);
  pending.set(i, (m) => { clearTimeout(t); resolve(m); });
  ws.send(JSON.stringify({ id: i, method, params }));
});
const evaluate = async (expr) => {
  const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true, userGesture: true });
  return r.result && r.result.result ? r.result.result.value : undefined;
};
let fail = 0;
const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) fail = 1; };
const shot = async (name) => {
  const r = await send('Page.captureScreenshot', { format: 'png' });
  if (r.result && r.result.data) fs.writeFileSync(`${out}/${name}.png`, Buffer.from(r.result.data, 'base64'));
};
// the engine's console log as the page keeps it, and a console command
const logLines = async () => (await evaluate('window.xt && xt.log ? xt.log() : []')) || [];
const command = (line) => evaluate(`xt.command(${JSON.stringify(line)})`);
// the description, read back through the engine: uib_dump prints the snapshot
const snapshot = async () => {
  const before = (await logLines()).length;
  await command('uib_dump');
  for (let i = 0; i < 40; i++) {
    await sleep(150);
    const l = (await logLines()).slice(before).find((x) => x.includes('"t":"snapshot"'));
    if (l) { try { const j = JSON.parse(l.slice(l.indexOf('{'))); return Object.fromEntries(j.nodes.map((n) => [n.id, n])); } catch (e) { /* cut line */ } }
  }
  return {};
};
const stack = (nodes) => (nodes.app && nodes.app.props.stack) || [];
const click = async (x, y) => {
  if (touch) {
    await send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x, y }] });
    await sleep(60);
    await send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
  } else {
    await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x, y });
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
    await sleep(60);
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
  }
};
const key = async (k, code, vk) => {
  await send('Input.dispatchKeyEvent', { type: 'rawKeyDown', key: k, code, windowsVirtualKeyCode: vk });
  await send('Input.dispatchKeyEvent', { type: 'keyUp', key: k, code, windowsVirtualKeyCode: vk });
};
// where an item of the menu is: the renderer reports it for tests (_uib_r_find)
const rectOf = async (node) => {
  await command(`set _uib_r_find "${node}"`);
  let r = null;
  for (let i = 0; i < 20 && !r; i++) {
    await sleep(150);
    const before = (await logLines()).length;
    await command('echo FOUND $_uib_r_found');
    await sleep(150);
    const l = (await logLines()).slice(before).find((x) => x.includes('FOUND '));
    const m = l && /FOUND ([\d.]+) ([\d.]+) ([\d.]+) ([\d.]+)/.exec(l);
    if (m) r = m.slice(1).map(Number);
  }
  await command('set _uib_r_find ""');
  return r && { x: (r[0] + r[2] / 2) * W, y: (r[1] + r[3] / 2) * H };
};

await send('Page.enable');
await send('Runtime.enable');
await send('Emulation.setFocusEmulationEnabled', { enabled: true });
await send('Emulation.setDeviceMetricsOverride', { width: W, height: H, deviceScaleFactor: touch ? 2.75 : 1, mobile: touch });
if (touch) await send('Emulation.setTouchEmulationEnabled', { enabled: true });
await send('Page.navigate', { url });
const t0 = Date.now();
let menu = false, clicked = false;
while (!menu && Date.now() - t0 < 600000) {
  await sleep(300);
  const s = await evaluate(`(() => {
    const vis = (id) => { const e = document.getElementById(id); return !!e && !e.hidden; };
    const log = window.xt && xt.log ? xt.log() : [];
    return { download: vis('xt-download'), failed: vis('xt-error'), err: (document.getElementById('xt-error-text') || {}).textContent || '',
             menu: log.some((l) => /menu/i.test(l) && /loaded|program/i.test(l)) };
  })()`);
  if (!s) continue;
  if (s.failed) { check(false, 'the page loads: ' + s.err); process.exit(1); }
  if (s.download && !clicked) { clicked = true; await evaluate(`document.getElementById('xt-dl-go').click()`); }
  menu = s.menu;
}
check(menu, `the engine's menu program started (${Math.round((Date.now() - t0) / 1000)} s after navigation)`);
await sleep(6000);
await shot('web-01-home');
let n = await snapshot();
const uibDefault = (await (async () => { const b = (await logLines()).length; await command('echo UIBCVAR $menu_touch_uib'); await sleep(400); return (await logLines()).slice(b).find((x) => x.includes('UIBCVAR')) || ''; })());
{ const b = (await logLines()).length; await command('echo SCALE dpi=$vid_touch_dpi w=$vid_width h=$vid_height cw=$vid_conwidth ch=$vid_conheight sbar=$scr_sbarscale touch=$vid_touchscreen det=$vid_touchscreen_detected insets=[$_touch_safe_insets] dpr=' ); await sleep(400);
  console.log('     ' + ((await logLines()).slice(b).find((x) => x.includes('SCALE')) || 'no scale line').trim() + (await evaluate('window.devicePixelRatio')) + ' css=' + (await evaluate('innerWidth + "x" + innerHeight'))); }
check(/UIBCVAR 1/.test(uibDefault), 'menu_touch_uib is 1 without anyone setting it (' + uibDefault.trim() + ')');
check(JSON.stringify(stack(n)) === '["home"]' && n['home/tiles/sp'] && n['home/tiles/campaign'] && n['home/tiles/mp'] && !n['home/hero'],
  'the menu that comes up is Home with the three cards');
let p = await rectOf('home/tiles/sp');
check(!!p, 'the QuakeC renderer drew the Singleplayer card' + (p ? ` at ${Math.round(p.x)},${Math.round(p.y)}` : ''));
if (p) await click(p.x, p.y);
await sleep(2500);
await shot('web-02-singleplayer');
n = await snapshot();
check(JSON.stringify(stack(n)) === '["home","sp"]', 'a real ' + (touch ? 'tap' : 'click') + ' on the card opens Singleplayer');
p = await rectOf('sp/maps/list#3');
if (p) await click(p.x, p.y);
await sleep(1500);
await shot('web-03-map-chosen');
n = await snapshot();
const sel = n['sp/maps/list'] && n['sp/maps/list'].props.selected;
check(JSON.stringify(stack(n)) === '["home","sp"]' && sel && sel[0] && n.app.props.context === 'menu', `one ${touch ? 'tap' : 'click'} on a map tile chooses it (${sel}) and starts nothing`);
if (!touch) {
  // the pointer resting on another tile: its picture stays a picture
  const q = await rectOf('sp/maps/list#6');
  if (q) await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: q.x, y: q.y });
  await sleep(1500);
  await shot('web-03b-pointer-on-a-tile');
}
await key('Escape', 'Escape', 27);
await sleep(1500);
n = await snapshot();
check(JSON.stringify(stack(n)) === '["home"]', 'Escape goes back to Home');
p = await rectOf('home/tiles/mp');
if (p) await click(p.x, p.y);
await sleep(2500);
await shot('web-04-multiplayer');
n = await snapshot();
check(JSON.stringify(stack(n)) === '["home","mp"]' && n['mp/join/go'] && n['mp/join/go'].props.enabled === false, 'Multiplayer opens; its one Join is off until something is chosen');
await key('Escape', 'Escape', 27);
await sleep(1200);
// a classic dialog over the menu, and back
await command('uib_event {"ev":"action","node":"home/nav/settings","action":"settings"}');
await sleep(1200);
await command('uib_event {"ev":"value","node":"settings/split","value":"menu"}');
await sleep(1200);
await shot('web-05-settings-menu');
p = await rectOf('settings/split/menu/classic/advanced');
if (p) await click(p.x, p.y);
await sleep(3000);
await shot('web-06-classic-advanced-settings');
await key('Escape', 'Escape', 27);
await sleep(2500);
await shot('web-07-back-in-the-menu');
n = await snapshot();
check(JSON.stringify(stack(n)) === '["home","settings"]', 'Escape closed the classic dialog and left the described Settings screen');
p = await rectOf('settings/split/menu/classic/advanced');
check(!!p, 'the QuakeC renderer draws again after the classic dialog (the old launcher does not show)');
console.log(fail ? 'web_menu: FAILED' : 'web_menu: all passed');
process.exit(fail);
