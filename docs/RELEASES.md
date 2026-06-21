# Releases and packaging

Xonotic Touch ships as **Flatpak** (Linux desktops and tablets) and **Click** (Ubuntu Touch). Packages are slim: game logic and touch configs are bundled; **textures, maps, and music download on first launch**.

All builds and publishing run through [GitHub Actions](.github/workflows/build-and-publish.yml).

## Flatpak (primary)

### Public remote (GitHub Pages)

After each push to `main`, CI publishes an OSTree repository:

**Remote URL:** `https://dixonSolutions.github.io/Xonotic-Ubuntu-Touch-App/flatpak`

**App ID:** `io.github.dixonSolutions.XonoticTouch`

### Install on Ultramarine / other Linux (x86_64 or aarch64)

```bash
flatpak remote-add --user --if-not-exists xonotic-touch \
  https://dixonSolutions.github.io/Xonotic-Ubuntu-Touch-App/flatpak

flatpak install --user xonotic-touch io.github.dixonSolutions.XonoticTouch

flatpak run io.github.dixonSolutions.XonoticTouch
```

First launch downloads game assets (~3 GB) into:

`$XDG_DATA_HOME/xonotic-touch/data/` (typically `~/.local/share/xonotic-touch/data/`)

User config and touch layout overrides remain in `~/.xonotic/`.

### Offline bundle install

Tagged releases (`v*`) attach per-arch Flatpak bundles:

```bash
flatpak install --user XonoticTouch-x86_64.flatpak
```

## Click (Ubuntu Touch)

CI builds `arm64` Click packages via Clickable. Download from:

- GitHub Releases (on version tags)
- Actions artifacts (on every `main` build)

**Package name:** `xonotic-touch.ratrad`

```bash
clickable install   # after building locally, or adb push + pkcon install
```

Click packages bundle `curl` and `unzip` so confined installs can fetch assets on first launch.

## CI overview

| Job | Output |
|-----|--------|
| `flatpak-x86_64` | `XonoticTouch-x86_64.flatpak` |
| `flatpak-aarch64` | `XonoticTouch-aarch64.flatpak` |
| `click-arm64` | `xonotic-touch.ratrad_*.click` |
| `publish-flatpak-remote` | GitHub Pages OSTree repo |
| `release` (tags `v*`) | GitHub Release with all artifacts |

### Triggering a release

```bash
git tag v1.1.0
git push origin v1.1.0
```

CI builds Flatpak and Click artifacts, updates the public Flatpak remote, and creates a GitHub Release.

### GitHub Pages setup

Enable **GitHub Pages** for this repository:

1. Settings → Pages → Build and deployment → **GitHub Actions**

The workflow deploys the combined Flatpak repository to the `github-pages` environment.

## Asset download sources

On first launch, `fetch-assets-runtime.sh` tries:

1. **Git sparse clone** from GitLab (`xonotic-data.pk3dir`, maps, music, nexcompat) when `git` is available
2. **Xonotic autobuild ZIPs** via `curl` (public autobuild credentials from the [Xonotic wiki](https://gitlab.com/xonotic/xonotic/-/wikis/Autobuilds))

Override with environment variables:

| Variable | Purpose |
|----------|---------|
| `XONOTIC_SKIP_ASSET_FETCH=1` | Skip download (dev/testing) |
| `XONOTIC_AUTOBUILD_URL` | Autobuild base URL |
| `XONOTIC_TOUCH_DATA_DIR` | Asset cache directory |

## Local Flatpak build

```bash
./scripts/install-flatpak.sh              # build and install locally
./scripts/install-flatpak.sh --from-remote --run   # install from CI remote
```

## Local Click build

```bash
./scripts/clickable.sh --container --setup --install
```

No need to run `fetch-sources.sh assets` before building — assets are not bundled anymore.

## Migrating from `xonotic.ratrad`

The rebranded Click package uses a new ID: `xonotic-touch.ratrad`. Remove the old package before installing:

```bash
pkcon remove xonotic.ratrad
```

Game data in `~/.local/share/xonotic-touch/` is shared across Flatpak and Click installs.
