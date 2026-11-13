#!/bin/bash
# Stage game data for the web build: build/web/data/, content-addressed.
#
#   scripts/stage-web-data.sh --stock <dir-with-stock-pk3s> [--astc <dir|zip>] [--web <dir|zip>]
#   scripts/stage-web-data.sh --zip <Xonotic-latest.zip>    [--astc ...] [--web ...]
#   scripts/stage-web-data.sh --shell <stock-data-url>
#   scripts/stage-web-data.sh --own-only        this fork's files only (tests)
#   --out DIR    write DIR/data instead of build/web/data
#   --slim DIR   take an already staged copy of this fork's files (tests)
#   --no-split   keep the maps pk3 whole (one profile, the layout before
#                maps on demand)
#
# Profiles (docs/WEB.md, "Game data"): the page picks one per browser.
#   web    the stock pk3s, or with --web a prebuilt "web" repack of them
#          (repack-phone-assets.py --profile slim --textures keep): DXT
#          textures, for every GPU; desktops have S3TC
#   astc   with --astc DIR|ZIP: the slim phone set (ASTC textures, zstd,
#          Opus), for GPUs with WEBGL_compressed_texture_astc
#   full   the stock pk3s as whole files, as every page before maps on
#          demand stored them. Its maps pk3 is not hosted ("hosted": false):
#          a page keeps this profile only while every chunk of it is
#          already in the browser, so returning visitors download nothing
# In web and astc the maps pk3 is split (scripts/split-web-maps.py) into a
# core pack, one pack per map, and an "extra" pack of textures no map uses.
#
# --shell stages only this fork's own files (about 15 MB) and points the page
# at another deployment's data/ for the stock pk3s, which the page reads at
# run time from that host's stock.json. GitHub Pages uses it.
#
# Download groups in a manifest:
#   core   this fork's files (zz-*.pk3 override the stock files; later pk3
#          names win), the loose configs, the stock data, xoncompat and font
#          pk3s, the maps core pack: needed before the menu
#   map    one map's pack ("map": its name): fetched when the map is asked
#          for, or in the background ("Download all maps")
#   extra  the maps pk3's unused textures: background only
#   music  the music pk3: optional, background
# Left out: nexcompat (old Nexuiz map textures, 125 MB; only servers running
# Nexuiz maps need it, and the browser cannot join servers).
#
# The compiled game code (progs.dat, csprogs.dat, menu.dat) goes into its own
# small zz-progs.pk3: it carries a per-commit watermark, so a code push
# changes that one file and nothing else. Every zip is reproducible: fixed
# timestamps (SOURCE_DATE_EPOCH, else 1980-01-01), attributes, entry order
# and compression level; the split map packs are copied entry by entry.
#
# Layout written:
#   data/blobs/<sha256>.bin  every hosted file cut into 32 MiB chunks, each
#                            named by its own hash: immutable
#   data/m/<id>.json         one manifest per profile, named by the hash of
#                            its own text: immutable
#   data/profiles.json       no-cache: {default, profiles: {name: {manifest,
#                            gpu?, keep?}}}; the built page carries a copy
#   data/stock.json          no-cache: the stock files alone, per profile,
#                            for shells on other hosts ("manifest": the full
#                            set's, for shells built before profiles)
#   data/manifest.json       no-cache: the web profile in the format pages
#                            before content addressing read (url / parts)
#   data/web-data.env        XT_DATA_MANIFEST=, XT_DATA_PROFILES= for the page
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
OUT="${XT_WEB_OUT:-$ROOT/build/web}/data"
STOCK=""
ZIP=""
SHELL_STOCK=""
OWN_ONLY=0
SLIM=""
ASTC=""
WEBSET=""
SPLIT=1

usage() { sed -n '2,62p' "$0" >&2; exit 2; }
while [ $# -gt 0 ]; do
    case "$1" in
        --stock) STOCK="${2:?}"; shift 2 ;;
        --zip) ZIP="${2:?}"; shift 2 ;;
        --shell) SHELL_STOCK="${2:?}"; shift 2 ;;
        --own-only) OWN_ONLY=1; shift ;;
        --slim) SLIM="${2:?}"; shift 2 ;;
        --astc) ASTC="${2:?}"; shift 2 ;;
        --web) WEBSET="${2:?}"; shift 2 ;;
        --no-split) SPLIT=0; shift ;;
        --out) OUT="${2:?}/data"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "stage-web-data: unknown argument: $1" >&2; usage ;;
    esac
done

work="$(mktemp -d "${TMPDIR:-/tmp}/xt-web-stage.XXXXXX")"
trap 'rm -rf "$work"' EXIT

# A data set given as a zip (Godnotic-data releases: Xonotic/data/*.pk3).
unpack() { # SRC NAME -> prints a directory of pk3s
    if [ -d "$1" ]; then echo "$1"; return; fi
    mkdir -p "$work/$2"
    unzip -q -j -o "$1" 'Xonotic/data/*.pk3' -d "$work/$2" >&2
    echo "$work/$2"
}

if [ -n "$ZIP" ]; then
    echo "Extracting stock pk3s from $ZIP…"
    STOCK="$(unpack "$ZIP" stock)"
fi
if [ -n "$SHELL_STOCK" ] || [ "$OWN_ONLY" = "1" ]; then
    STOCK=""
    ASTC=""
    WEBSET=""
else
    [ -n "$STOCK" ] || { echo "stage-web-data: pass --stock DIR, --zip FILE or --shell URL" >&2; usage; }
    [ -d "$STOCK" ] || { echo "stage-web-data: $STOCK is not a directory" >&2; exit 1; }
    ls "$STOCK"/xonotic-*-data.pk3 >/dev/null 2>&1 || { echo "stage-web-data: no xonotic-*-data.pk3 in $STOCK" >&2; exit 1; }
fi
[ -z "$ASTC" ] || ASTC="$(unpack "$ASTC" astc)"
[ -z "$WEBSET" ] || WEBSET="$(unpack "$WEBSET" webset)"

# The fork's own data, exactly as the Android and Flatpak packages ship it.
# The stock data pk3 has the menu skin, so the boot-asset fetch is not needed.
if [ -z "$SLIM" ]; then
    SLIM="$work/slim"
    XONOTIC_SKIP_BOOT_ASSETS=1 bash "$ROOT/scripts/stage-slim-data.sh" "$SLIM" >/dev/null
fi

# Split each profile's maps pk3 (seconds: entries are copied, not recompressed).
PY="${PYTHON:-python3}"
split() { # SRCDIR NAME -> prints the directory of split packs (or nothing)
    local maps data
    maps="$(ls "$1"/xonotic-*-maps.pk3 2>/dev/null | head -1 || true)"
    data="$(ls "$1"/xonotic-*-data.pk3 2>/dev/null | head -1 || true)"
    if [ "$SPLIT" = 1 ] && [ -n "$maps" ]; then
        "$PY" -I "$ROOT/scripts/split-web-maps.py" "$work/split-$2" "$maps" $data >&2
        echo "$work/split-$2"
    fi
}
WEB_SRC="${WEBSET:-$STOCK}"
WEB_SPLIT=""
[ -z "$WEB_SRC" ] || WEB_SPLIT="$(split "$WEB_SRC" web)"
ASTC_SPLIT=""
[ -z "$ASTC" ] || ASTC_SPLIT="$(split "$ASTC" astc)"

rm -rf "$OUT"
mkdir -p "$OUT/blobs" "$OUT/m"

COMMIT="${VERCEL_GIT_COMMIT_SHA:-${GITHUB_SHA:-$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo local)}}" \
"$PY" -I - "$SLIM" "$OUT" "$SHELL_STOCK" "$work/pack" "$STOCK" "$WEB_SRC" "$WEB_SPLIT" "$ASTC" "$ASTC_SPLIT" <<'PY'
import hashlib, json, os, sys, time, zipfile, zlib

slim, out, shell_stock, work_dir, stock, web_src, web_split, astc, astc_split = sys.argv[1:10]
os.makedirs(work_dir, exist_ok=True)

# 32 MiB: the page stores, verifies and resumes in units of this size, and
# static hosts cap single files at 100 MB. Changing it changes every chunk
# hash, so every visitor would download everything again: leave it.
CHUNK = 32 * 1024 * 1024
# The map a first visit downloads with the core data, so a first match on
# it needs no second wait: small (4 MB) and in most game types.
DEFAULT_MAP = 'stormkeep'

def put_blob(data, host):
    h = hashlib.sha256(data).hexdigest()
    path = os.path.join(out, 'blobs', h + '.bin')
    if host and not os.path.exists(path):
        with open(path + '.tmp', 'wb') as f:
            f.write(data)
        os.replace(path + '.tmp', path)
    return h

record_cache = {}

def record(src, path, group, stock=False, host=True, **extra):
    """One file of the game's data dir; its bytes go to blobs/ when hosted."""
    key = (os.path.realpath(src), host)
    if key not in record_cache:
        whole = hashlib.sha256()
        chunks = []
        size = 0
        with open(src, 'rb') as f:
            while True:
                block = f.read(CHUNK)
                if not block:
                    break
                whole.update(block)
                chunks.append(put_blob(block, host))
                size += len(block)
        if not chunks:  # an empty file is one empty chunk
            chunks.append(put_blob(b'', host))
        record_cache[key] = (size, whole.hexdigest(), chunks)
    size, digest, chunks = record_cache[key]
    entry = {'name': os.path.basename(path), 'path': 'data/' + path, 'size': size,
             'sha256': digest, 'rev': digest[:16], 'group': group, 'chunks': list(chunks)}
    if stock:
        entry['stock'] = True
    if not host:
        entry['hosted'] = False
    entry.update(extra)
    return entry

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

# ---- this fork's files: the same in every profile
PROGS = ('progs.dat', 'csprogs.dat', 'menu.dat')
progs = []
own = []

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
    own.append(record(pack(name, items), name, 'core'))

# "zz-progs" sorts after the stock data pk3, which has progs of its own.
if progs:
    own.append(record(pack('zz-progs.pk3', progs), 'zz-progs.pk3', 'core'))

# Loose files at the top of the data dir (xonotic.cfg, touch/ profiles).
for d, dirs, fs in os.walk(slim):
    dirs[:] = sorted(x for x in dirs if not x.endswith('.pk3dir'))
    for f in sorted(fs):
        full = os.path.join(d, f)
        rel = os.path.relpath(full, slim).replace(os.sep, '/')
        if rel.startswith('.'):
            continue
        own.append(record(full, rel, 'core'))

# ---- the stock files of one profile
def stock_files(src, split_dir, host_maps=True):
    files = []
    split = None
    if split_dir:
        with open(os.path.join(split_dir, 'split.json')) as f:
            split = json.load(f)
    for f in sorted(os.listdir(src)) if src else []:
        if not f.endswith('.pk3') or 'nexcompat' in f:
            continue
        full = os.path.join(src, f)
        if f.endswith('-music.pk3'):
            files.append(record(full, f, 'music', stock=True))
        elif split and f == split['source']:
            files.append(record(os.path.join(split_dir, split['index']), split['index'], 'core', stock=True))
            if split.get('extra'):
                files.append(record(os.path.join(split_dir, split['extra']), split['extra'], 'extra', stock=True))
            for name in sorted(split['maps']):
                p = split['maps'][name]['pack']
                files.append(record(os.path.join(split_dir, p), p, 'map', stock=True, map=name))
        else:
            maps_pk3 = f.endswith('-maps.pk3')
            files.append(record(full, f, 'core', stock=True, host=host_maps or not maps_pk3))
    return files

def write_manifest(doc):
    """Writes m/<id>.json, id = the start of its own text's sha256."""
    text = json.dumps(doc, sort_keys=True, separators=(',', ':')).encode()
    mid = hashlib.sha256(text).hexdigest()[:20]
    with open(os.path.join(out, 'm', mid + '.json'), 'wb') as f:
        f.write(text)
    return 'm/' + mid + '.json'

def manifest(profile, stock_list, with_own=True):
    files = (own if with_own else []) + stock_list
    doc = {'format': 4, 'chunk': CHUNK, 'profile': profile, 'files': files}
    if any(f.get('map') == DEFAULT_MAP for f in files):
        doc['defaultMap'] = DEFAULT_MAP
    if shell_stock and with_own:
        doc['stock'] = shell_stock  # stock files: that host's stock.json
    return write_manifest(doc)

profiles = {}
stock_profiles = {}
web_stock = stock_files(web_src, web_split)
profiles['web'] = {'manifest': manifest('web', web_stock)}
if web_stock:
    stock_profiles['web'] = {'manifest': manifest('web', web_stock, with_own=False)}
if astc:
    astc_stock = stock_files(astc, astc_split)
    profiles['astc'] = {'manifest': manifest('astc', astc_stock), 'gpu': 'astc'}
    stock_profiles['astc'] = {'manifest': manifest('astc', astc_stock, with_own=False), 'gpu': 'astc'}
if stock and web_split:
    # What pages before maps on demand stored: the stock pk3s whole. Only
    # the maps pk3 differs from the web profile, and it is not hosted.
    full_stock = stock_files(stock, None, host_maps=False)
    profiles['full'] = {'manifest': manifest('full', full_stock), 'keep': True}
    stock_profiles['full'] = {'manifest': manifest('full', full_stock, with_own=False), 'keep': True}

with open(os.path.join(out, 'profiles.json'), 'w') as f:
    json.dump({'format': 4, 'default': 'web', 'profiles': profiles}, f, sort_keys=True)
if stock_profiles:
    with open(os.path.join(out, 'stock.json'), 'w') as f:
        # "manifest" is for shells built before profiles, which read nothing
        # else: the full set, which their visitors already hold.
        legacy_stock = stock_profiles.get('full', stock_profiles['web'])['manifest']
        json.dump({'format': 4, 'manifest': legacy_stock, 'default': 'web',
                   'profiles': stock_profiles}, f, sort_keys=True)

# Pages loaded before content addressing (format 2) read url / parts and keep
# their files under "rev". Point them at the web profile's blobs.
legacy = []
for f in own + web_stock:
    e = {k: f[k] for k in ('name', 'path', 'size', 'rev', 'group')}
    if e['group'] in ('map', 'extra'):
        e['group'] = 'core'  # those pages load every file
    if f.get('stock'):
        e['stock'] = True
    urls = ['blobs/' + h + '.bin' for h in f['chunks']]
    if len(urls) == 1:
        e['url'] = urls[0]
    else:
        e['parts'] = [{'url': u, 'size': min(CHUNK, f['size'] - i * CHUNK)} for i, u in enumerate(urls)]
    legacy.append(e)
current = profiles['web']['manifest']
doc = {'format': 2, 'manifest': current, 'commit': os.environ.get('COMMIT', ''),
       'zlib': zlib.ZLIB_RUNTIME_VERSION, 'files': legacy}
if shell_stock:
    doc['stock'] = shell_stock
with open(os.path.join(out, 'manifest.json'), 'w') as m:
    json.dump(doc, m, indent=1)
with open(os.path.join(out, 'web-data.env'), 'w') as m:
    m.write(f'XT_DATA_MANIFEST={current}\n')
    m.write('XT_DATA_PROFILES=' + json.dumps({'default': 'web', 'profiles': profiles}, sort_keys=True, separators=(',', ':')) + '\n')

def summary(name, path):
    m = json.load(open(os.path.join(out, path)))
    by = {}
    for f in m['files']:
        by[f['group']] = by.get(f['group'], 0) + f['size']
    first = sum(f["size"] for f in m["files"] if f.get("hosted", True))
    return f"{name}: " + ', '.join(f'{g} {v/1e6:.0f} MB' for g, v in sorted(by.items())) + f"; one download {first/1e6:.0f} MB"
blobs = os.listdir(os.path.join(out, 'blobs'))
for name, p in profiles.items():
    print(summary(name, p['manifest']) + f" ({p['manifest']})")
print(f"Hosted: {sum(os.path.getsize(os.path.join(out, 'blobs', b)) for b in blobs)/1e6:.0f} MB in {len(blobs)} blobs -> {out}")
PY

# The page carries the profiles it was built with (the xt-config block in
# index.html). If the page is already built, write them in; a page built
# later reads data/web-data.env itself.
page="$(dirname "$OUT")/index.html"
if [ -f "$page" ]; then
    "$PY" -I - "$page" "$OUT/web-data.env" <<'PY'
import json, re, sys
page, env = sys.argv[1:3]
vals = dict(l.split('=', 1) for l in open(env).read().splitlines() if '=' in l)
html = open(page, encoding='utf-8').read()
pat = re.compile(r'(<script id="xt-config" type="application/json">)(.*?)(</script>)', re.S)
m = pat.search(html)
if m:
    cfg = json.loads(m.group(2))
    cfg['manifest'] = vals['XT_DATA_MANIFEST']
    if 'XT_DATA_PROFILES' in vals:
        cfg['profiles'] = json.loads(vals['XT_DATA_PROFILES'])
    html = html[:m.start(2)] + json.dumps(cfg, separators=(',', ':')) + html[m.end(2):]
    open(page, 'w', encoding='utf-8').write(html)
PY
fi
