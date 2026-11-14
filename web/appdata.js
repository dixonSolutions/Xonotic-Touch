// Godnotic in the browser: the game data of an installed Godnotic app.
//
// A web page cannot look at the device's files, so it cannot find an install
// by itself. What every browser has is a file picker, and the installed app
// can put its game data in it:
//
//   Android       the app's GameDataProvider (a DocumentsProvider) lists its
//                 packs as "Godnotic" in the system picker that
//                 <input type=file> opens (the app's own folder under
//                 Android/data is out of the picker's reach since Android 11)
//   Linux         the Flatpak's data folder, picked by path (it is hidden);
//                 desktop Chromium uses the folder picker instead (shell.js,
//                 "an existing Godnotic folder"), which reads it in place
//   Ubuntu Touch  Morph's picker is the Content Hub, which lists apps that
//                 are content sources; the click package is not one yet
//
// The packs picked are checked (whole zips, a data and a maps pack, textures
// this GPU can draw) and copied once into the browser's own files (OPFS,
// directory xt-appdata), at the speed of the disk: nothing comes from the
// network, and later visits need neither the picker nor the app. At start
// they are used exactly like a picked folder (shell.js mounts them in
// place); the site still supplies this build's own files.
'use strict';

(function (global) {
  const DIR = 'xt-appdata';
  const INDEX = 'index.json';

  async function dir(create) {
    const root = await global.navigator.storage.getDirectory();
    return root.getDirectoryHandle(DIR, { create });
  }

  /** Can this browser keep the packs (OPFS with createWritable)? */
  function supported() {
    const nav = global.navigator;
    const FH = global.FileSystemFileHandle;
    return !!(nav.storage && nav.storage.getDirectory && FH && 'createWritable' in FH.prototype);
  }

  /**
   * Let the player pick files. Resolves with the File list, or [] when the
   * picker was closed. accept is left out on Android: providers type .pk3
   * files as they like, and a filter that matches none hides them all.
   */
  function pick({ android = false } = {}) {
    return new Promise((resolve) => {
      const input = global.document.createElement('input');
      input.type = 'file';
      input.multiple = true;
      if (!android) input.accept = '.pk3,.cfg';
      input.style.display = 'none';
      let done = false;
      const finish = (files) => {
        if (done) return;
        done = true;
        input.remove();
        resolve(files);
      };
      input.addEventListener('change', () => finish(Array.from(input.files || [])));
      input.addEventListener('cancel', () => finish([]));
      global.document.body.appendChild(input);
      input.click();
    });
  }

  /**
   * Check the picked files: one pack of each kind (the newest when there are
   * more), at least a data and a maps pack, every one a whole zip, textures
   * this GPU can draw. tools: {packKind, zipDirectory, hasBytes, gpu}.
   * Returns {packs: [{kind, name, file, size}], config: File|null}; throws
   * with a sentence for the player.
   */
  async function check(files, tools) {
    const packs = new Map();
    let config = null;
    for (const file of files) {
      const name = file.name.replace(/^.*[\\/]/, '');
      if (name === 'config.cfg') { config = file; continue; }
      const kind = tools.packKind(name, true);
      if (!kind) continue;
      if (!packs.has(kind) || name > packs.get(kind).name) packs.set(kind, { kind, name, file, size: file.size });
    }
    if (!packs.has('data') || !packs.has('maps')) {
      const names = files.map((f) => f.name).slice(0, 4).join(', ');
      throw new Error('That is not the Godnotic game data: it needs xonotic-…-data.pk3 and xonotic-…-maps.pk3 (select all the files at once)' +
        (names ? `. Picked: ${names}${files.length > 4 ? '…' : ''}.` : '.'));
    }
    for (const p of packs.values()) {
      const cd = await tools.zipDirectory(p.file);
      if (!cd) throw new Error(`${p.name} is not complete (is the app still downloading it?)`);
      if (p.kind === 'data' && tools.hasBytes(cd, '.ktx') && !(tools.gpu && tools.gpu.astc)) {
        throw new Error('That is the phone data set (ASTC textures), which this device’s graphics chip cannot draw');
      }
    }
    return { packs: [...packs.values()], config };
  }

  /**
   * Copy checked packs into the browser's files. The index is written last:
   * a copy cut short (a reload, a full disk) is never used. onProgress(done,
   * total, name) in bytes.
   */
  async function save(packs, onProgress) {
    const total = packs.reduce((n, p) => n + p.size, 0);
    await remove();
    const d = await dir(true);
    let done = 0;
    for (const p of packs) {
      const w = await (await d.getFileHandle(p.name, { create: true })).createWritable();
      try {
        const reader = p.file.stream().getReader();
        for (;;) {
          const { done: end, value } = await reader.read();
          if (end) break;
          await w.write(value);
          done += value.byteLength;
          if (onProgress) onProgress(done, total, p.name);
        }
        await w.close();
      } catch (e) {
        try { await w.abort(); } catch (e2) { /* gone */ }
        throw e;
      }
    }
    const index = { saved: Date.now(), packs: packs.map((p) => ({ kind: p.kind, name: p.name, size: p.size })) };
    const w = await (await d.getFileHandle(INDEX, { create: true })).createWritable();
    await w.write(JSON.stringify(index));
    await w.close();
    return index;
  }

  /**
   * The packs kept from an earlier pick, as shell.js's folder source wants
   * them: {name, files: [{kind, name, file, size}], kinds, config: null,
   * app: true}. null when there are none or they are not whole any more.
   */
  async function open() {
    if (!supported()) return null;
    let d;
    try { d = await dir(false); } catch (e) { return null; }
    let index;
    try { index = JSON.parse(await (await (await d.getFileHandle(INDEX)).getFile()).text()); } catch (e) { return null; }
    const files = [];
    for (const p of index.packs || []) {
      let file;
      try { file = await (await d.getFileHandle(p.name)).getFile(); } catch (e) { return null; }
      if (file.size !== p.size) return null; // the browser cleared part of it
      files.push({ kind: p.kind, name: p.name, file, size: file.size });
    }
    if (!files.some((x) => x.kind === 'data')) return null;
    return { name: 'the Godnotic app', files, kinds: new Set(files.map((x) => x.kind)), config: null, app: true };
  }

  async function remove() {
    try { await (await global.navigator.storage.getDirectory()).removeEntry(DIR, { recursive: true }); } catch (e) { /* none */ }
  }

  global.XTAppData = { supported, pick, check, save, open, remove, DIR };
})(typeof window !== 'undefined' ? window : globalThis);
