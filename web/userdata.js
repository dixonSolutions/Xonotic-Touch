// Godnotic web: everything the game writes stays in the browser.
//
// The user folder (/xt-user: settings, touch layouts, Lua mods and their
// files, which mods are on and what they may do, what mods save, screenshots,
// demos) lives in memory while the game runs, and IndexedDB holds the copy
// that survives a reload. shell.js copies it over when config.cfg is rewritten
// and when the tab is hidden; neither happens when a mod is installed, turned
// on, granted a permission or saves its own files, so those were lost to a
// reload that came first.
//
// This walks the folder once a second (a few hundred entries: well under a
// millisecond) and asks for a save when anything in it changed: a file
// written, added or removed. shell.js coalesces the saves (Module.xt.syncFS).
(function () {
  'use strict';

  const USERDIR = '/xt-user'; // shell.js mounts the userdir here (tests/web-mods checks it)
  const MAX_ENTRIES = 20000;  // a folder this big is not walked every second

  // "path size mtime" for every entry under root, sorted; null when there is no root
  function signature(FS, root) {
    const out = [];
    const walk = (dir) => {
      let names;
      try { names = FS.readdir(dir); } catch (e) { return; }
      for (const n of names) {
        if (n === '.' || n === '..') continue;
        if (out.length >= MAX_ENTRIES) return;
        const p = `${dir}/${n}`;
        let st;
        try { st = FS.lstat(p); } catch (e) { continue; }
        const mtime = st.mtime instanceof Date ? st.mtime.getTime() : +st.mtime || 0;
        const isDir = FS.isDir(st.mode);
        out.push(`${p} ${isDir ? 'd' : st.size} ${isDir ? '' : mtime}`);
        if (isDir) walk(p);
      }
    };
    try { FS.lstat(root); } catch (e) { return null; }
    walk(root);
    out.sort();
    return out.join('\n');
  }

  // A watcher: tick() returns true when the folder changed since the last tick
  // (the first tick only takes the starting point).
  function watcher(FS, root = USERDIR) {
    let last = null;
    return {
      tick() {
        const now = signature(FS, root);
        if (now === null) return false;
        if (last === null) { last = now; return false; }
        if (now === last) return false;
        last = now;
        return true;
      },
    };
  }

  function start() {
    let w = null;
    setInterval(() => {
      const M = window.Module;
      if (!M || !M.FS || !window.xt || !xt.state || !xt.state.started) return;
      if (!w) w = watcher(M.FS);
      if (w.tick() && M.xt && M.xt.syncFS) M.xt.syncFS();
    }, 1000);
  }

  const api = { signature, watcher, USERDIR };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else { window.XTUserData = api; start(); }
})();
