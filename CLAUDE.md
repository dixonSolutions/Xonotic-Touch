# Xonotic Touch — agent notes

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

Pushing to `main` publishes a release. Put `[skip ci]` in the message of commits that only
touch docs or agent config.
