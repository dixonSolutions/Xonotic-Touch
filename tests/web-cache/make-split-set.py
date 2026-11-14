#!/usr/bin/env python3
"""A small made-up split data set for the web loader tests: the packs and
godnotic-data.json that scripts/split-data-packs.py writes for the real one
(docs/PHONE_ASSETS.md, "Split set"), a megabyte and a half in all.

    python3 tests/web-cache/make-split-set.py OUT [--archive] [--old OLDDIR]

OUT/godnotic-t-<pack>.pk3   core (every map's .mapinfo and level shot, no
                            .bsp), shared-1, shared-2, map-alpha (the default
                            map; needs shared-1), map-beta (shared-1 and
                            shared-2), map-gamma (nothing shared), music,
                            detail, extras
OUT/godnotic-data.json      the manifest (format 1)
OUT/expect.json             for the tests: every entry's sha256, and what
                            each map's .bsp lists (the files it loads)
--archive                   also OUT/Godnotic-data-t.zip, the packs stored
                            under Xonotic/data/, with "archive" in the
                            manifest (offsets, size, sha256)
--old OLDDIR                also the same game as the layout before the split
                            set: OLDDIR/xonotic-t-data.pk3, -maps.pk3 and
                            -music.pk3 under the official names

Entries are stored (no compression) and the bytes are fixed, so the same
call gives the same files.
"""
import hashlib
import json
import os
import struct
import sys
import zipfile
import zlib

STAMP = (1980, 1, 1, 0, 0, 0)


def noise(n, seed):
    out = bytearray()
    h = hashlib.sha256(str(seed).encode()).digest()
    while len(out) < n:
        h = hashlib.sha256(h).digest()
        out += h
    return bytes(out[:n])


def write_zip(path, entries):
    with zipfile.ZipFile(path, 'w') as z:
        for name, data in sorted(entries.items()):
            zi = zipfile.ZipInfo(name, date_time=STAMP)
            zi.compress_type = zipfile.ZIP_STORED
            zi.external_attr = 0o100644 << 16
            z.writestr(zi, data)


def ibsp(shaders):
    """A Q3 IBSP with a texture lump, for scripts/split-web-maps.py."""
    tex = b''.join(s.encode().ljust(64, b'\0') + struct.pack('<ii', 0, 1) for s in shaders)
    head = 8 + 17 * 8
    lumps = [(head, 0), (head, len(tex))] + [(0, 0)] * 15
    return b'IBSP' + struct.pack('<i', 46) + b''.join(struct.pack('<ii', o, l) for o, l in lumps) + tex


MAPS = {  # map -> (shared packs it needs, the textures its .bsp lists)
    'alpha': (['shared-1'], ['textures/s1', 'textures/alpha']),
    'beta': (['shared-1', 'shared-2'], ['textures/s1', 'textures/s2', 'textures/beta']),
    'gamma': ([], ['textures/gamma']),
}


def main():
    args = sys.argv[1:]
    out = args[0]
    os.makedirs(out, exist_ok=True)
    packs = {
        'core': ('core', {'gfx/menu.tga': noise(40000, 'menu'), 'pad/core.bin': noise(300000, 'core')}),
        'shared-1': ('shared', {'textures/s1.tga': noise(60000, 's1')}),
        'shared-2': ('shared', {'textures/s2.tga': noise(70000, 's2')}),
        'music': ('music', {'cdtracks.cfg': b'_cdtrack 1 a\n', 'sound/cdtracks/a.opus': noise(600000, 'music')}),
        'detail': ('detail', {'models/lod.md3': noise(50000, 'detail')}),
        'extras': ('extras', {'textures/extra.tga': noise(200000, 'extras')}),
    }
    loads = {}
    for m, (shared, textures) in MAPS.items():
        packs['core'][1][f'maps/{m}.mapinfo'] = f'title {m}\ngametype dm\n'.encode()
        packs['core'][1][f'maps/{m}.jpg'] = noise(3000, 'shot' + m)
        files = [t + '.tga' for t in textures]
        loads[m] = files
        packs['map-' + m] = ('map', {
            f'maps/{m}.bsp': ('bsp %s\n' % m + ''.join(f + '\n' for f in files)).encode() + noise(80000, 'bsp' + m),
            f'textures/{m}.tga': noise(20000, 'tex' + m),
        })
    order = ['core', 'shared-1', 'map-alpha', 'music', 'shared-2', 'map-beta', 'map-gamma', 'detail', 'extras']
    listed = []
    entries = {}
    for priority, name in enumerate(order):
        kind, content = packs[name]
        fname = f'godnotic-t-{name}.pk3'
        path = os.path.join(out, fname)
        write_zip(path, content)
        data = open(path, 'rb').read()
        for n, b in content.items():
            entries[n] = hashlib.sha256(b).hexdigest()
        p = {'name': name, 'file': fname, 'install_as': fname, 'kind': kind, 'size': len(data),
             'sha256': hashlib.sha256(data).hexdigest(), 'crc32': '%08x' % (zlib.crc32(data) & 0xffffffff),
             'priority': priority, 'entries': len(content), 'depends': []}
        if kind == 'map':
            m = name[4:]
            p['maps'] = [m]
            p['depends'] = list(MAPS[m][0])
        listed.append(p)
    manifest = {
        'format': 1, 'set': 't-' + hashlib.sha256(''.join(p['sha256'] for p in listed).encode()).hexdigest()[:12],
        'meta': {}, 'default_map': 'alpha',
        'first_play': ['core', 'shared-1', 'map-alpha'],
        'maps': {m: {'pack': 'map-' + m, 'needs': ['core'] + MAPS[m][0] + ['map-' + m]} for m in MAPS},
        'packs': listed, 'left_out': [],
    }
    if '--archive' in args:
        arc = os.path.join(out, 'Godnotic-data-t.zip')
        with zipfile.ZipFile(arc, 'w') as z:
            for p in listed:
                zi = zipfile.ZipInfo('Xonotic/data/' + p['file'], date_time=STAMP)
                zi.compress_type = zipfile.ZIP_STORED
                z.writestr(zi, open(os.path.join(out, p['file']), 'rb').read())
        with zipfile.ZipFile(arc) as z:
            for p in listed:
                zi = z.getinfo('Xonotic/data/' + p['file'])
                p['offset'] = zi.header_offset + 30 + len(zi.filename.encode())
        data = open(arc, 'rb').read()
        for p in listed:
            assert data[p['offset']:p['offset'] + p['size']] == open(os.path.join(out, p['file']), 'rb').read(), p['name']
        manifest['archive'] = {'file': 'Godnotic-data-t.zip', 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                               'folder': 'Xonotic/data/'}
    with open(os.path.join(out, 'godnotic-data.json'), 'w') as f:
        json.dump(manifest, f, indent=1)
    with open(os.path.join(out, 'expect.json'), 'w') as f:
        json.dump({'entries': entries, 'loads': loads, 'set': manifest['set'],
                   'first': [p['file'] for p in listed if p['name'] in manifest['first_play']],
                   'rest': [p['file'] for p in listed if p['name'] not in manifest['first_play']]}, f, indent=1)
    if '--old' in args:
        old = args[args.index('--old') + 1]
        os.makedirs(old, exist_ok=True)
        data = dict(packs['core'][1])
        maps = {}
        for name, (kind, content) in packs.items():
            if kind in ('shared', 'detail', 'extras'):
                # (dds/: where the map split looks for a shader's texture)
                maps.update({('dds/' + n[:-4] + '.dds' if n.startswith('textures/') else n): b for n, b in content.items()})
        for m, (shared, textures) in MAPS.items():
            for n in (f'maps/{m}.mapinfo', f'maps/{m}.jpg'):
                maps[n] = data.pop(n)
            maps[f'maps/{m}.bsp'] = ibsp(textures)
            maps[f'dds/textures/{m}.dds'] = packs['map-' + m][1][f'textures/{m}.tga']
        write_zip(os.path.join(old, 'xonotic-t-data.pk3'), data)
        write_zip(os.path.join(old, 'xonotic-t-maps.pk3'), maps)
        write_zip(os.path.join(old, 'xonotic-t-music.pk3'), packs['music'][1])


if __name__ == '__main__':
    main()
