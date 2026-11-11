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

Upstream Xonotic arrives through `scripts/upstream-sync.py` and the daily
`upstream-watch` workflow, which open a PR from `upstream-sync`. Never copy upstream
files in by hand. See [UPSTREAM_SYNC.md](UPSTREAM_SYNC.md).
The synced upstream commits are in `upstream.lock`.
