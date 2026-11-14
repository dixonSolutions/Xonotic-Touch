// SPDX-License-Identifier: GPL-3.0-or-later
// The menu of the web build in a real headless Chrome (CDP): the page loads its
// data, the engine starts with its default configuration, and the menu that
// comes up must be the described one (Home with three cards), drawn by the
// QuakeC renderer. Then real pointer and key events go through the browser:
// the welcome's name field (with the engine's keys on a phone), a click on
// a card, a click on a map tile, Escape, the classic dialog.
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
// What the engine printed after a command. The page keeps only the last 200
// lines (xt.log), so a count taken before the command stops moving once the
// tail is full and every line after it looked missing: the checks after the
// first Escape failed whenever the engine had printed enough by then. A marker
// echoed just before the command finds its place in the tail instead.
let markN = 0;
const run = async (line) => { const m = `XTMARK${++markN}`; await command(`echo ${m}`); await command(line); return m; };
const after = (lines, m) => { for (let i = lines.length - 1; i >= 0; i--) if (lines[i].includes(m)) return lines.slice(i + 1); return []; };
const ask = async (line, match, tries = 20) => {
  const m = await run(line);
  for (let i = 0; i < tries; i++) {
    await sleep(150);
    const l = after(await logLines(), m).find(match);
    if (l) return l;
  }
  return '';
};
// the description, read back through the engine: uib_dump prints the snapshot
const snapshot = async () => {
  for (let k = 0; k < 2; k++) {
    const l = await ask('uib_dump', (x) => x.includes('"t":"snapshot"'), 40);
    try { const j = JSON.parse(l.slice(l.indexOf('{'))); return Object.fromEntries(j.nodes.map((n) => [n.id, n])); } catch (e) { /* cut line: again */ }
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
    const m = /FOUND ([\d.]+) ([\d.]+) ([\d.]+) ([\d.]+)/.exec(await ask('echo FOUND $_uib_r_found', (x) => x.includes('FOUND '), 3));
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
const uibDefault = await ask('echo UIBCVAR $menu_touch_uib', (x) => x.includes('UIBCVAR'));
console.log('     ' + ((await ask('echo SCALE dpi=$vid_touch_dpi w=$vid_width h=$vid_height cw=$vid_conwidth ch=$vid_conheight sbar=$scr_sbarscale touch=$vid_touchscreen det=$vid_touchscreen_detected insets=[$_touch_safe_insets] dpr=',
  (x) => x.includes('SCALE'))) || 'no scale line').trim() + (await evaluate('window.devicePixelRatio')) + ' css=' + (await evaluate('innerWidth + "x" + innerHeight')));
check(/UIBCVAR 1/.test(uibDefault), 'menu_touch_uib is 1 without anyone setting it (' + uibDefault.trim() + ')');
check(JSON.stringify(stack(n)) === '["home"]' && n['home/tiles/sp'] && n['home/tiles/campaign'] && n['home/tiles/mp'] && !n['home/hero'],
  'the menu that comes up is Home with the three cards');
// a new player first picks a name: the welcome over Home, through real input
check(JSON.stringify(n.app && n.app.props.modal) === '["welcome"]', 'a new player gets the welcome over Home');
const wp = await rectOf('welcome/form/name');
check(!!wp, 'the QuakeC renderer drew the Name field');
if (wp) await click(wp.x, wp.y);
await sleep(1200);
// A phone (a coarse pointer and no fine one) has no keys: a tap on the field
// brings up the engine's own keys (touch_ui.c), and the field moves above them.
// They type through the same key events as uib_input. Headless Chrome emulating
// a phone may still report the host's mouse as a fine pointer; the engine then
// treats it as a laptop with a touchscreen and takes key events from the page.
const touchOnly = touch && await evaluate("matchMedia('(any-pointer: coarse)').matches && !matchMedia('(any-pointer: fine)').matches");
if (touch && !touchOnly) console.log('note the emulated phone reports a fine pointer too: no screen keys, typing through key events');
if (touchOnly) {
  await sleep(600);
  const kt = Number(((await ask('echo KEYSTOP $_touch_menu_keys_top', (x) => x.includes('KEYSTOP'))).split('KEYSTOP ')[1] || '0').trim());
  const fr = await rectOf('welcome/form/name');
  await shot('web-01a-welcome-keys');
  check(kt > 0 && fr && fr.y / H < kt, `a tap on the Name field brings up the screen keys, with the field above them (keys from ${kt}, field at ${fr && (fr.y / H).toFixed(2)})`);
  await command('uib_input text Webby');
} else {
  for (const ch of 'Webby') {
    await send('Input.dispatchKeyEvent', { type: 'keyDown', key: ch, text: ch, unmodifiedText: ch, code: 'Key' + ch.toUpperCase(), windowsVirtualKeyCode: ch.toUpperCase().charCodeAt(0) });
    await send('Input.dispatchKeyEvent', { type: 'keyUp', key: ch, code: 'Key' + ch.toUpperCase(), windowsVirtualKeyCode: ch.toUpperCase().charCodeAt(0) });
  }
}
await sleep(800);
await shot('web-01b-welcome-typed');
n = await snapshot();
check(n.welcome && n.welcome.props.primary && n.welcome.props.primary.enabled === true, 'typing a name turns Continue on');
if (touchOnly) await command('uib_input key ENTER'); else await key('Enter', 'Enter', 13);
await sleep(1500);
n = await snapshot();
const named = await ask('echo NAMEIS $_cl_name', (x) => x.includes('NAMEIS'));
check(JSON.stringify(n.app && n.app.props.modal) === '[]' && /NAMEIS Webby/.test(named), 'Enter saves the name and closes the welcome (' + named.trim() + ')');
if (touchOnly) check(Number(((await ask('echo KEYSTOP $_touch_menu_keys_top', (x) => x.includes('KEYSTOP'))).split('KEYSTOP ')[1] || '1').trim()) === 0, 'the screen keys go away with the field');
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
