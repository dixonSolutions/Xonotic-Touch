#!/bin/sh
# Checks scripts/pack-phone-archive.py: a phone pack goes into the archive under
# the official name the app installs it as, every entry is stored (byte ranges
# only resume on stored entries), and nothing else is added.
#
#   scripts/test-phone-archive.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
PY=${PYTHON:-python3}

printf 'pk3' > "$tmp/xonotic-20261107-maps-phone.pk3"
printf 'pk3' > "$tmp/xonotic-20261107-data-phone.pk3"

"$PY" -I "$here/pack-phone-archive.py" -o "$tmp/out.zip" \
    "$tmp/xonotic-20261107-maps-phone.pk3" "$tmp/xonotic-20261107-data-phone.pk3" 2>"$tmp/log" \
    || { cat "$tmp/log"; exit 1; }

"$PY" -I - "$tmp/out.zip" <<'EOF'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
assert z.testzip() is None
names = sorted(z.namelist())
assert names == ["Xonotic/data/xonotic-20261107-data.pk3",
                 "Xonotic/data/xonotic-20261107-maps.pk3"], names
for i in z.infolist():
    assert i.compress_type == 0, (i.filename, i.compress_type)
print("ok: archive names and stored entries")
EOF
