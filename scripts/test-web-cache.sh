#!/bin/bash
# Web loader tests: the data store and loader flow (node, no browser), and
# the reproducibility of the staged data.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
node "$ROOT/tests/web-cache/cache.test.mjs"
bash "$ROOT/scripts/test-web-cache-repro.sh"
