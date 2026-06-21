# Xonotic Touch

Touch-first port of [Xonotic](https://xonotic.org) for Linux phones, tablets, and touch PCs. Native C + QuakeC — no Qt shell. **Slim packages**: textures, maps, and music download on first launch (~3 GB).

Targets:

- **Flatpak** — Linux desktops and tablets (e.g. Ultramarine on Surface)
- **Click** — Ubuntu Touch (`arm64`)

## Install (Flatpak)

```bash
flatpak remote-add --user --if-not-exists xonotic-touch \
  https://dixonSolutions.github.io/Xonotic-Ubuntu-Touch-App/flatpak
flatpak install --user xonotic-touch io.github.dixonSolutions.XonoticTouch
flatpak run io.github.dixonSolutions.XonoticTouch
```

See [docs/RELEASES.md](docs/RELEASES.md) for Click packages, GitHub Releases, and CI details.

## Maintainer workflow

```bash
# Engine source is vendored under engine/ (UT touch changes integrated in-tree).
./scripts/fetch-sources.sh code     # refresh missing compile deps only

# Edit game UI, controls, engine directly under engine/
# engine/darkplaces/                          — C engine (gettouchfinger, GLES)
# engine/data/xonotic-data.pk3dir/qcsrc/      — menus, HUD, touch CSQC

# Optional local native run (downloads assets on launch like packages):
./scripts/compile-and-install-deps.sh
./scripts/run-local-no-clickable.sh

# Clickable SDK build (slim package):
./scripts/clickable.sh --container --install
```

| Path | Purpose |
|------|---------|
| `engine/` | Full Xonotic fork with touch changes integrated |
| `touch/xonotic.cfg` | Gameplay / graphics defaults |
| `touch/profiles/` | Touch layout / feel / performance presets |
| `touch/screen-calc.sh` | Landscape width/height + DPI |
| `packaging/start.sh` | Launcher: sync bundle, fetch assets, run game |
| `flatpak/` | Flatpak manifest, metainfo, desktop entry |
| `scripts/` | Build, staging, runtime asset fetch |
| `build/` | Local / CI build output (gitignored) |

Large binary assets are **not** in git and **not** in release packages. They land in `~/.local/share/xonotic-touch/data/` on first launch.

## Ubuntu Touch testers

```bash
clickable build --arch arm64
clickable install
```

Package: `xonotic-touch.ratrad`. See [docs/TESTING.md](docs/TESTING.md).

## Docs

- [docs/RELEASES.md](docs/RELEASES.md) — Flatpak remote, Click, GitHub Actions, releases
- [docs/TESTING.md](docs/TESTING.md) — Clickable community testing
- [docs/MAINTAINING.md](docs/MAINTAINING.md) — source maintainer guide
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — technical overview
- [docs/SOURCES.md](docs/SOURCES.md) — where to edit UI and controls
- [docs/SCREEN.md](docs/SCREEN.md) — landscape screen calculation
- [docs/CONTROLS.md](docs/CONTROLS.md) — touch controls and presets
