# Godnotic — agent notes

## Coordinating agents: agent-locks

This repo is set up for [agent-locks](https://github.com/luohoa97/agent-locks) (`.mcp.json`).
Locks are markdown files under `<git-common-dir>/agents-locks/`, so they are never committed.

Parallel agents share the main checkout. Never launch an agent with `isolation: "worktree"`
or call EnterWorktree: every worktree rebuilds the engine and the Flatpak, and ten of them
filled the disk on 2026-09-23. A global hook refuses both in this repo.

Any agent that edits files here:

1. `lock_query` to see who is working on what, then `lock_check_conflict` with the globs you
   are about to touch. Neither blocks you. If an active lock overlaps, keep off those files or
   coordinate through the agent that holds the lock.
2. `lock_create` with a title, the globs you will touch, and your task checklist.
3. `lock_update` as each task is finished, not in one batch at the end.
4. `lock_finish` with a one-line summary when the work is committed.

Scope globs are repo-relative (`engine/src/input/**`). Keep them as narrow as the work
allows: a lock on `engine/**` tells nobody anything.

## Committing and pushing

Pushing to `main` publishes a release. Put `[skip ci]` in the message of commits that only
touch docs, tests, CI or agent config. GitHub skips the whole push when its *last* commit
says `[skip ci]`, so push such a commit on its own, never at the end of a push that carries
code.

The shared checkout's `main` is often behind `origin/main` and holds other agents'
uncommitted work. Before committing, `git fetch origin` and look at `git status -sb`:

- Stage only your own files. Never `git add -A`, `git stash`, `git reset` or `git checkout .`
  in the shared checkout: the other changes belong to running agents.
- If `main` is behind or diverged, do not rebase or pull there. Make the commit in a scratch
  clone (`git clone --shared --no-checkout <repo> <tmp>`, then
  `git -C <tmp> checkout -B main origin/main`), copy in only your files, commit, push, then
  remove your copies from the shared checkout so its next pull is clean.
- Base every edit on `origin/main`'s version of the file. A copy from the shared checkout
  (or from memory) silently reverts whatever landed since.

## Upstream code under engine/

Most of `engine/` is upstream Xonotic. `upstream.lock` and the `upstream/vendor` branch
record exactly which upstream files we carry, and `.github/workflows/upstream-watch.yml`
merges new upstream daily. Every line Touch changes in an upstream file is a line that
can conflict at the next sync. See [`docs/UPSTREAM_SYNC.md`](docs/UPSTREAM_SYNC.md).

- **Ask before editing a file under `engine/`:**
  `python3 scripts/upstream-sync.py patched <file>` says whether it is upstream code.
- **Put Touch logic in Touch-owned files** (`engine/darkplaces/touch_*.c/.h`,
  `qcsrc/client/touch_*.qc/.qh`, `qcsrc/menu/xonotic/dialog_touch_*`, `touch/*.cfg`).
  In an upstream file add only the hook: a call on its own line, marked `// Touch:`.
- **Do not restyle upstream code**: no reformatting, re-indenting, renaming, reordering,
  typo or warning fixes in passing. Add lines rather than rewriting upstream's.
- **Override rather than replace** upstream strings, defaults and blocks when a Touch-side
  override works (a `touch/*.cfg` default, a Touch dialog, a hook before or after).
- **Never bring upstream code in by hand**: no copying upstream files, cherry-picking
  GitLab commits or syncing one directory. Only `scripts/upstream-sync.py` does that.
  Never edit `upstream.lock` by hand, and never push to or rewrite `upstream/vendor`.
- **Do not run** `qcsrc/tools/genmod.sh`; edit `_mod.inc` / `_mod.qh` includes by hand.
- **Upstream sync work** (a sync PR, a conflict, an `upstream-sync` issue, a failing
  upstream-watch run): load the `upstream-sync` skill first. While a sync PR is open,
  check its files (`gh pr diff <n> --name-only`) before editing upstream files, and say
  in your lock which sync PR you are racing.

## Skills

Project skills live in `.claude/skills/<name>/SKILL.md`:

- `upstream-sync` — merging upstream Xonotic and resolving sync conflicts.
