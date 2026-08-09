# Releases and packaging

**Xonotic Touch** ships as:

| Format | Targets | Notes |
|--------|---------|--------|
| **Flatpak** | Linux desktop + tablets (`x86_64`, `aarch64`) | Versioned OSTree remote on GitHub Pages (history kept) |
| **Click** | Ubuntu Touch (`arm64`, `armhf`) | GitHub Pages download remote + versioned GitHub Release assets |

Packages are slim: compiled game logic and touch configs ship in the app; **textures, maps, and music download on first launch**.

All builds and publishing run through [GitHub Actions](.github/workflows/build-and-publish.yml).

Each push to `main` creates a **new immutable version** `1.2.<run_number>` (tag `v1.2.<run_number>`). Older GitHub Releases are **not** deleted. The Flatpak remote **appends** the new commit and keeps prior OSTree history (pruned to the last ~40 commits per ref).

## Flatpak

### Public remote (GitHub Pages)

| | |
|---|---|
| **Remote URL** | `https://dixonSolutions.github.io/Xonotic-Touch/flatpak` |
| **Remote name** | `xonotic-touch` |
| **App ID** | `io.github.dixonSolutions.XonoticTouch` |
| **Architectures** | `x86_64` (desktop/laptop), `aarch64` (ARM tablets/phones) |

```bash
flatpak remote-add --user --if-not-exists xonotic-touch \
  https://dixonSolutions.github.io/Xonotic-Touch/flatpak

flatpak install --user xonotic-touch io.github.dixonSolutions.XonoticTouch
flatpak update --user io.github.dixonSolutions.XonoticTouch
```

List older commits (rollback):

```bash
flatpak remote-info --log xonotic-touch io.github.dixonSolutions.XonoticTouch
# then install/update to a specific commit hash if needed
```

First launch downloads game assets (~3 GB) into:

| Install | Path |
|---------|------|
| **Flatpak** | `~/.var/app/io.github.dixonSolutions.XonoticTouch/data/xonotic-touch/data/` (wiped by Delete app data / `flatpak uninstall --delete-data`) |
| **Click (Ubuntu Touch)** | `~/.local/share/xonotictouch.dixonsolutions/data/` (AppArmor-writable `APP_PKGNAME` dir) |
| **Native / unpackaged** | `~/.local/share/xonotic-touch/data/` |

Legacy Flatpak installs that still have `~/.local/share/xonotic-touch` are migrated into the Flatpak data dir on next launch (the manifest exposes that host path only for migration — not as the live store). To force a clean first-run wizard now:

```bash
flatpak uninstall --user --delete-data io.github.dixonSolutions.XonoticTouch
rm -rf ~/.local/share/xonotic-touch \
  ~/.var/app/io.github.dixonSolutions.XonoticTouch
```

User config and touch layout overrides remain in `~/.xonotic/`.

### Local Flatpak build

```bash
./scripts/install-flatpak.sh
./scripts/install-flatpak.sh --from-remote --run
```

## Ubuntu Touch (.click)

Click packages target Ubuntu Touch devices (ARM). App ID: `xonotictouch.dixonsolutions` (must match the [OpenStore](https://open-store.io/app/xonotictouch.dixonsolutions) listing).

There is no OSTree/apt remote for `.click` like Flatpak — instead each `main` push publishes a **stable download remote** on GitHub Pages (always-latest URLs) plus versioned files and GitHub Release attachments. The `openstore` CI job uploads arm64 + armhf revisions when the `OPENSTORE_API_KEY` repository secret is set (required for that job to pass).

### Public download remote (GitHub Pages)

| | |
|---|---|
| **Index** | `https://dixonSolutions.github.io/Xonotic-Touch/click/` |
| **Always-latest arm64** | `https://dixonSolutions.github.io/Xonotic-Touch/click/latest-arm64.click` |
| **Always-latest armhf** | `https://dixonSolutions.github.io/Xonotic-Touch/click/latest-armhf.click` |
| **Machine-readable** | `https://dixonSolutions.github.io/Xonotic-Touch/click/latest.json` |

```bash
wget https://dixonSolutions.github.io/Xonotic-Touch/click/latest-arm64.click
pkcon install-local --allow-untrusted latest-arm64.click
```

### Install from GitHub Releases

1. Open the [latest release](https://github.com/dixonSolutions/Xonotic-Touch/releases/latest) (or any older `v1.2.*` tag).
2. Download `xonotictouch.dixonsolutions_*_arm64.click` (or `_armhf.click`).
3. On the device:

```bash
pkcon install-local --allow-untrusted xonotictouch.dixonsolutions_*_arm64.click
```

### Local Click build

With [Clickable](https://clickable-ut.dev/) (recommended):

```bash
clickable build --arch arm64
clickable build --arch armhf
```

Standalone:

```bash
./scripts/build-click.sh --arch arm64 --install-deps
./scripts/install-click.sh --arch arm64 --skip-build
```

Metadata lives under `click/` (`manifest.json.in`, desktop hook, AppArmor). Framework: `ubuntu-touch-24.04-1.x`.

## GitHub Releases (automatic)

Each push to `main` publishes a **new** release tag `v1.2.<run_number>` (marked latest). Previous tags stay available.

Requires **Settings → Actions → General → Workflow permissions → Read and write permissions** (otherwise `release` fails with `403 Resource not accessible by integration`).

Attached assets:

- `XonoticTouch-<version>-x86_64.flatpak`
- `XonoticTouch-<version>-aarch64.flatpak`
- `xonotictouch.dixonsolutions_<version>_arm64.click`
- `xonotictouch.dixonsolutions_<version>_armhf.click`

Browse all versions: https://github.com/dixonSolutions/Xonotic-Touch/releases  
Latest: https://github.com/dixonSolutions/Xonotic-Touch/releases/latest

Offline Flatpak install:

```bash
flatpak install --user XonoticTouch-1.2.<N>-x86_64.flatpak
```

> The old `continuous` tag (if still present) is legacy from before versioned releases. Prefer `releases/latest` or a specific `v1.2.*` tag.

## CI overview

| Job | Trigger | Output |
|-----|---------|--------|
| `version` | push to `main` | `1.2.<run_number>` / `v1.2.<run_number>` |
| `flatpak` (`x86_64` on `ubuntu-latest`, `aarch64` on native `ubuntu-24.04-arm`) | push to `main` | Versioned Flatpak bundles |
| `click` (matrix `arm64`, `armhf`) | push to `main` | Versioned `.click` packages |
| `publish-pages` | push to `main` | GitHub Pages: Flatpak OSTree + Click downloads |
| `release` | push to `main` | New GitHub Release tag (old tags kept) |
| `openstore` | push to `main` | OpenStore revision upload (`OPENSTORE_API_KEY` required) |

### OpenStore

<a href="https://open-store.io/app/xonotictouch.dixonsolutions"><img src="https://open-store.io/badges/en_US.svg" alt="OpenStore" width="200" /></a>

| | |
|---|---|
| **App id** | `xonotictouch.dixonsolutions` |
| **Title** | Xonotic Touch |
| **Manage** | https://open-store.io/manage/xonotictouch.dixonsolutions |
| **Public page** | https://open-store.io/app/xonotictouch.dixonsolutions |
| **Secret** | Repo secret `OPENSTORE_API_KEY` |
| **Icon** | Official `engine/misc/logos/icons_png/xonotic_256.png` inside the `.click` (extracted on revision upload) |
| **Changelog** | Leave the general store changelog blank; each CI upload sets a **revision** changelog (`Xonotic Touch <version>: …`) |

After the first successful `openstore` job, open the manage page and set **Published = Yes** once automated review passes.

### GitHub Pages setup

Enable **GitHub Pages** for this repository:

1. Settings → Pages → Build and deployment → **GitHub Actions**

## Asset download sources

On first launch, `fetch-assets-runtime.sh` tries:

1. **Git sparse clone** from GitLab when `git` is available
2. **Xonotic autobuild ZIPs** via `curl`

| Variable | Purpose |
|----------|---------|
| `XONOTIC_SKIP_ASSET_FETCH=1` | Skip download (dev/testing) |
| `XONOTIC_AUTOBUILD_URL` | Autobuild base URL |
| `XONOTIC_TOUCH_DATA_DIR` | Asset cache directory |
