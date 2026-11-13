#!/bin/bash
# The web data is reproducible: staging this fork's files twice, from clean
# checkouts-worth of files with different timestamps, gives byte-identical
# pk3s and the same manifest id; the game code lands in zz-progs.pk3 alone.
# Stock pk3s are copied as they are, so they need no check here.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/xt-web-repro.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

stage() {
    local n="$1"
    XONOTIC_SKIP_BOOT_ASSETS=1 bash "$ROOT/scripts/stage-slim-data.sh" "$tmp/slim$n" >/dev/null
    # Stand-ins for the compiled game code (scripts/build-web.sh makes it).
    for f in progs.dat csprogs.dat menu.dat; do
        printf 'qc %s\n' "$f" > "$tmp/slim$n/xonotic-data.pk3dir/$f"
    done
    # A different checkout: other mtimes and modes on every file.
    find "$tmp/slim$n" -type f -exec touch -d "@$((1700000000 + n * 86400))" {} +
    [ "$n" = 2 ] && find "$tmp/slim$n" -type f -exec chmod 600 {} +
    bash "$ROOT/scripts/stage-web-data.sh" --own-only --slim "$tmp/slim$n" --out "$tmp/out$n" >/dev/null
}
stage 1
stage 2

m1="$(cat "$tmp/out1/data/web-data.env")"
m2="$(cat "$tmp/out2/data/web-data.env")"
[ "$m1" = "$m2" ] || { echo "FAIL: manifests differ: $m1 vs $m2" >&2; exit 1; }
diff -r "$tmp/out1/data/blobs" "$tmp/out2/data/blobs" >/dev/null || { echo "FAIL: blobs differ" >&2; exit 1; }

python3 -I - "$tmp/out1/data" <<'PY'
import json, os, sys, zipfile
d = sys.argv[1]
m = json.load(open(os.path.join(d, 'manifest.json')))
files = {f['name']: f for f in m['files']}
assert 'zz-progs.pk3' in files, 'zz-progs.pk3 staged'
def names(f):
    blob = os.path.join(d, files[f]['url'])
    return zipfile.ZipFile(blob).namelist()
assert sorted(names('zz-progs.pk3')) == ['csprogs.dat', 'menu.dat', 'progs.dat'], names('zz-progs.pk3')
assert not [n for n in names('zz-xonotic-data.pk3') if n.endswith('.dat') and '/' not in n], 'no progs in zz-xonotic-data.pk3'
for z in ('zz-progs.pk3', 'zz-xonotic-data.pk3'):
    for i in zipfile.ZipFile(os.path.join(d, files[z]['url'])).infolist():
        assert i.date_time == (1980, 1, 1, 0, 0, 0), (z, i.filename, i.date_time)
        assert i.external_attr >> 16 == 0o100644, (z, i.filename, oct(i.external_attr >> 16))
print(f"web data reproducible: {m['manifest']}, zz-progs.pk3 {files['zz-progs.pk3']['size']} bytes")
PY
