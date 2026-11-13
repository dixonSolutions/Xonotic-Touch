---
name: upstream-sync
description: Merge upstream Xonotic into this repo and resolve sync conflicts without losing Touch changes. Use for an upstream-sync PR or branch, an "Upstream Xonotic sync needs a hand merge" issue, a failing upstream-watch workflow run, conflicts from scripts/upstream-sync.py, or any change to upstream.lock or the upstream/vendor branch.
---

# Upstream sync

Background and the full design: `docs/UPSTREAM_SYNC.md`. The tool is
`scripts/upstream-sync.py` (exit 0 done, 1 conflicts/problems, 2 error, 3 nothing to merge).

## Where to work

Never in the shared checkout (it is behind `origin/main` and holds other agents' edits),
never in a git worktree (CLAUDE.md). Use a scratch clone of `origin/main`:

```bash
# from the shared checkout; SCRATCH is your scratch directory
git clone --shared --no-checkout "$(git rev-parse --show-toplevel)" "$SCRATCH/sync"
cd "$SCRATCH/sync"
git remote set-url origin https://github.com/dixonSolutions/Godnotic
git fetch origin main upstream/vendor:upstream/vendor
git checkout -B upstream-sync origin/main
```

Take an agent-lock on `upstream.lock` and the conflicted files. Check `lock_query` for
active locks on files upstream changed (`git diff --name-only <old vendor> upstream/vendor`)
and name those overlaps in the PR.

## Steps

1. `python3 scripts/upstream-sync.py check`, then `vendor` if upstream moved past the
   vendor branch. Push `upstream/vendor` (fast-forward only) before anything that names it.
2. `python3 scripts/upstream-sync.py resolve`. It refuses if HEAD lacks `origin/main`.
3. For each conflicted file, read both sides before editing:
   - upstream's change: `git diff <old vendor> upstream/vendor -- <file>`
   - Touch's change: `git diff <old vendor> origin/main -- <file>`

   (`<old vendor>` is the `vendor` line of `upstream.lock` on `origin/main`.) Then edit the
   conflict markers in place: keep upstream's change and re-apply the Touch change on top
   of it. Never paste a version of the file from elsewhere or from memory.
4. Commit (it is a merge commit with the vendor commit as second parent), then
   `python3 scripts/upstream-sync.py hooks`. It must pass: it catches a dropped Touch
   addition and leftover conflict markers.
5. Build and test (below), push `upstream-sync`, open a PR against `main` with the
   report from `merge --report` or `resolve`, the resolutions, and what was and was not
   tested. Never push a sync to `main` directly; merging the PR publishes a release.

## Common conflicts

- **Upstream restyled lines Touch changed** (both 2026-11-09 conflicts): write the Touch
  behaviour in upstream's new form. Do not restore the old style.
- **Upstream rewrote a string Touch replaced**: keep the Touch string; consider moving
  the override out of the upstream file.
- **Upstream moved or renamed a function Touch hooks**: move the hook to the new place;
  `hooks` reports the token missing if you forget.
- **Upstream deleted a file Touch patched**: find where the code went (`git log` in the
  upstream repo) and re-hook there; do not resurrect the file.
- **Translations (`*.po`, `*.pot`, `languages.txt`)**: take upstream's.
- **`_mod.inc` / `_mod.qh`**: merge the include lines by hand. Never run `genmod.sh`.

## Build and test

```bash
. scripts/lib/xonotic-shlib.sh
QCCFLAGS_WATERMARK=upstream-sync xonotic_compile_qc_only     # -Werror; must pass
xonotic_compile_engine_only     # if engine/darkplaces changed (host needs SDL2 headers)
for t in $(grep -oE 'bash scripts/test-[A-Za-z0-9_.-]+\.sh' .github/workflows/tests.yml | awk '{print $2}' | sort -u); do bash "$t"; done
```

Then play a headless match against the old code as a baseline (compile `origin/main`'s
QuakeC in a second clone):

- basedir `data/`: symlink the newest autobuild pk3s (`~/.cache/xt-web/Xonotic/data/*.pk3`)
  and the tree's `engine/data/font-*.pk3dir`; link the tree's `engine/data/xonotic-data.pk3dir`
  as `zz-xonotic-data.pk3dir`; copy `touch/gfx` and `touch/xonotic.cfg`.
- run `SDL_VIDEODRIVER=offscreen engine/darkplaces/darkplaces-sdl -xonotic -basedir B
  -userdir B/home +set sv_cheats 1 +port <unique> +exec smoke.cfg +map stormkeep +togglemenu 0`
- `smoke.cfg`: `set g_warmup 0; set minplayers 5; set bot_number 4`, then `defer` (wall
  clock, the map takes >10 s to load) `togglemenu 0` at 18 and 22 s (a fresh profile opens
  the first-run menu, which pauses a local game), `cmd join` at 25, fire, `kill`,
  `screenshot name.tga`, `status` near the end, `quit` at 120.
- Compare the error and warning lines of both logs; they should match. A host without
  libjpeg shows missing icons in both runs.

## Afterwards

When the PR merges, main's `upstream.lock` names the new vendor commit and the daily run
goes quiet. The workflow closes a stale "needs a hand merge" issue by itself.
