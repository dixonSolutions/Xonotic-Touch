# engine/ directory

Xonotic upstream source lives here. **Source is committed** in this port repo; build outputs and large binary assets are gitignored.

| Subtree | Contents |
|---------|----------|
| `darkplaces/` | C engine |
| `data/xonotic-data.pk3dir/qcsrc/` | QuakeC menus, HUD, client |
| `gmqcc/` | QuakeC compiler sources |
| `d0_blind_id/` | Crypto library |
| `misc/` | Bundled build dependencies (zlib, etc.) |

## First-time git setup (maintainers)

After `./scripts/fetch-sources.sh code`:

```bash
./scripts/clean-local-artifacts.sh          # drop any compile outputs
./scripts/prepare-engine-for-git.sh --yes   # remove nested engine/**/.git
git add engine/
```

Tracked size is ~60–80 MB (source + fonts). Textures/models/sound (~3 GB) stay local only — fetch with `./scripts/fetch-sources.sh assets` or `full`.

## Updates from upstream

See [UPSTREAM_SYNC.md](UPSTREAM_SYNC.md) for the overlay workflow and last synced SHAs.

```bash
./scripts/sync-upstream-fork.sh --init-git --allow-unrelated   # optional nested-git path
FORK_DARKPLACES=… FORK_DATA=… ./scripts/sync-upstream-fork.sh
./scripts/prepare-engine-for-git.sh --yes    # before monorepo commit
```

See [MAINTAINING.md](MAINTAINING.md) and [SOURCES.md](SOURCES.md).
