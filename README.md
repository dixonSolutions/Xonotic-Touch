# Xonotic Touch

**Xonotic Touch** is a touch-only build of [Xonotic](https://xonotic.org) for Linux tablets and phones. Virtual sticks, weapon wheels, and layout presets are tuned for two-thumb play in landscape. Ships as a slim **Flatpak** (~60 MB); textures, maps, and music download on first launch (~3 GB). Native C + QuakeC — no Qt shell.

| | |
|---|---|
| **Install** | Flatpak remote + GitHub Releases (auto-built on each `main` push) |
| **Platforms** | Linux x86_64 and aarch64 touch devices (Wayland/X11) |
| **Input** | Touchscreen required — mouse-as-touch only for local dev |

## Install (Flatpak)

```bash
flatpak remote-add --user --if-not-exists xonotic-touch \
  https://dixonSolutions.github.io/Xonotic-Touch/flatpak
flatpak install --user xonotic-touch io.github.dixonSolutions.XonoticTouch
flatpak run io.github.dixonSolutions.XonoticTouch
```

Or download offline bundles from [GitHub Releases](https://github.com/dixonSolutions/Xonotic-Touch/releases) (`continuous` tag, updated on each `main` build).

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
./scripts/run-local.sh
```

| Path | Purpose |
|------|---------|
| `engine/` | Xonotic fork with touch changes integrated in-tree |
| `touch/` | Defaults, screen math, layout/performance presets |
| `packaging/start.sh` | Launcher: sync bundle, fetch assets, run game |
| `flatpak/` | Flatpak manifest, metainfo, desktop entry |
| `scripts/` | Build, staging, runtime asset fetch, local installers |

## Docs

- [docs/RELEASES.md](docs/RELEASES.md) — Flatpak remote, CI, GitHub Releases
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — technical overview
- [docs/MAINTAINING.md](docs/MAINTAINING.md) — source maintainer guide
- [docs/TESTING.md](docs/TESTING.md) — Flatpak and local testing
- [docs/SOURCES.md](docs/SOURCES.md) — UI and controls source map
- [docs/SCREEN.md](docs/SCREEN.md) — landscape screen calculation
- [docs/CONTROLS.md](docs/CONTROLS.md) — touch controls and presets
- [docs/SETUP.md](docs/SETUP.md) — first-run wizard, download progress, OSK, touch menus
