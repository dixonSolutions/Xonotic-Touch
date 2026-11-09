# Upstream sync

How engine/ follows upstream Xonotic while keeping the Touch port's changes, and
how to write Touch changes so that following upstream stays cheap.

## How it works

- **`upstream.lock`** (repo root) names, for each upstream repository, the commit
  its files under `engine/` come from, plus the `vendor` commit that holds exactly
  those upstream files.
- **`upstream/vendor`** is a branch with only untouched upstream files, laid out as
  under `engine/` here. Paths this repo ignores (textures, models, sounds, …) are
  left out. Each sync adds a commit on top; its history is never rewritten.
- **A sync is a three-way merge with an explicit base**: the vendor commit in
  `upstream.lock` (old upstream), `main` (old upstream + Touch), and the new vendor
  commit (new upstream). Lines upstream did not touch keep our changes; conflicts
  only appear where upstream and Touch changed the same lines.
- **[`scripts/upstream-sync.py`](../scripts/upstream-sync.py)** does every step.
  Except `resolve`, its commands work on git objects only, never the working tree,
  so they are safe in the shared checkout.

| Command | What it does |
|---------|--------------|
| `check` | compare `upstream.lock` with the upstream branch heads |
| `vendor` | add a commit to `upstream/vendor` with the current upstream files (none when only ignored files changed) |
| `merge` | merge a vendor commit into a revision and write the result to a branch (default `upstream-sync`) |
| `resolve` | start the same merge in a clean checkout that contains `origin/main`, conflicts staged as in `git merge` |
| `hooks` | check a merge: every token Touch added to an upstream file is still there, Touch-only files exist, no conflict markers |
| `patched` | list upstream files Touch changed, or say for given files whether they are upstream code |

Exit status: `0` done, `1` conflicts or problems found, `2` error, `3` nothing to merge.

`hooks` needs no list of hooks: it compares old upstream with main to find what
Touch added to each shared file, then checks the merge result still has it on top
of new upstream. Run it after every hand-merge: dropping a Touch hook while
resolving a conflict is the usual way features break.

[`scripts/test-upstream-sync.sh`](../scripts/test-upstream-sync.sh) runs every
command against throwaway repositories (clean sync, asset-only change, conflict
resolved wrongly then rightly, leftover markers, stale checkout, bad input). It
runs in `tests.yml`, so a change to the tool cannot reach main untested.

## Writing Touch changes that survive syncs

Every line Touch changes in an upstream file is a line a future sync can conflict
on. `python3 scripts/upstream-sync.py patched --top 20` shows where we stand
(111 upstream files, about 2,800 added lines, in November 2026). Before editing
a file under `engine/`, ask:

```bash
python3 scripts/upstream-sync.py patched engine/darkplaces/vid_sdl.c
```

- **Put the logic in a Touch-owned file** (`engine/darkplaces/touch_*.c/.h`,
  `qcsrc/client/touch_*.qc/.qh`, `qcsrc/menu/xonotic/dialog_touch_*`, `touch/*.cfg`)
  and add only a call in the upstream file, on its own line, marked `// Touch:`.
- **Leave upstream's lines alone**: no reformatting, re-indenting, renaming,
  reordering, typo fixes or warning fixes in passing. Add lines instead of
  rewriting them.
- **Override instead of replacing** where you can: a cvar default in `touch/*.cfg`,
  a Touch dialog, a hook that runs before or after upstream's code. Both conflicts
  in the 2026-11-09 sync were in-place rewrites: a menu string, and a drag-bounds
  test upstream later restyled.
- **Never bring upstream code in by hand** (copying files, cherry-picking GitLab
  commits, syncing one directory). The merge counts anything that differs from the
  vendor commit as a Touch change, so hand-imported code conflicts at every sync.
  The July 2026 sync took only `qcsrc/`, and the first vendor commit had to record
  two bases to stop 64 files conflicting for nothing.
- **Do not run** `qcsrc/tools/genmod.sh`. Our `_mod.inc` / `_mod.qh` are kept by
  hand: genmod reorders the Touch includes and drops `client/touch_theme.qh`. The
  text merge handles upstream's new entries.

The agent rules in [`AGENTS.md`](../AGENTS.md) say the same, and the
[`upstream-sync` skill](../.claude/skills/upstream-sync/SKILL.md) holds the full
conflict procedure.

## Automation

[`.github/workflows/upstream-watch.yml`](../.github/workflows/upstream-watch.yml) runs
daily at 05:23 UTC and on demand:

1. `check`; when upstream moved, `vendor` and push `upstream/vendor`.
2. If the `upstream-sync` branch already merges that vendor commit (an open PR,
   made by an earlier run or by hand), stop: main's lock catches up when the PR
   is merged.
3. Otherwise `merge` into main:
   - **Nothing to merge** (upstream only touched assets): nothing else happens.
   - **Clean**: `hooks`, push `upstream-sync`, compile QuakeC and the engine, run
     every test script `tests.yml` runs, and open or refresh a PR (a draft when the
     build fails). The `tests` and `android` workflows run on the branch too.
   - **Conflicts**: open or refresh an issue labelled `upstream-sync` with the files.
4. When nothing needs a hand-merge any more, it closes that issue.

It never pushes to `main`. Merging the PR publishes a release, so play a match on
a touch device and on a public server first.

One-time setup:

- The vendor branch is on GitHub (`upstream/vendor`). Done 2026-11-09.
- Settings → Actions → General → **Allow GitHub Actions to create and approve pull
  requests**. Done 2026-11-10. Without it (and without the token below) the clean
  path pushes `upstream-sync` but cannot open the PR, and that job fails. To check:
  `gh api repos/dixonSolutions/Xonotic-Touch/actions/permissions/workflow` should
  show `"can_approve_pull_request_reviews":true`.
- Optional: a fine-grained PAT as secret `UPSTREAM_SYNC_TOKEN` (contents, pull requests,
  issues, actions). Pushes made with it trigger the branch workflows themselves;
  without it the workflow dispatches `tests.yml` and `android.yml` on the branch.

## Merging conflicts by hand

Follow the [`upstream-sync` skill](../.claude/skills/upstream-sync/SKILL.md). In short,
in a fresh clone of `origin/main` (not the shared checkout, not a git worktree):

```bash
git fetch origin upstream/vendor:upstream/vendor
python3 scripts/upstream-sync.py resolve
# edit the conflict markers, keeping upstream's change and every Touch change
git add <files> && git commit
python3 scripts/upstream-sync.py hooks
```

`resolve` refuses to start unless HEAD contains `origin/main`: resolving on a stale
copy silently reverts the Touch work done since. It writes the new `upstream.lock`,
and the commit gets the vendor commit as its second parent. Build, test, push the
`upstream-sync` branch and open a PR.

## History

- **2026-07-27**: hand sync of darkplaces `d93f9c4`, xonotic-data `qcsrc/` only at
  `8bca8a4`, gmqcc `b2a319e`.
- **2026-11-09**: the vendor branch and lock. The first vendor commit (`62d7dc6`)
  records the tree as July left it: xonotic-data `qcsrc/` from `8bca8a4`, its
  other files from `729015d` (2026-06-11), superproject `85dc7f0`, d0_blind_id
  `64983f4`, all found by comparing blob hashes with main.
- **2026-11-09**: first sync with the tool, PR #60: upstream as of that day (175
  xonotic-data commits), two conflicts (`menu/item/button.qc`,
  `menu/xonotic/dialog_firstrun.qc`), both upstream restyles over Touch lines.

`scripts/sync-upstream-fork.sh`, the older nested-git path, is retired: it now only
points here.

Do **not** restore obsolete `client/mapvoting.*` / `server/mapvoting.*`: upstream now
owns `common/mapvoting/`. Touch code includes `<common/mapvoting/_mod.qh>`.
