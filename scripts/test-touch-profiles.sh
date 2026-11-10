#!/bin/sh
# Validate touch/profiles/*.cfg (no compile). See docs/CONTROLS.md
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROFILES="$ROOT/touch/profiles"
REQUIRED="standard casual competitive left minimal battery balanced quality"
FAIL=0

for name in $REQUIRED; do
    f="$PROFILES/${name}.cfg"
    if [ ! -f "$f" ]; then
        echo "missing profile: $f" >&2
        FAIL=1
        continue
    fi
    if [ "$name" = "battery" ] || [ "$name" = "balanced" ] || [ "$name" = "quality" ]; then
        grep -q 'touch_performance_profile' "$f" || {
            echo "$f: expected touch_performance_profile" >&2
            FAIL=1
        }
    else
        grep -q 'touch_preset' "$f" || {
            echo "$f: expected touch_preset" >&2
            FAIL=1
        }
    fi
done

# touch_defaults.cfg is generated from the table in touch_init.qc: fail when it is stale.
python3 "$(dirname "$0")/../touch/gen-defaults.py" --check || FAIL=1

if [ "$FAIL" -ne 0 ]; then
    exit 1
fi

echo "touch profiles OK ($PROFILES)"
wc -l "$PROFILES"/*.cfg | tail -1
