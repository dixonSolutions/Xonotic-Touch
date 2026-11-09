#!/bin/bash
# Stage game data for the web build: build/web/data/ + manifest.json.
#
#   scripts/stage-web-data.sh --stock <dir-with-stock-pk3s>
#   scripts/stage-web-data.sh --zip <Xonotic-latest.zip>
#
# What goes in, and in which download group:
#   core   this fork's game code and touch assets, packed into zz-*.pk3 so
#          they override the stock files (later pk3 names win), plus the
#          loose configs (xonotic.cfg, touch profiles); and the stock data,
#          maps, xoncompat and font pk3s
#   music  the stock music pk3 (110 MB), optional in the page
# Left out: nexcompat (old Nexuiz map textures, 125 MB; only servers running
# Nexuiz maps need it, and the browser build cannot join servers yet).
#
# Files over 40 MB (XT_WEB_PART_MB) are split into parts for static hosts'
# per-file limits. Each manifest entry carries a content hash; the page
# re-downloads a file only when its hash changes.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
OUT="${XT_WEB_OUT:-$ROOT/build/web}/data"
STOCK=""
ZIP=""

usage() { sed -n '2,18p' "$0" >&2; exit 2; }
while [ $# -gt 0 ]; do
    case "$1" in
        --stock) STOCK="${2:?}"; shift 2 ;;
        --zip) ZIP="${2:?}"; shift 2 ;;
        --out) OUT="${2:?}/data"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "stage-web-data: unknown argument: $1" >&2; usage ;;
    esac
done

work="$(mktemp -d "${TMPDIR:-/tmp}/xt-web-stage.XXXXXX")"
trap 'rm -rf "$work"' EXIT

if [ -n "$ZIP" ]; then
    echo "Extracting stock pk3s from $ZIP…"
    mkdir -p "$work/stock"
    unzip -q -j -o "$ZIP" 'Xonotic/data/*.pk3' -d "$work/stock"
    STOCK="$work/stock"
fi
[ -n "$STOCK" ] || { echo "stage-web-data: pass --stock DIR or --zip FILE" >&2; usage; }
[ -d "$STOCK" ] || { echo "stage-web-data: $STOCK is not a directory" >&2; exit 1; }
ls "$STOCK"/xonotic-*-data.pk3 >/dev/null 2>&1 || { echo "stage-web-data: no xonotic-*-data.pk3 in $STOCK" >&2; exit 1; }

# The fork's own data, exactly as the Android and Flatpak packages ship it.
# The stock data pk3 has the menu skin, so the boot-asset fetch is not needed.
XONOTIC_SKIP_BOOT_ASSETS=1 bash "$ROOT/scripts/stage-slim-data.sh" "$work/slim" >/dev/null

rm -rf "$OUT"
mkdir -p "$OUT"

python3 -I - "$work/slim" "$STOCK" "$OUT" <<'PY'
import hashlib, json, os, sys, zipfile

slim, stock, out = sys.argv[1:4]
files = []

def rev(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()[:16]

PART = int(os.environ.get('XT_WEB_PART_MB', '40')) * 1024 * 1024

def add(name, group):
    """Record a file; split anything over PART into name.partNN pieces.

    Static hosts cap single files (GitHub Pages and Vercel at 100 MB), and
    smaller pieces resume better on a flaky phone connection. The page joins
    the pieces back into one file as it downloads. Piece names carry the
    content hash, so hosts can cache them forever."""
    p = os.path.join(out, name)
    size, r = os.path.getsize(p), rev(p)
    entry = {'name': os.path.basename(name), 'path': 'data/' + name, 'size': size, 'rev': r, 'group': group}
    if size > PART:
        parts = []
        with open(p, 'rb') as src:
            i = 0
            while True:
                block = src.read(PART)
                if not block:
                    break
                part = f'{name}.{r}.part{i:02d}'
                with open(os.path.join(out, part), 'wb') as dst:
                    dst.write(block)
                parts.append({'url': part, 'size': len(block)})
                i += 1
        os.remove(p)
        entry['parts'] = parts
    else:
        entry['url'] = name
    files.append(entry)

# The fork's pk3dirs, packed. "zz-" sorts after every stock pk3, so these
# win wherever both have a file, as the pk3dir does in a native install.
for entry in sorted(os.listdir(slim)):
    src = os.path.join(slim, entry)
    if entry.endswith('.pk3dir') and os.path.isdir(src):
        name = 'zz-' + entry[:-len('.pk3dir')] + '.pk3'
        with zipfile.ZipFile(os.path.join(out, name), 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            for d, dirs, fs in os.walk(src):
                dirs.sort()
                for f in sorted(fs):
                    full = os.path.join(d, f)
                    arc = os.path.relpath(full, src)
                    if arc.startswith('.') or '/.' in arc:
                        continue
                    # Already-compressed formats gain nothing from deflate.
                    kind = zipfile.ZIP_STORED if f.lower().endswith(('.ogg', '.jpg', '.png', '.pk3', '.dds')) else zipfile.ZIP_DEFLATED
                    z.write(full, arc, compress_type=kind)
        add(name, 'core')

# Loose files at the top of the data dir (xonotic.cfg, touch/ profiles).
for d, dirs, fs in os.walk(slim):
    dirs[:] = sorted(x for x in dirs if not x.endswith('.pk3dir'))
    for f in sorted(fs):
        full = os.path.join(d, f)
        rel = os.path.relpath(full, slim)
        if rel.startswith('.'):
            continue
        dst = os.path.join(out, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(full, 'rb') as a, open(dst, 'wb') as b:
            b.write(a.read())
        add(rel, 'core')

# Stock pk3s.
for f in sorted(os.listdir(stock)):
    if not f.endswith('.pk3') or 'nexcompat' in f:
        continue
    dst = os.path.join(out, f)
    try:
        os.link(os.path.join(stock, f), dst)  # add() replaces it with parts
    except OSError:
        import shutil
        shutil.copyfile(os.path.join(stock, f), dst)
    add(f, 'music' if f.endswith('-music.pk3') else 'core')

manifest = {'format': 2, 'files': files}
with open(os.path.join(out, 'manifest.json'), 'w') as m:
    json.dump(manifest, m, indent=1)
core = sum(f['size'] for f in files if f['group'] == 'core')
music = sum(f['size'] for f in files if f['group'] == 'music')
print(f"Staged {len(files)} files: core {core/1e6:.0f} MB, music {music/1e6:.0f} MB -> {out}")
PY
