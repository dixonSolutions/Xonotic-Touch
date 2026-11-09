# Upstream sync

How engine/ follows upstream Xonotic while keeping the Touch port's changes.

## How it works

- **`upstream.lock`** (repo root) names, for each upstream repository, the commit
  its files under `engine/` come from, plus the `vendor` commit that holds exactly
  those upstream files. It replaces the hand-kept SHA table this page used to have.
- **`upstream/vendor`** is a branch with only untouched upstream files, laid out as
  under `engine/` here. Paths this repo ignores (textures, models, sounds, …) are
  left out. Each sync adds a commit on top.
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
| `vendor` | add a commit to `upstream/vendor` with the current upstream files |
| `merge` | merge a vendor commit into a revision and write the result to a branch (default `upstream-sync`) |
| `resolve` | start the same merge in a clean checkout, conflicts staged as in `git merge` |
| `hooks` | check that every token Touch added to an upstream file is still there after a merge |

`hooks` needs no list of hooks: it compares old upstream with main to find what
Touch added to each shared file, then checks the merge result still has it on top
of new upstream. Touch-only files must still exist. Run it after every hand-merge:
dropping a Touch hook while resolving a conflict is the usual way features break.

## Automation

[`.github/workflows/upstream-watch.yml`](../.github/workflows/upstream-watch.yml) runs
daily at 05:23 UTC and on demand:

1. `check`, then `vendor` and push `upstream/vendor` when upstream moved.
2. `merge` into main.
   - **Nothing vendored changed** (upstream only touched assets): nothing else happens.
   - **Clean**: `hooks`, push `upstream-sync`, then compile QuakeC and the engine and
     run the Touch tests on it, and open or refresh a PR (a draft when the build
     fails). The `tests` and `android` workflows run on the branch too.
   - **Conflicts**: open or refresh an issue labelled `upstream-sync` with the files
     and the commands below.
3. It never pushes to `main`. Merging the PR publishes a release, so play a match
   on a touch device and on a public server first.

One-time setup:

- Push the vendor branch: `git push origin upstream/vendor`.
- Settings → Actions → General → **Allow GitHub Actions to create and approve pull requests**.
- Optional: a fine-grained PAT as secret `UPSTREAM_SYNC_TOKEN` (contents, pull requests,
  issues, actions). Pushes made with it trigger the branch workflows themselves;
  without it the workflow dispatches `tests.yml` and `android.yml` on the branch.

## Merging conflicts by hand

In a clean clone of this repo (not a git worktree, see CLAUDE.md):

```bash
git fetch origin upstream/vendor:upstream/vendor
python3 scripts/upstream-sync.py resolve
# fix the conflicted files, keeping every Touch change, then:
git add -A engine && git commit
python3 scripts/upstream-sync.py hooks
```

`resolve` also writes the new `upstream.lock`, and the commit gets the vendor commit
as its second parent. Then build (`scripts/build.sh`), push a branch and open a PR.

Do not regenerate `_mod.inc` / `_mod.qh` with upstream's `qcsrc/tools/genmod.sh`.
Ours are kept by hand: genmod reorders the Touch includes and drops
`client/touch_theme.qh`. The text merge handles upstream's new entries.

## Current base

The first vendor commit (`62d7dc6`) records the tree as the July 2026 sync left it.
That sync took only `qcsrc/` from xonotic-data, so the commit holds `qcsrc/` from
`8bca8a4` and every other xonotic-data file from `729015d` (2026-06-11), matching
what main had. Both were found by comparing blob hashes with main, and so were
`d0_blind_id` (`64983f4`) and the superproject (`85dc7f0`).

A trial merge of upstream as of 2026-11-09 (175 xonotic-data commits plus
superproject, gmqcc and d0_blind_id changes) hit two conflicts, both easy:
`qcsrc/menu/item/button.qc` (our touch slop) and `qcsrc/menu/xonotic/dialog_firstrun.qc`
(our welcome text). With our side kept, `hooks` passed and QuakeC compiled.

The older nested-git path, [`scripts/sync-upstream-fork.sh`](../scripts/sync-upstream-fork.sh),
is superseded by this.

## Touch-only paths (do not drop)

**darkplaces**

- `touch_ui.c`, `touch_ui.h`

**xonotic-data `qcsrc/`**

- `client/touch_*.qc` / `.qh`, `client/touch_api.qh`
- `menu/xonotic/dialog_settings_touch.*`
- `menu/xonotic/dialog_touch_*.`*
- `menu/xonotic/touch_*.`*, `touchbutton.*`
- `dpdefs/ut_touchfinger.qc`
- `common/mutators/mutator/overkill/cl_overkill.*` (Touch CSQC hook)

Do **not** restore obsolete `client/mapvoting.*` / `server/mapvoting.*` — upstream now owns `common/mapvoting/`. Touch code that included `<client/mapvoting.qh>` must use `<common/mapvoting/_mod.qh>` instead (see `client/touch_input.qc`).

## Shared hotspots (re-port after overlay)

**darkplaces:** `vid_sdl.c`, `cl_screen.c`, `screen.h`, `makefile.inc`, `clvm_cmds.c`, `svvm_cmds.c`, `vid_shared.c`, `vid.h`

**qcsrc:** `client/main.qc`, `client/view.qc`, `client/command/cl_cmd.qc`, `client/hud/panel/weapons.qc`, `menu/menu.qc`, `menu/item/inputbox.qc`, `menu/xonotic/dialog_gamemenu.qc`, `leavematchbutton.*`, `dialog_settings.qc`, `mainwindow.qc`, `dialog_firstrun.qc`, `dialog_termsofservice.qc`, `server/main.qc`, `*/_mod.inc` / `*/_mod.qh` include lists
