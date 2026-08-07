# Xonotic Touch: Technical Architecture

Native C + QuakeC touch port for Linux touch tablets and phones. **Slim Flatpak and Ubuntu Touch click packages** ship compiled logic and touch configs; large game assets download on first launch into `~/.local/share/xonotic-touch/`.

## 1. Roles

| Role | Compiles? | Actions |
|------|-----------|---------|
| Maintainer | Optional | Edit `engine/`, push; CI builds Flatpak + Click on `main` |
| User / tester | No | Install from Flatpak remote, `.click`, or GitHub Releases |

## 2. Core architecture

| Component | Location |
|-----------|----------|
| Engine | `engine/darkplaces/` (`gettouchfinger`, touch input) |
| Menus / HUD / controls | `engine/data/xonotic-data.pk3dir/qcsrc/` |
| Touch defaults | `touch/xonotic.cfg` |
| Touch presets | `touch/profiles/*.cfg` |
| Screen layout | `touch/screen-calc.sh` |
| Launcher | `packaging/start.sh` — sync bundle, background asset fetch, launch |
| Click confinement rules | [UBUNTU_TOUCH_LAUNCH.md](UBUNTU_TOUCH_LAUNCH.md) — bundled busybox, POSIX launch path |
| Runtime assets | `scripts/fetch-assets-runtime.sh`, `scripts/lib/asset-fetch.sh` |
| First-run UX | In-game wizard chain + download progress — [SETUP.md](SETUP.md) |
| Flatpak | `flatpak/io.github.dixonSolutions.XonoticTouch.yml` |
| Click | `click/`, `clickable.yaml`, `scripts/build-click.sh` |

## 3. Repository layout

```
engine/              # Xonotic fork; touch changes integrated in-tree
touch/               # xonotic.cfg, screen-calc.sh, profiles/
packaging/           # start.sh
flatpak/             # Flatpak manifest + metadata
click/               # Ubuntu Touch manifest, desktop, AppArmor
clickable.yaml       # Clickable entrypoint for .click builds
scripts/             # build, stage-slim-data, stage-click, fetch-assets-runtime, installers
.github/workflows/   # Flatpak + Click CI, Pages remote, GitHub Releases
```

## 4. Launch flow

```mermaid
flowchart TD
  A[start.sh] --> B[Sync slim bundle to user data dir]
  B --> C{Assets present?}
  C -->|no| D[Background fetch + progress file]
  C -->|yes| E[Write screen layout + touch startup]
  D --> F[exec xonotic -xonotic]
  E --> F
  F --> G[Menu startup chain: ToS → download UI → profile → touch setup]
```

Asset download runs in parallel with the game when needed; progress is shown in `XonoticTouchAssetFetchDialog`. See [SETUP.md](SETUP.md).

## 5. Packaging

| Format | ID | Architectures | CI |
|--------|-----|---------------|-----|
| Flatpak | `io.github.dixonSolutions.XonoticTouch` | `x86_64`, `aarch64` | Yes — every `main` push |
| Click | `xonotictouch.dixonsolutions` | `arm64`, `armhf` | Yes — every `main` push |

Public Flatpak remote: GitHub Pages OSTree repo with retained commit history. Click + offline Flatpak bundles attach to each versioned GitHub Release (see [RELEASES.md](RELEASES.md)).

## 6. Docs

- [RELEASES.md](RELEASES.md)
- [MAINTAINING.md](MAINTAINING.md)
- [TESTING.md](TESTING.md)
- [SOURCES.md](SOURCES.md)
- [SCREEN.md](SCREEN.md)
- [CONTROLS.md](CONTROLS.md)
- [SETUP.md](SETUP.md) — first-run wizard, asset progress, touch menus, OSK
- [UBUNTU_TOUCH_LAUNCH.md](UBUNTU_TOUCH_LAUNCH.md) — AppArmor click confinement contract
