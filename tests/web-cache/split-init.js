// Run in the page before its own scripts by tests/web-cache/split.sh.
(() => {
  // Headless Firefox has no WebGL, which the page checks for before anything
  // else; the stand-in engine draws nothing.
  const real = HTMLCanvasElement.prototype.getContext;
  HTMLCanvasElement.prototype.getContext = function (type, attrs) {
    const gl = real.call(this, type, attrs);
    if (gl || type !== 'webgl2') return gl;
    return { RENDERER: 1, MAX_TEXTURE_SIZE: 2, getParameter: (p) => (p === 1 ? 'none (test)' : 4096), getExtension: () => null };
  };
  const $ = (id) => document.getElementById(id);
  const uniq = (a) => [...new Set(a)];
  // Blob URLs -> the names of the files they are chunks of.
  const names = (urls) => {
    const by = new Map();
    for (const f of [...(xt.state.files || []), ...xt.prog.counted]) for (const h of f.chunks) by.set(h, f.name);
    return uniq(urls.map((u) => by.get((/([0-9a-f]{64})\.bin/.exec(u) || [])[1]) || u));
  };
  const blobs = () => performance.getEntriesByType('resource').map((r) => r.name).filter((u) => u.includes('/data/blobs/'));
  const E = () => window.__engine;
  window.splitTest = {
    expect: async () => (await fetch('expect.json', { cache: 'no-store' })).json(),
    dialog: () => ({ title: $('xt-dl-title').textContent, size: $('xt-dl-size').textContent, lead: $('xt-dl-size').parentNode.textContent.trim() }),
    // The moment the engine started: what had been fetched, what it found.
    async started() {
      const x = await splitTest.expect();
      const before = names(E().startRequests);
      const own = xt.state.files.filter((f) => !f.stock).map((f) => f.name);
      return {
        before, startPacks: E().startPacks, lazy: xt.state.lazy, store: xt.state.store,
        beforeIsFirstPlay: before.slice().sort().join() === own.concat(x.first).sort().join(),
        packsAreFirstPlay: E().startPacks.join() === own.concat(x.first).filter((n) => n.endsWith('.pk3')).sort().join(),
        hasAll: x.first.concat(x.rest).every((n) => E().startPacks.includes(n)),
        oldLayout: E().startPacks.some((n) => n.startsWith('xonotic-t-')) && !E().startPacks.some((n) => n.startsWith('godnotic-t-')),
        dialogShown: !$('xt-download').hidden,
        coreBsp: (E().entries(x.first[0]) || []).filter((n) => n.endsWith('.bsp')),
        coreMapinfo: (E().entries(x.first[0]) || []).filter((n) => n.endsWith('.mapinfo')).length,
        status0: (E().statuses[0] || [0, ''])[1],
      };
    },
    // Now: what was mounted late, what is readable, what the engine heard.
    async now() {
      const x = await splitTest.expect();
      const st = E().statuses;
      // "At most about twice a second": the most lines in any one second.
      let perSecond = 0;
      for (let i = 0; i < st.length; i++) {
        let j = i;
        while (j < st.length && st[j][0] - st[i][0] < 1000) j++;
        perSecond = Math.max(perSecond, j - i);
      }
      const verify = await E().verify(x.entries);
      return {
        mountpacks: E().mountpacks, restInOrder: E().mountpacks.join() === x.rest.join(), rest: x.rest,
        fetched: names(blobs()), fetchedBeforeStart: names(E().startRequests),
        verify, allFiles: Object.keys(x.entries).length, errors: E().errors, loaded: E().loaded, commands: E().commands, execs: E().execs.join(),
        reads: E().reads, lazyErrors: (xt.lazyStats() || {}).errors || 0,
        statuses: st.length, perSecond, last: (st[st.length - 1] || [0, ''])[1],
        wants: uniq(st.map((s) => (/ want (\S+) /.exec(s[1]) || [])[1]).filter(Boolean)),
        badStatus: st.map((s) => s[1]).filter((l) => !/^touch_data_status state (fetching|paused|waiting|error|complete|legacy) done_kb \d+ total_kb \d+( want \S+ want_done_kb \d+ want_total_kb \d+)?( message "[^"]*")?$/.test(l)),
        card: !$('xt-map').hidden,
      };
    },
    complete: () => E() && E().statuses.some((s) => / state complete /.test(s[1])),
  };
})();
