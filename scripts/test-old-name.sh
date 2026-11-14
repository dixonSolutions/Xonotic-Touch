#!/bin/bash
# The project was renamed to Godnotic. Fail when the earlier name comes back in a
# tracked file or path: a new file in an old package folder, a pasted app ID, an
# environment variable from an old script.
#
# The lines that carry installed players over from the old name are the only
# ones allowed to say it, and they mark themselves with OLD_NAME or
# OLD_FLATPAK_APP_ID.
set -euo pipefail
cd "$(dirname "$0")/.."

a='xonotic'; b='touch'
# "qcsrc/menu/xonotic/touch_*.qc" and "~/.xonotic/touch.layout.cfg" are a folder
# named xonotic followed by a touch file, not the old name.
pattern="${a}[ _-]?${b}"
false_hits="${a}/${b}|\\.${a}/${b}\\."

status=0
hits="$(git grep -n -I -i -E "$pattern" -- . ':!scripts/test-old-name.sh' ':!upstream.lock' \
    | grep -v -i -E "$false_hits" | grep -v -E 'OLD_NAME|OLD_FLATPAK_APP_ID' || true)"
if [ -n "$hits" ]; then
    echo "FAIL: the old project name is back in:" >&2
    echo "$hits" | cut -c1-200 >&2
    status=1
fi
paths="$(git ls-files | grep -i -E "$pattern" || true)"
if [ -n "$paths" ]; then
    echo "FAIL: paths carry the old project name:" >&2
    echo "$paths" >&2
    status=1
fi
[ "$status" = 0 ] && echo "PASS: no old project name in tracked files"
exit "$status"
