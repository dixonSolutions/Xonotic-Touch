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
  A[start.sh] --> B[Sync slim bundle to our user data dir]
  B --> C{Our app assets ready?}
  C -->|yes| E[Normal menu]
  C -->|no| W[Fullscreen setup wizard]
  W --> BG[Background: Flatpak copy or download]
  BG --> R[Relaunch when ready]
  E --> F[exec xonotic]
  W --> F
```

Fast ready-check before launch. Flatpak copy / download run in the **background** and drive a **fullscreen** wizard — the main menu must not show through. Our app owns all user data and packs; Flatpak `org.xonotic.Xonotic` is copy-only. See [SETUP.md](SETUP.md).

The wizard is engine UI, so the package cannot be asset-free: `scripts/fetch-boot-assets.sh` stages the menu skin and console graphics (~31 MB) that the first screen is drawn from, and `scripts/stage-slim-data.sh` calls it after stripping the large media directories. Setup precedes the terms-of-service dialog in the startup chain, because the ToS arrives asynchronously and would otherwise land on top of a download in progress.

Engine-visible handshake files live under `data/touch/` with plain names (`asset-progress.txt`, `relaunch-request.txt`): DarkPlaces' `FS_CheckNastyPath` refuses any path with a leading dot, so the shell-side `.assets-ready` marker reaches the menu as the `_touch_assets_ready` cvar instead. Finished downloads go back through the launcher rather than hot-loading, because packs added after startup are not in the engine's search path.

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
