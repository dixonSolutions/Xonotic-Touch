# Xonotic Touch

**Xonotic Touch** is a touch-first build of [Xonotic](https://xonotic.org) for Linux tablets, phones, and touch PCs. It ships as a slim **Flatpak** (~60 MB): textures, maps, and music download on first launch (~3 GB). Native C + QuakeC — no Qt shell.

| | |
|---|---|
| **Install** | Flatpak remote + GitHub Releases (auto-built on every `main` push) |
| **Platforms** | Linux x86_64 and aarch64 (Wayland/X11, touch or mouse-as-touch) |
| **Also supported locally** | Ubuntu Touch Click package (`scripts/clickable.sh`) — not built in CI yet |

## Install (Flatpak)

```bash
flatpak remote-add --user --if-not-exists xonotic-touch \
  https://dixonSolutions.github.io/Xonotic-Ubuntu-Touch-App/flatpak
flatpak install --user xonotic-touch io.github.dixonSolutions.XonoticTouch
flatpak run io.github.dixonSolutions.XonoticTouch
```

Or download offline bundles from [GitHub Releases](https://github.com/dixonSolutions/Xonotic-Ubuntu-Touch-App/releases) (`continuous` tag, updated on each `main` build).

First launch downloads game data to `~/.local/share/xonotic-touch/data/`. See [docs/RELEASES.md](docs/RELEASES.md).

## Maintainer workflow

```bash
./scripts/fetch-sources.sh code     # refresh missing compile deps only

# Edit under engine/ — engine, menus, touch CSQC
# engine/darkplaces/
# engine/data/xonotic-data.pk3dir/qcsrc/

# Local Flatpak build and install:
./scripts/install-flatpak.sh

# Optional native run (assets download on launch, like packages):
./scripts/compile-and-install-deps.sh
./scripts/run-local-no-clickable.sh
```

| Path | Purpose |
|------|---------|
| `engine/` | Xonotic fork with touch changes integrated in-tree |
| `touch/` | Defaults, screen math, layout/performance presets |
| `packaging/start.sh` | Launcher: sync bundle, fetch assets, run game |
| `flatpak/` | Flatpak manifest, metainfo, desktop entry |
| `scripts/` | Build, staging, runtime asset fetch, local installers |
| `clickable.json` | Ubuntu Touch Click packaging (local/CI optional) |

## Docs

- [docs/RELEASES.md](docs/RELEASES.md) — Flatpak remote, CI, GitHub Releases
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — technical overview
- [docs/MAINTAINING.md](docs/MAINTAINING.md) — source maintainer guide
- [docs/TESTING.md](docs/TESTING.md) — local Click build (optional)
- [docs/SOURCES.md](docs/SOURCES.md) — UI and controls source map
- [docs/SCREEN.md](docs/SCREEN.md) — landscape screen calculation
- [docs/CONTROLS.md](docs/CONTROLS.md) — touch controls and presets
