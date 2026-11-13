#!/bin/bash
# Screenshots of fixture screens at 1280x800, 1920x1080 and 420x900, light and dark.
#   scripts/shots-all.sh <outdir> [fixture...]
set -uo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$1"; shift
mkdir -p "$OUT"
FX=("$@")
[ ${#FX[@]} -gt 0 ] || FX=($(cd "$HERE/../tests/uibridge/fixtures" && ls *.json | sed 's/\.json$//'))
for f in "${FX[@]}"; do
  for size in 1280x800 1920x1080 420x900; do
    for scheme in light dark; do
      timeout 60 "$HERE/scripts/shot.sh" "$f" "$size" "$scheme" "$OUT/$f-$size-$scheme.png" >/dev/null 2>&1 || echo "FAILED $f $size $scheme"
    done
  done
done
ls "$OUT" | wc -l
