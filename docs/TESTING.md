# Testing

Primary distribution is **Flatpak** (built automatically on each `main` push). For day-to-day testing on a Linux tablet or desktop:

```bash
./scripts/install-flatpak.sh --from-remote --run
```

Or install bundles from the [continuous release](https://github.com/dixonSolutions/Xonotic-Ubuntu-Touch-App/releases/tag/continuous).

Packages are **slim** (~60 MB): textures, maps, and music download on **first launch** (network required). See [RELEASES.md](RELEASES.md).

---

## Optional: Ubuntu Touch (Click)

Click packaging is **not built in CI** but remains available for local/device testing via Clickable.

### Prerequisites

- [Clickable](https://github.com/ubports/clickable) (`./scripts/install-clickable.sh`)
- Device with `adb` or Clickable device target (for `--install`)

### Script entry points

| Script | Purpose |
|--------|---------|
| `./scripts/clickable.sh --container --install` | Build and install click package (shortcut) |
| `./scripts/install-clickable.sh --container` | Clickable CLI + SDK container setup |
| `./scripts/run-clickable.sh --container --install` | Alternative full wrapper |
| `./scripts/compile-and-install-deps.sh` | Native apt deps + compile |
| `./scripts/run-local-no-clickable.sh` | Run on Linux desktop without Clickable |

### Standard Click build

```bash
./scripts/clickable.sh --container --setup --install
```

Package ID: `xonotic-touch.ratrad`

### Playable build

After `clickable install`, launch once on a **networked** device — assets download automatically (~3 GB).

For local desktop testing without Clickable:

```bash
./scripts/compile-and-install-deps.sh
./scripts/fetch-assets-runtime.sh "$HOME/.local/share/xonotic-touch/data"
./scripts/run-local-no-clickable.sh
```

### Troubleshooting

- **Missing textures:** launch on Wi‑Fi so assets download, or run `./scripts/fetch-assets-runtime.sh`
- **Touch not responding:** verify `vid_touchscreen 1` in `touch/xonotic.cfg`

See [MAINTAINING.md](MAINTAINING.md) for maintainer workflow.
