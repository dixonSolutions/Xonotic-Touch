# Testing

Primary distribution is **Flatpak** (built automatically on each `main` push). For day-to-day testing on a Linux touch tablet:

```bash
./scripts/install-flatpak.sh --from-remote --run
```

Or install bundles from the [continuous release](https://github.com/dixonSolutions/Xonotic-Touch/releases/tag/continuous).

Packages are **slim** (~60 MB): textures, maps, and music download on **first launch** with an in-game progress bar (network required). See [RELEASES.md](RELEASES.md) and [SETUP.md](SETUP.md).

## Local Flatpak build

```bash
./scripts/install-flatpak.sh
./scripts/install-flatpak.sh --run
```

Launch once on a **networked** device so assets download automatically (~3 GB). A **Xonotic Touch** setup dialog shows download progress; profile and touch steps are skipped if already configured.

## First-run setup

See [SETUP.md](SETUP.md) for the full wizard chain, skip rules, OSK on GNOME/Wayland, and touch menu behavior (no on-screen pointer).

Quick checks:

| Check | Expected |
|-------|----------|
| Window title | `Xonotic Touch` |
| Fresh install + Wi‑Fi | Download dialog with progress bar, then profile / touch if needed |
| Returning user (name + `touch_setup_done`) | No profile/touch dialogs |
| Menu text field tap | System keyboard (GNOME: Accessibility → Screen Keyboard) |

## Native Linux (dev only)

For engine work on a desktop without Flatpak — mouse emulates touch when `vid_touchscreen=1`:

```bash
./scripts/compile-and-install-deps.sh
./scripts/fetch-assets-runtime.sh "$HOME/.local/share/xonotic-touch/data"
./scripts/run-local.sh
```

| Script | Purpose |
|--------|---------|
| `./scripts/install-flatpak.sh` | Build or install Flatpak |
| `./scripts/compile-and-install-deps.sh` | Native apt deps + compile |
| `./scripts/run-local.sh` | Run compiled binary on host Linux |

### Troubleshooting

- **Missing textures:** launch on Wi‑Fi and wait for the in-game download dialog to finish, or run `./scripts/fetch-assets-runtime.sh`
- **Touch not responding:** verify `vid_touchscreen 1` in `touch/xonotic.cfg`
- **No OSK on tablet:** enable GNOME Screen Keyboard; focus a menu input box (see [SETUP.md](SETUP.md))

See [MAINTAINING.md](MAINTAINING.md) for maintainer workflow.
