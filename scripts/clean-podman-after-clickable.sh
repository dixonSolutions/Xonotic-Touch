#!/bin/bash
# Deprecated — use: ./scripts/install-clickable.sh --clean-container
exec bash "$(cd "$(dirname "$0")" && pwd)/install-clickable.sh" --clean-container "$@"
