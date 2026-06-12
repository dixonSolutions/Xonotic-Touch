# Testing with Clickable

For **Ubuntu Touch / Clickable community testers** building and installing on device.

## Prerequisites

- [Clickable](https://github.com/ubports/clickable) installed (`./scripts/install-clickable.sh`)
- Device with `adb` or Clickable device target (for `--install`)
- Enough disk for SDK container + source (several GB) when using container mode

## Script entry points

| Script | Purpose |
|--------|---------|
| `./scripts/compile-and-install-deps.sh` | Native apt deps + compile to `build/bin/xonotic` |
| `./scripts/run-local-no-clickable.sh` | Run on Linux desktop without Clickable (auto-build/deps if needed) |
| `./scripts/install-clickable.sh --desktop` | Clickable CLI + host toolchain (`--container-mode` builds) |
| `./scripts/install-clickable.sh --container` | Clickable CLI + Podman/Docker SDK images |
| `./scripts/run-clickable.sh --desktop` | Build on host, run Clickable desktop sim |
| `./scripts/run-clickable.sh --container` | Build in SDK container, run desktop sim |
| `./scripts/run-clickable.sh --container --install` | Build in container and install on device |
| `./scripts/install-clickable.sh --clean-container` | Prune Podman/buildah after SDK builds |

## Standard build (UI / control changes)

Uses `code` fetch in prebuild (darkplaces, gmqcc, game QuakeC). Touch changes are already integrated in `engine/`. Compiles inside the SDK — **not** on the maintainer's machine.

```bash
git clone <maintainer-repo-url>
cd Xonotic-Ubuntu-Touch-App
./scripts/install-clickable.sh --container
./scripts/run-clickable.sh --container --install
```

Or manually:

```bash
clickable build --arch arm64
clickable install
```

## Native Linux desktop (no Clickable container)

```bash
./scripts/run-local-no-clickable.sh --build
```

Or compile once, then run:

```bash
./scripts/compile-and-install-deps.sh
./scripts/run-local-no-clickable.sh
```

## Playable build (maps, music, full data)

Source is in git; **binary assets are not**. Before a playable in-game test:

```bash
./scripts/fetch-sources.sh assets
clickable build --arch arm64
clickable install
```

`assets` downloads textures/models/sound (~3 GB). For the complete upstream `./all update` workflow, use `full` (requires `engine/.git`, not vendored source).

## Maintainer forks

If the maintainer uses forked upstream repos, set URLs before syncing or fetching:

```bash
export DARKPLACES_URL=https://gitlab.com/<user>/darkplaces.git
export DATA_URL=https://gitlab.com/<user>/xonotic-data.pk3dir.git
./scripts/fetch-sources.sh code
clickable build --arch arm64
```

## What gets installed

- `bin/xonotic` — compiled in SDK during `clickable build`
- `bin/start.sh` — launcher
- `data/` — from `engine/data` when present + `touch/xonotic.cfg` + `data/touch/profiles/`

## Troubleshooting

- **Missing textures / pink checkerboard:** run `./scripts/fetch-sources.sh assets` before build.
- **Build fails on QuakeC:** ensure `engine/data/xonotic-data.pk3dir/qcsrc/client/touch_*.qc` exists (integrated in repo).
- **Touch not responding:** verify `vid_touchscreen 1` in `touch/xonotic.cfg` and device has multitouch.

See [MAINTAINING.md](MAINTAINING.md) for maintainer workflow.
