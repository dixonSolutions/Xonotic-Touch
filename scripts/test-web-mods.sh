#!/bin/bash
# Lua mods on the web build, the page's half (web/mods.js): see
# tests/web-mods/mods.test.mjs. The engine's half (touch_webfetch.h) is
# exercised in a browser by the web build checks in docs/WEB.md.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
node "$ROOT/tests/web-mods/mods.test.mjs"
