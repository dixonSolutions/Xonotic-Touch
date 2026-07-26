# Xonotic Touch

**Xonotic Touch** is a touch-only build of [Xonotic](https://xonotic.org) for Linux tablets and phones. Virtual sticks, weapon wheels, and layout presets are tuned for two-thumb play in landscape. Ships as a slim **Flatpak** or Ubuntu Touch **`.click`** (~60 MB); textures, maps, and music download on first launch (~3 GB). Native C + QuakeC — no Qt shell.

| | |
|---|---|
| **Install** | Flatpak remote, [OpenStore](https://open-store.io/app/xonotictouch.dixonsolutions) / Ubuntu Touch `.click`, and versioned GitHub Releases (each `main` push) |
| **Platforms** | Flatpak: Linux `x86_64` + `aarch64` (Wayland/X11). Click: Ubuntu Touch `arm64` + `armhf` |
| **Input** | Touchscreen required — mouse-as-touch only for local dev |

<img width="1954" height="1302" alt="image" src="https://github.com/user-attachments/assets/cf918732-3540-4fd4-a9f9-b7550dc1b6d2" />


In-game: glass MOVE stick and FIRE / JUMP / crouch, top **CONSOLE** pill, and the stock **right-side weapons strip** (tap an icon to switch — no separate WEP button).

## Install (Flatpak)

```bash
flatpak remote-add --user --if-not-exists xonotic-touch \
  https://dixonSolutions.github.io/Xonotic-Touch/flatpak
flatpak install --user xonotic-touch io.github.dixonSolutions.XonoticTouch
flatpak run io.github.dixonSolutions.XonoticTouch
```

Or download offline bundles from [GitHub Releases](https://github.com/dixonSolutions/Xonotic-Touch/releases/latest) (new `v1.2.*` tag per build; older tags kept).

### Ubuntu Touch (.click)

Preferred: install from OpenStore on the device.

<a href="https://open-store.io/app/xonotictouch.dixonsolutions"><img src="https://open-store.io/badges/en_US.svg" alt="OpenStore" width="200" /></a>

Sideload (Pages download remote):

```bash
wget https://dixonSolutions.github.io/Xonotic-Touch/click/latest-arm64.click
pkcon install-local --allow-untrusted latest-arm64.click
```

Or grab a versioned `.click` from [GitHub Releases](https://github.com/dixonSolutions/Xonotic-Touch/releases/latest). Index: https://dixonSolutions.github.io/Xonotic-Touch/click/

Local build with [Clickable](https://clickable-ut.dev/): `clickable build --arch arm64`  
Or: `./scripts/build-click.sh --arch arm64`

First launch downloads game data to `~/.local/share/xonotic-touch/data/`. See [docs/RELEASES.md](docs/RELEASES.md).

## Maintainer workflow

```bash
./scripts/fetch-sources.sh code     # refresh missing compile deps only

# Edit under engine/ — engine, menus, touch CSQC
# engine/darkplaces/
# engine/data/xonotic-data.pk3dir/qcsrc/

# Local Flatpak build and install:
./scripts/install-flatpak.sh

# Ubuntu Touch click (arm64 / armhf):
./scripts/build-click.sh --arch arm64
# or: clickable build --arch arm64

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
| `click/` | Ubuntu Touch click metadata (manifest, desktop, AppArmor) |
| `clickable.yaml` | Clickable build entry for UT `.click` packages |
| `scripts/` | Build, staging, runtime asset fetch, local installers |

## Docs

- [docs/RELEASES.md](docs/RELEASES.md) — Flatpak + Click, CI, GitHub Releases
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — technical overview
- [docs/MAINTAINING.md](docs/MAINTAINING.md) — source maintainer guide
- [docs/TESTING.md](docs/TESTING.md) — Flatpak and local testing
- [docs/SOURCES.md](docs/SOURCES.md) — UI and controls source map
- [docs/SCREEN.md](docs/SCREEN.md) — landscape screen calculation
- [docs/CONTROLS.md](docs/CONTROLS.md) — touch controls and presets
- [docs/SETUP.md](docs/SETUP.md) — first-run wizard, download progress, OSK, touch menus
