# Releases and packaging

**Xonotic Touch** is distributed primarily as **Flatpak** for Linux (desktops and tablets). Packages are slim: compiled game logic and touch configs ship in the app; **textures, maps, and music download on first launch**.

Ubuntu Touch **Click** packaging remains in the repo (`clickable.json`, `scripts/clickable.sh`) for local builds but is **not** produced by CI today.

All Flatpak builds and publishing run through [GitHub Actions](.github/workflows/build-and-publish.yml).

## Flatpak

### Public remote (GitHub Pages)

After each push to `main`, CI publishes an OSTree repository:

| | |
|---|---|
| **Remote URL** | `https://dixonSolutions.github.io/Xonotic-Ubuntu-Touch-App/flatpak` |
| **Remote name** | `xonotic-touch` |
| **App ID** | `io.github.dixonSolutions.XonoticTouch` |

```bash
flatpak remote-add --user --if-not-exists xonotic-touch \
  https://dixonSolutions.github.io/Xonotic-Ubuntu-Touch-App/flatpak

flatpak install --user xonotic-touch io.github.dixonSolutions.XonoticTouch

flatpak run io.github.dixonSolutions.XonoticTouch
```

First launch downloads game assets (~3 GB) into:

`$XDG_DATA_HOME/xonotic-touch/data/` (typically `~/.local/share/xonotic-touch/data/`)

User config and touch layout overrides remain in `~/.xonotic/`.

### GitHub Releases (automatic)

Each push to `main` creates or updates the **`continuous`** release with:

- `XonoticTouch-x86_64.flatpak`
- `XonoticTouch-aarch64.flatpak`

Download from: https://github.com/dixonSolutions/Xonotic-Ubuntu-Touch-App/releases/tag/continuous

Offline install:

```bash
flatpak install --user XonoticTouch-x86_64.flatpak
```

## CI overview

| Job | Trigger | Output |
|-----|---------|--------|
| `flatpak` (matrix) | push to `main` | Per-arch Flatpak bundles |
| `publish-flatpak-remote` | push to `main` | GitHub Pages OSTree repo |
| `release` | push to `main` | GitHub Release `continuous` |

### GitHub Pages setup

Enable **GitHub Pages** for this repository:

1. Settings → Pages → Build and deployment → **GitHub Actions**

The workflow deploys the combined Flatpak repository to the `github-pages` environment.

## Asset download sources

On first launch, `fetch-assets-runtime.sh` tries:

1. **Git sparse clone** from GitLab (`xonotic-data.pk3dir`, maps, music, nexcompat) when `git` is available
2. **Xonotic autobuild ZIPs** via `curl` (public autobuild credentials from the [Xonotic wiki](https://gitlab.com/xonotic/xonotic/-/wikis/Autobuilds))

| Variable | Purpose |
|----------|---------|
| `XONOTIC_SKIP_ASSET_FETCH=1` | Skip download (dev/testing) |
| `XONOTIC_AUTOBUILD_URL` | Autobuild base URL |
| `XONOTIC_TOUCH_DATA_DIR` | Asset cache directory |

## Local Flatpak build

```bash
./scripts/install-flatpak.sh
./scripts/install-flatpak.sh --from-remote --run
```

## Local Click build (optional)

Not built in CI. For Ubuntu Touch devices:

```bash
./scripts/clickable.sh --container --setup --install
```

Package ID: `xonotic-touch.ratrad`. See [TESTING.md](TESTING.md).
