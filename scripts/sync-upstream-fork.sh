#!/bin/bash
# Retired. This merged upstream into nested git repos under engine/ with
# --allow-unrelated-histories, which turned every upstream change into a
# conflict. Upstream now arrives through scripts/upstream-sync.py and the
# upstream-watch workflow; see docs/UPSTREAM_SYNC.md.
echo "sync-upstream-fork.sh is retired: use scripts/upstream-sync.py (docs/UPSTREAM_SYNC.md)." >&2
exit 1
