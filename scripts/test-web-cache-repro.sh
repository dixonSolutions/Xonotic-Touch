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

# The map split (scripts/split-web-maps.py): a made-up maps pk3 with two maps
# that share one texture and one each of their own, plus a texture no map
# uses. Splitting twice gives the same bytes, and every file lands where it
# should.
python3 -I - "$ROOT/scripts/split-web-maps.py" "$tmp" <<'PY'
import hashlib, importlib.util, json, os, struct, subprocess, sys, zipfile
script, tmp = sys.argv[1:3]

def bsp(shaders, ents=b''):
    """A Q3 IBSP with an entity lump and a texture lump, nothing else."""
    tex = b''.join(s.encode().ljust(64, b'\0') + struct.pack('<ii', 0, 1) for s in shaders)
    head = 8 + 17 * 8
    lumps = [(head, len(ents)), (head + len(ents), len(tex))] + [(0, 0)] * 15
    return b'IBSP' + struct.pack('<i', 46) + b''.join(struct.pack('<ii', o, l) for o, l in lumps) + ents + tex

src = os.path.join(tmp, 'xonotic-test-maps.pk3')
with zipfile.ZipFile(src, 'w') as z:
    z.writestr('maps/alpha.bsp', bsp(['textures/set/shared', 'textures/set/alpha_only'], b'{ "noise" "amb/wind.ogg" }'))
    z.writestr('maps/alpha.mapinfo', 'title Alpha\ngametype dm\n')
    z.writestr('maps/alpha.jpg', b'shot')
    z.writestr('maps/alpha.map', b'editor source')
    z.writestr('maps/beta.bsp', bsp(['textures/set/shared', 'textures/sky/beta_sky']))
    z.writestr('maps/beta.mapinfo', 'title Beta\n')
    z.writestr('dds/textures/set/shared.dds', b'S' * 100)
    z.writestr('dds/textures/set/alpha_only.dds', b'A' * 100)
    z.writestr('dds/textures/set/alpha_only_norm.dds', b'N' * 100)
    z.writestr('textures/set/unused.tga', b'U' * 100)
    z.writestr('sound/amb/wind.ogg', b'W' * 100)
    z.writestr('scripts/sky.shader', 'textures/sky/beta_sky\n{\n\tskyparms env/beta -\n}\n')
    z.writestr('env/beta_ft.jpg', b'F' * 10)
    z.writestr('gfx/beta_mini.tga', b'R' * 10)
    z.writestr('scripts/test.arena', 'arena')

outs = []
for n in (1, 2):
    out = os.path.join(tmp, f'split{n}')
    subprocess.run([sys.executable, '-I', script, out, src], check=True, stdout=subprocess.DEVNULL)
    outs.append(out)
files = sorted(os.listdir(outs[0]))
assert files == sorted(os.listdir(outs[1]))
for f in files:
    a, b = (open(os.path.join(o, f), 'rb').read() for o in outs)
    assert a == b, f'{f} differs between two splits'
split = json.load(open(os.path.join(outs[0], 'split.json')))
names = lambda f: sorted(zipfile.ZipFile(os.path.join(outs[0], f)).namelist())
alpha, beta = (names(split['maps'][m]['pack']) for m in ('alpha', 'beta'))
core, extra = names(split['index']), names(split['extra'])
assert alpha == ['dds/textures/set/alpha_only.dds', 'dds/textures/set/alpha_only_norm.dds', 'maps/alpha.bsp',
                 'maps/alpha.jpg', 'maps/alpha.mapinfo', 'sound/amb/wind.ogg'], alpha
assert beta == ['env/beta_ft.jpg', 'gfx/beta_mini.tga', 'maps/beta.bsp', 'maps/beta.mapinfo'], beta
assert 'dds/textures/set/shared.dds' in core and 'scripts/sky.shader' in core and 'scripts/test.arena' in core, core
assert extra == ['textures/set/unused.tga'], extra
z = zipfile.ZipFile(os.path.join(outs[0], split['index']))
assert z.read('maps/alpha.bsp') == b'', 'stand-in bsp'
assert z.read('maps/alpha.mapinfo').decode().startswith('title Alpha (download '), 'marked title'
assert z.read('maps/alpha.jpg') == b'shot', 'level shot in the core'
assert not any(n.endswith('.map') for n in core + alpha), 'editor sources left out'
for f in files:
    if f.endswith('.pk3'):
        assert zipfile.ZipFile(os.path.join(outs[0], f)).testzip() is None, f
print(f"map split reproducible: {len(split['maps'])} map packs, core {len(core)} files, extra {len(extra)}")
PY
