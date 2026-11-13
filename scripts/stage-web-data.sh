#!/bin/bash
# Stage game data for the web build: build/web/data/, content-addressed.
#
#   scripts/stage-web-data.sh --stock <dir-with-stock-pk3s>
#   scripts/stage-web-data.sh --zip <Xonotic-latest.zip>
#   scripts/stage-web-data.sh --shell <stock-data-url>
#   scripts/stage-web-data.sh --own-only        this fork's files only (tests)
#   --out DIR    write DIR/data instead of build/web/data
#   --slim DIR   take an already staged slim data dir (tests)
#
# --shell stages only this fork's own files (about 15 MB) and points the page
# at another deployment's data/ for the stock pk3s, which the page reads at
# run time from that host's stock.json. GitHub Pages uses it: the stock data
# does not fit there.
#
# What goes in, and in which download group:
#   core   this fork's files, packed into zz-*.pk3 so they override the stock
#          files (later pk3 names win), plus the loose configs (xonotic.cfg,
#          touch profiles); and the stock data, maps, xoncompat and font pk3s
#   music  the stock music pk3 (110 MB), optional in the page
# Left out: nexcompat (old Nexuiz map textures, 125 MB; only servers running
# Nexuiz maps need it, and the browser build cannot join servers yet).
#
# The compiled game code (progs.dat, csprogs.dat, menu.dat) goes into its own
# small zz-progs.pk3: it carries a per-commit watermark, so a code push
# changes that one file and nothing else. Every zip is reproducible: fixed
# timestamps (SOURCE_DATE_EPOCH, else 1980-01-01), attributes, entry order
# and compression level, so the same sources give the same bytes on every
# run and every host with the same zlib.
#
# Layout written (docs/WEB.md, "Game data"):
#   data/blobs/<sha256>.bin  every file cut into 32 MiB chunks, each named by
#                            its own hash: immutable, cacheable forever
#   data/m/<id>.json         the manifest (files, chunk hashes), named by the
#                            hash of its own text: immutable
#   data/stock.json          no-cache pointer to a manifest of the stock files
#                            alone, for shells on other hosts
#   data/manifest.json       no-cache: names the current manifest, and lists
#                            the files in the old format (url / parts) for
#                            pages loaded before this layout
#   data/web-data.env        XT_DATA_MANIFEST=m/<id>.json, for the page
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
OUT="${XT_WEB_OUT:-$ROOT/build/web}/data"
STOCK=""
ZIP=""
SHELL_STOCK=""
OWN_ONLY=0
SLIM=""

usage() { sed -n '2,40p' "$0" >&2; exit 2; }
while [ $# -gt 0 ]; do
    case "$1" in
        --stock) STOCK="${2:?}"; shift 2 ;;
        --zip) ZIP="${2:?}"; shift 2 ;;
        --shell) SHELL_STOCK="${2:?}"; shift 2 ;;
        --own-only) OWN_ONLY=1; shift ;;
        --slim) SLIM="${2:?}"; shift 2 ;;
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
if [ -n "$SHELL_STOCK" ] || [ "$OWN_ONLY" = "1" ]; then
    STOCK="$work/none"
    mkdir -p "$STOCK"
else
    [ -n "$STOCK" ] || { echo "stage-web-data: pass --stock DIR, --zip FILE or --shell URL" >&2; usage; }
    [ -d "$STOCK" ] || { echo "stage-web-data: $STOCK is not a directory" >&2; exit 1; }
    ls "$STOCK"/xonotic-*-data.pk3 >/dev/null 2>&1 || { echo "stage-web-data: no xonotic-*-data.pk3 in $STOCK" >&2; exit 1; }
fi

# The fork's own data, exactly as the Android and Flatpak packages ship it.
# The stock data pk3 has the menu skin, so the boot-asset fetch is not needed.
if [ -z "$SLIM" ]; then
    SLIM="$work/slim"
    XONOTIC_SKIP_BOOT_ASSETS=1 bash "$ROOT/scripts/stage-slim-data.sh" "$SLIM" >/dev/null
fi

rm -rf "$OUT"
mkdir -p "$OUT/blobs" "$OUT/m"

COMMIT="${VERCEL_GIT_COMMIT_SHA:-${GITHUB_SHA:-$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo local)}}" \
python3 -I - "$SLIM" "$STOCK" "$OUT" "$SHELL_STOCK" "$work/pack" <<'PY'
import hashlib, json, os, sys, time, zipfile, zlib

slim, stock, out, shell_stock, work_dir = sys.argv[1:6]
os.makedirs(work_dir, exist_ok=True)
files = []

# 32 MiB: the page stores, verifies and resumes in units of this size, and
# static hosts cap single files at 100 MB. Changing it changes every chunk
# hash, so every visitor would download everything again: leave it.
CHUNK = 32 * 1024 * 1024

def put_blob(data):
    h = hashlib.sha256(data).hexdigest()
    path = os.path.join(out, 'blobs', h + '.bin')
    if not os.path.exists(path):
        with open(path + '.tmp', 'wb') as f:
            f.write(data)
        os.replace(path + '.tmp', path)
    return h

def add(src, path, group, stock=False):
    """Record one file of the game's data dir; its bytes go to blobs/."""
    whole = hashlib.sha256()
    chunks = []
    size = 0
    with open(src, 'rb') as f:
        while True:
            block = f.read(CHUNK)
            if not block:
                break
            whole.update(block)
            chunks.append(put_blob(block))
            size += len(block)
    if not chunks:  # an empty file is one empty chunk
        chunks.append(put_blob(b''))
    entry = {'name': os.path.basename(path), 'path': 'data/' + path, 'size': size,
             'sha256': whole.hexdigest(), 'rev': whole.hexdigest()[:16], 'group': group,
             'chunks': chunks}
    if stock:
        entry['stock'] = True
    files.append(entry)

# Reproducible zips. A ZipInfo carries everything that would otherwise come
# from the checkout (mtime, mode, the OS that wrote it).
epoch = int(os.environ.get('SOURCE_DATE_EPOCH') or 315532800)  # 1980-01-01
stamp = time.gmtime(max(epoch, 315532800))[:6]
STORED_EXT = ('.ogg', '.jpg', '.jpeg', '.png', '.pk3', '.dds', '.ktx', '.opus', '.webp')

def pack(name, entries):
    """entries: [(arcname, fullpath)], written in arcname order."""
    dst = os.path.join(work_dir, name)
    with zipfile.ZipFile(dst, 'w') as z:
        for arc, full in sorted(entries):
            zi = zipfile.ZipInfo(arc, date_time=stamp)
            zi.create_system = 3
            zi.external_attr = 0o100644 << 16
            level = None
            if arc.lower().endswith(STORED_EXT):
                zi.compress_type = zipfile.ZIP_STORED
            else:
                zi.compress_type = zipfile.ZIP_DEFLATED
                level = 6
            with open(full, 'rb') as f:
                z.writestr(zi, f.read(), compresslevel=level)
    return dst

PROGS = ('progs.dat', 'csprogs.dat', 'menu.dat')
progs = []

# The fork's pk3dirs, packed. "zz-" sorts after every stock pk3, so these
# win wherever both have a file, as the pk3dir does in a native install.
for entry in sorted(os.listdir(slim)):
    src = os.path.join(slim, entry)
    if not (entry.endswith('.pk3dir') and os.path.isdir(src)):
        continue
    items = []
    for d, dirs, fs in os.walk(src):
        dirs.sort()
        for f in sorted(fs):
            full = os.path.join(d, f)
            arc = os.path.relpath(full, src).replace(os.sep, '/')
            if arc.startswith('.') or '/.' in arc:
                continue
            if '/' not in arc and f in PROGS:
                progs.append((arc, full))  # game code: zz-progs.pk3, below
                continue
            if '/' not in arc and f.endswith('.lno'):
                continue  # line numbers, for debuggers only
            items.append((arc, full))
    name = 'zz-' + entry[:-len('.pk3dir')] + '.pk3'
    add(pack(name, items), name, 'core')

# "zz-progs" sorts after the stock data pk3, which has progs of its own.
if progs:
    add(pack('zz-progs.pk3', progs), 'zz-progs.pk3', 'core')

# Loose files at the top of the data dir (xonotic.cfg, touch/ profiles).
for d, dirs, fs in os.walk(slim):
    dirs[:] = sorted(x for x in dirs if not x.endswith('.pk3dir'))
    for f in sorted(fs):
        full = os.path.join(d, f)
        rel = os.path.relpath(full, slim).replace(os.sep, '/')
        if rel.startswith('.'):
            continue
        add(full, rel, 'core')

# Stock pk3s.
for f in sorted(os.listdir(stock)):
    if not f.endswith('.pk3') or 'nexcompat' in f:
        continue
    add(os.path.join(stock, f), f, 'music' if f.endswith('-music.pk3') else 'core', stock=True)

def write_manifest(doc):
    """Writes m/<id>.json, id = the start of its own text's sha256."""
    text = json.dumps(doc, sort_keys=True, separators=(',', ':')).encode()
    mid = hashlib.sha256(text).hexdigest()[:20]
    with open(os.path.join(out, 'm', mid + '.json'), 'wb') as f:
        f.write(text)
    return 'm/' + mid + '.json'

main = {'format': 3, 'chunk': CHUNK, 'files': files}
if shell_stock:
    main['stock'] = shell_stock  # stock files: that host's stock.json
current = write_manifest(main)

stock_files = [f for f in files if f.get('stock')]
if stock_files:
    stock_manifest = write_manifest({'format': 3, 'chunk': CHUNK, 'files': stock_files})
    with open(os.path.join(out, 'stock.json'), 'w') as f:
        json.dump({'format': 3, 'manifest': stock_manifest}, f)

# Pages loaded before this layout (format 2) read url / parts and keep their
# files under "rev". Point them at the blobs: same bytes, same 32 MiB parts.
legacy = []
for f in files:
    e = {k: f[k] for k in ('name', 'path', 'size', 'rev', 'group')}
    if f.get('stock'):
        e['stock'] = True
    urls = ['blobs/' + h + '.bin' for h in f['chunks']]
    if len(urls) == 1:
        e['url'] = urls[0]
    else:
        e['parts'] = [{'url': u, 'size': min(CHUNK, f['size'] - i * CHUNK)} for i, u in enumerate(urls)]
    legacy.append(e)
doc = {'format': 2, 'manifest': current, 'commit': os.environ.get('COMMIT', ''),
       'zlib': zlib.ZLIB_RUNTIME_VERSION, 'files': legacy}
if shell_stock:
    doc['stock'] = shell_stock
with open(os.path.join(out, 'manifest.json'), 'w') as m:
    json.dump(doc, m, indent=1)
with open(os.path.join(out, 'web-data.env'), 'w') as m:
    m.write(f'XT_DATA_MANIFEST={current}\n')

core = sum(f['size'] for f in files if f['group'] == 'core')
music = sum(f['size'] for f in files if f['group'] == 'music')
print(f"Staged {len(files)} files: core {core/1e6:.0f} MB, music {music/1e6:.0f} MB -> {out} ({current})")
PY

# The page names the manifest it was built with (the xt-config block in
# index.html). If the page is already built, write this one in; a page built
# later reads data/web-data.env itself.
page="$(dirname "$OUT")/index.html"
if [ -f "$page" ]; then
    python3 -I - "$page" "$OUT/web-data.env" <<'PY'
import json, re, sys
page, env = sys.argv[1:3]
manifest = open(env).read().strip().split('=', 1)[1]
html = open(page, encoding='utf-8').read()
pat = re.compile(r'(<script id="xt-config" type="application/json">)(.*?)(</script>)', re.S)
m = pat.search(html)
if m:
    cfg = json.loads(m.group(2))
    cfg['manifest'] = manifest
    html = html[:m.start(2)] + json.dumps(cfg, separators=(',', ':')) + html[m.end(2):]
    open(page, 'w', encoding='utf-8').write(html)
PY
fi
