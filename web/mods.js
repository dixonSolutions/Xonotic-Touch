// Godnotic web: installing Lua mods from a file.
//
// The Mods tab (Settings → Mobile → Mods → Install) asks the platform for a
// file through two files in the engine's write folder, the same handshake
// Android's ModImport answers (qcsrc/menu/xonotic/dialog_touch_mods.qh):
//
//   touch/mod-import-status.txt   one line; that it exists at all is what makes
//                                 the menu show "Pick file"
//   touch/mod-import-request.txt  the menu writes it when "Pick file" is pressed
//
// This page answers with the browser's file picker. The picked .zip is copied,
// unchanged and only if it is at most 8 MB, to <userdir>/luastate/incoming/,
// and the status becomes "picked <time> <path>"; the menu then runs
// lua_mod_install on it, so the engine checks it and shows its review like any
// other package. Nothing here opens the archive.
//
// A .zip dropped on the page goes the same way, then straight to
// lua_mod_install. It is installed switched off with nothing granted, as
// always: the review in the Mods tab is where the player decides.
(function () {
  'use strict';

  const MAX = 8 * 1024 * 1024; // the engine's limit for a package (touch_lua_pkg.c)
  const USERDIR = '/xt-user'; // shell.js mounts the userdir here (test-web-mods.sh checks it)
  // QuakeC's fopen writes under <userdir>/<gamedir>/data/
  const WRITE_DIR = `${USERDIR}/data/data`;
  const STATUS = `${WRITE_DIR}/touch/mod-import-status.txt`;
  const REQUEST = `${WRITE_DIR}/touch/mod-import-request.txt`;
  const INCOMING = `${USERDIR}/luastate/incoming`;

  // Mods.name("My Mod (2).zip") === "My_Mod__2_"
  function stem(name) {
    let s = String(name || '').replace(/^.*[\\/]/, '').replace(/\.zip$/i, '');
    s = s.replace(/[^A-Za-z0-9._-]/g, '_').replace(/^[.]+/, '').slice(0, 48);
    return s || 'imported_mod';
  }

  function mkdirs(FS, path) {
    let at = '';
    for (const part of path.split('/').filter(Boolean)) {
      at += '/' + part;
      try { FS.mkdir(at); } catch (e) { /* exists */ }
    }
  }

  function exists(FS, path) {
    try { FS.stat(path); return true; } catch (e) { return false; }
  }

  function make(env) {
    const { FS, command, now = () => Date.now(), sync = () => {} } = env;

    function publish(line) {
      mkdirs(FS, STATUS.replace(/\/[^/]+$/, ''));
      FS.writeFile(STATUS, line + '\n');
    }

    // Copies the bytes to incoming/ and returns the path, or throws a message
    // the player can read.
    function store(name, bytes) {
      if (!bytes || bytes.length === 0) throw new Error('The file is empty');
      if (bytes.length > MAX) throw new Error('The file is over 8 MB, the most a mod may be');
      mkdirs(FS, INCOMING);
      // only the latest pick waits here
      for (const f of FS.readdir(INCOMING)) {
        if (f !== '.' && f !== '..') { try { FS.unlink(`${INCOMING}/${f}`); } catch (e) { /* gone */ } }
      }
      const path = `${INCOMING}/${stem(name)}.zip`;
      FS.writeFile(path, bytes);
      sync();
      return path;
    }

    return {
      publish,
      store,
      // The menu asked for a file: true when it did (the request is consumed
      // first, so one that fails is not retried forever).
      takeRequest() {
        if (!exists(FS, REQUEST)) return false;
        try { FS.unlink(REQUEST); } catch (e) { /* gone */ }
        return true;
      },
      picked(name, bytes) {
        try {
          const path = store(name, bytes);
          publish(`picked ${now()} ${path}`);
          return path;
        } catch (e) {
          publish(e.message || String(e));
          return null;
        }
      },
      dropped(name, bytes) {
        const path = store(name, bytes);
        command(`togglemenu 1\nmenu_cmd directmenu touchmods\nlua_mod_install "${path}"`);
        return path;
      },
    };
  }

  // The page itself: wait for the engine, then answer the menu.
  function start() {
    let mods = null;
    let input = null;

    function ready() {
      const M = window.Module;
      return M && M.FS && window.xt && xt.state && xt.state.started && exists(M.FS, USERDIR);
    }

    function readFile(file) {
      if (file.size > MAX) return Promise.reject(new Error('The file is over 8 MB, the most a mod may be'));
      return file.arrayBuffer().then((ab) => new Uint8Array(ab));
    }

    function pick() {
      if (!input) {
        input = document.createElement('input');
        input.type = 'file';
        input.accept = '.zip,application/zip,application/x-zip-compressed';
        input.hidden = true;
        input.addEventListener('change', () => {
          const file = input.files && input.files[0];
          input.value = '';
          if (!file) { mods.publish('Import cancelled'); return; }
          mods.publish('Reading the file...');
          readFile(file).then((bytes) => mods.picked(file.name, bytes), (e) => mods.publish(e.message || String(e)));
        });
        input.addEventListener('cancel', () => mods.publish('Import cancelled'));
        document.body.appendChild(input);
      }
      // The press in the menu is a fresh user activation, which a file picker
      // needs; the request reaches us within a frame or two of it.
      try { input.click(); mods.publish('Pick the mod’s .zip'); } catch (e) { mods.publish('This browser would not open a file picker'); }
    }

    const timer = setInterval(() => {
      if (!ready()) return;
      const M = window.Module;
      if (!mods) {
        mods = make({ FS: M.FS, command: (t) => xt.command(t), sync: () => M.xt && M.xt.syncFS && M.xt.syncFS() });
        mods.publish(' ');
      }
      if (mods.takeRequest()) pick();
    }, 150);

    document.addEventListener('dragover', (e) => {
      if (!mods || !e.dataTransfer || ![...(e.dataTransfer.types || [])].includes('Files')) return;
      e.preventDefault();
      e.dataTransfer.dropEffect = 'copy';
    });
    document.addEventListener('drop', (e) => {
      const file = e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files[0];
      if (!mods || !file || !/\.zip$/i.test(file.name)) return;
      e.preventDefault();
      readFile(file).then((bytes) => mods.dropped(file.name, bytes))
        .catch((err) => xt.command(`echo "Mod not installed: ${String(err.message || err).replace(/"/g, "'")}"`));
    });
    return () => clearInterval(timer);
  }

  const api = { make, stem, MAX, USERDIR, STATUS, REQUEST, INCOMING };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else { window.XTMods = api; start(); }
})();
