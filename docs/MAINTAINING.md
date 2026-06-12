# Maintainer guide (no local compile)

This project is designed so **you edit source and others build**. That saves disk, CPU, and time on your machine.

## Daily workflow

1. Clone this repo — `engine/` already contains the full Xonotic fork with Ubuntu Touch changes integrated.
2. Edit files under `engine/` directly (see [SOURCES.md](SOURCES.md)).
3. Commit and push your work in this repo.
4. Ask a Clickable tester to run `clickable build --arch arm64` on your branch.

## Local builds (optional)

Do **not** run `cd engine && ./all compile` unless you explicitly want a raw upstream build.

Native compile and run:

```bash
./scripts/compile-and-install-deps.sh
./scripts/run-local-no-clickable.sh
```

Clickable testers use `./scripts/run-clickable.sh --container --install` (see [TESTING.md](TESTING.md)).

## Sharing changes

`engine/` source is **committed in this repo** (minus build artifacts and large binary assets). Push branches/PRs here like any normal project.

| Method | When |
|--------|------|
| **Commit in this repo** | Default — edit under `engine/`, `git add`, push (run `prepare-engine-for-git.sh` once if nested `.git` dirs remain) |
| **GitLab fork sub-repos** | Push `engine/darkplaces`, `engine/data/xonotic-data.pk3dir` to your forks; sync upstream with `sync-upstream-fork.sh` |

Large textures/models/sound are **not** in git (~3 GB). Testers fetch them with `./scripts/fetch-sources.sh assets` before a playable build.

## Fork workflow (pull upstream, keep UT changes)

Ubuntu Touch patches live **in the codebase** under `engine/` — not as separate overlay dirs or patch files.

1. Fork on GitLab: [xonotic](https://gitlab.com/xonotic/xonotic), [darkplaces](https://gitlab.com/xonotic/darkplaces), [xonotic-data.pk3dir](https://gitlab.com/xonotic/xonotic-data.pk3dir).
2. Initialize git in sub-repos (once, if vendored without `.git`):

```bash
./scripts/sync-upstream-fork.sh --init-git
```

3. Merge upstream and push to your forks:

```bash
FORK_DARKPLACES=https://gitlab.com/you/darkplaces.git \
FORK_DATA=https://gitlab.com/you/xonotic-data.pk3dir.git \
  ./scripts/sync-upstream-fork.sh
```

Resolve merge conflicts in `engine/` — UT-specific files include `touch_*.qc`, menu touch dialogs, and darkplaces multitouch builtins.

4. Re-vendor for monorepo commit if needed:

```bash
./scripts/prepare-engine-for-git.sh --yes
git add engine/
```

## Disk usage

| Fetch | Approx. role |
|-------|----------------|
| `minimal` | Engine C only (~100 MB darkplaces) |
| `code` | UI + QuakeC + compile deps (source committed in git; assets fetched separately) |
| `assets` | Binary pk3dir assets only (textures, models, sound, …) |
| `full` | Entire game + maps/music via `./all update` (needs `engine/.git`) |

After an accidental compile: `./scripts/clean-local-artifacts.sh`

## Screen / landscape layout

Edit **`touch/screen-calc.sh`** for detection and landscape sizing. No compile:

```bash
./scripts/test-screen-calc.sh
XONOTIC_SCREEN_WIDTH=1224 XONOTIC_SCREEN_HEIGHT=2700 ./scripts/test-screen-calc.sh
```

See [SCREEN.md](SCREEN.md). QuakeC HUD should use `vid_width` / `vid_height` cvars, not hardcoded pixels.

## Touch controls

Edit **`touch/profiles/*.cfg`** for preset bundles (no compile). Full cvar schema and CSQC checklist: [CONTROLS.md](CONTROLS.md).

```bash
XONOTIC_TOUCH_PROFILE=casual XONOTIC_TOUCH_PERF_PROFILE=battery ./scripts/test-screen-calc.sh
# Profile env vars apply when packaging/start.sh runs on device; validate cfg syntax locally:
grep -E '^touch_' touch/profiles/standard.cfg
```

## What testers need

- **Compile + UI changes**: `code` fetch (default in `clickable.json` prebuild).
- **Play in-game on device**: `assets` fetch before build, or ship map pk3s separately.
