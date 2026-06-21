# Source layout and modification guide

No binaries in this repo. Clone this repo to get the full `engine/` fork (~70 MB tracked source). Large game assets: `./scripts/fetch-sources.sh assets`.

## Upstream repositories

| Tree | Repository | Touch port work (integrated in engine/) |
|------|------------|-------------------------------------------|
| `engine/darkplaces/` | [darkplaces](https://gitlab.com/xonotic/darkplaces) | SDL, GLES, `gettouchfinger`, `DP_UT_TOUCHFINGER` |
| `engine/data/xonotic-data.pk3dir/qcsrc/menu/` | [xonotic-data.pk3dir](https://gitlab.com/xonotic/xonotic-data.pk3dir) | Touch menus, wizard, settings tab |
| `engine/data/xonotic-data.pk3dir/qcsrc/client/` | same | `touch_*.qc` HUD, sticks, multitouch input |
| `engine/data/xonotic-data.pk3dir/qcsrc/common/` | same | Shared input / constants |
| `engine/gmqcc/` | [gmqcc](https://gitlab.com/xonotic/gmqcc) | QuakeC compiler |

## Fetch modes (maintainers)

```bash
./scripts/fetch-sources.sh minimal   # darkplaces only
./scripts/fetch-sources.sh code      # UI + QuakeC + engine (default)
./scripts/fetch-sources.sh assets    # textures/models/sound (playable build)
./scripts/fetch-sources.sh full      # entire game via ./all update (needs engine/.git)
```

## Building

**Maintainers:** do not compile locally unless needed — see [MAINTAINING.md](MAINTAINING.md).

**Testers:** `./scripts/install-flatpak.sh` or `./scripts/run-local.sh` — see [TESTING.md](TESTING.md).

## Port-specific files (this repo)

- `touch/xonotic.cfg` — gameplay / graphics cvars
- `touch/profiles/*.cfg` — control layout, feel, and performance presets ([CONTROLS.md](CONTROLS.md))
- `touch/screen-calc.sh` — landscape resolution and DPI ([SCREEN.md](SCREEN.md))
- `packaging/start.sh` — Flatpak launcher

All QuakeC and engine touch changes are **in `engine/`** — edit there directly.

QuakeC HUD layout should use `vid_width`, `vid_height`, and touch DPI cvars set at launch—not fixed 1920×1080.

## Syncing upstream

```bash
./scripts/sync-upstream-fork.sh --init-git   # once
FORK_DARKPLACES=… FORK_DATA=… ./scripts/sync-upstream-fork.sh
```

See [MAINTAINING.md](MAINTAINING.md).
