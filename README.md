# Xonotic for Ubuntu Touch

Source-first port: no Qt/QML shell, no binaries in git. **Full Xonotic source under `engine/` is committed** with Ubuntu Touch touch controls, menus, and engine changes integrated directly in-tree (not separate overlays). Build outputs and ~3 GB textures/models are not in git.

## Maintainer workflow (you)

```bash
# Fresh clone of this repo already includes integrated engine/ source.
# To re-fetch missing deps or binary assets:
./scripts/fetch-sources.sh code     # source only (~70 MB in git)
./scripts/fetch-sources.sh assets   # textures/models/sound for playable builds

# Edit game UI, controls, engine directly under engine/
# engine/darkplaces/                          — C engine (incl. gettouchfinger)
# engine/data/xonotic-data.pk3dir/qcsrc/      — menus, HUD, touch CSQC

# Pull latest upstream Xonotic into your fork sub-repos:
./scripts/sync-upstream-fork.sh --init-git   # once, if engine/ has no nested .git
FORK_DARKPLACES=https://gitlab.com/you/darkplaces.git \
FORK_DATA=https://gitlab.com/you/xonotic-data.pk3dir.git \
  ./scripts/sync-upstream-fork.sh

# Commit port + engine source in this repo (strip nested .git once):
./scripts/prepare-engine-for-git.sh --yes
git add engine/ && git commit

# Do NOT run compile scripts locally unless you choose to — use:

```bash
./scripts/compile-and-install-deps.sh      # native build
./scripts/run-local-no-clickable.sh        # native run
./scripts/run-clickable.sh --container     # Clickable SDK build
```

# Test landscape screen math without compiling:
./scripts/test-screen-calc.sh
./scripts/test-touch-profiles.sh
```

| Path | Purpose |
|------|---------|
| `engine/` | Full Xonotic fork with UT touch changes integrated |
| `touch/xonotic.cfg` | Gameplay / graphics defaults (tracked) |
| `touch/profiles/` | Touch layout / feel / performance presets (tracked) |
| `touch/screen-calc.sh` | Landscape width/height + DPI layer (tracked) |
| `packaging/` | Click launcher for testers |
| `build/` | **Never on your machine** — Clickable SDK output only |

Reclaim disk after accidental builds: `./scripts/clean-local-artifacts.sh`  
Reclaim Podman layers after Clickable: `./scripts/install-clickable.sh --clean-container`

## Clickable testers

See [docs/TESTING.md](docs/TESTING.md).

```bash
clickable build --arch arm64
clickable install
```

For a **playable** test (maps/music), testers run `./scripts/fetch-sources.sh assets` (or `full`) before `clickable build`.

## Docs

- [docs/TESTING.md](docs/TESTING.md) — for Clickable community
- [docs/MAINTAINING.md](docs/MAINTAINING.md) — source-only maintainer guide
- [docs/SOURCES.md](docs/SOURCES.md) — where to edit UI and controls
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — technical overview
- [docs/SCREEN.md](docs/SCREEN.md) — landscape screen calculation layer
- [docs/CONTROLS.md](docs/CONTROLS.md) — touch controls, cvar schema, presets
