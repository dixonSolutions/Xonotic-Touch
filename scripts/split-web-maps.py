#!/usr/bin/env python3
"""Split a Xonotic maps pk3 into a core pack and one pack per map, for the web.

    split-web-maps.py OUTDIR MAPS.pk3 [OTHER.pk3 ...] [--report]

MAPS.pk3 is the pk3 to split (xonotic-<ver>-maps.pk3, full or slim). The
OTHER pk3s (the data pk3) are read for their shaders, models, QuakeC and
configs: whatever those name stays in the core. Writes, with <stem> the
maps pk3's name without ".pk3":

  OUTDIR/<stem>-0core.pk3    always loaded: files two or more maps share,
                             what the game itself names, and every
                             untraced file that is not a texture, model or
                             sound; plus each map's stand-ins, so the menu
                             lists maps not downloaded yet: the .mapinfo
                             with " (download N MB)" after its title, the
                             level shot, and an empty maps/<map>.bsp
  OUTDIR/<stem>-<map>.pk3    one per map: the .bsp, lightmaps, real
                             .mapinfo, waypoints, radar image, and every
                             texture, model and sound only that map uses
  OUTDIR/<stem>-0extra.pk3   textures, models and sounds no map and nothing
                             in the game was traced to (texture sets for
                             mappers, mostly): fetched only with "download
                             all maps"
  OUTDIR/split.json          {"index", "extra", "maps": {name: {pack, size}}}

Names: the map packs sort after the core pack (later pk3 names win in
DarkPlaces, so a map pack's .bsp and .mapinfo replace the stand-ins), and
all of them sort where the maps pk3 stood, so the data pk3 keeps its place.
Map editor sources (maps/*.map, *.map.options) are left out.

Tracing: the .bsp's shader names and the paths in its entity lump (noise
keys relative to sound/); shaders (scripts/*.shader) by name and every path
inside them, with skyparms' six sides; models (md3, iqm, .skin files) and
their surface shaders; a texture brings its _norm, _gloss, _glow, _pants,
_shirt, _reflect and _luma companions.

Entries are copied byte for byte (no recompression, zstd entries of the slim
packs too) into zips with fixed timestamps in name order: the same input
always gives the same bytes. Python 3.9+, standard library; the slim packs'
zstd entries are read with compression.zstd (3.14), the zstandard module or
the zstd command.
"""
import json
import os
import re
import struct
import sys
import zipfile
import zlib

EPOCH_DOSTIME = (0 << 11) | (0 << 5) | 0   # 00:00:00
EPOCH_DOSDATE = ((1980 - 1980) << 9) | (1 << 5) | 1   # 1980-01-01
COMPANIONS = ('', '_norm', '_gloss', '_glow', '_pants', '_shirt', '_reflect', '_luma', '_bump', '_deluxe', '_addmap')
SKY_SIDES = ('_rt', '_bk', '_lf', '_ft', '_up', '_dn', '_px', '_nx', '_py', '_ny', '_pz', '_nz')
MODEL_EXT = ('.md3', '.iqm', '.mdl', '.dpm', '.obj', '.ase', '.md2', '.zym', '.psk')
EDITOR_ONLY = re.compile(r'^maps/[^/]+\.map(\.options)?$', re.I)


def zstd_decompress(data):
    """zstd entries (method 93) of the slim packs: Python 3.14's
    compression.zstd, else the zstandard module, else the zstd command."""
    try:
        import compression.zstd as zs
        return zs.decompress(data)
    except ImportError:
        pass
    try:
        import zstandard
        return zstandard.ZstdDecompressor().decompressobj().decompress(data)
    except ImportError:
        pass
    import subprocess
    return subprocess.run(['zstd', '-dcq'], input=data, stdout=subprocess.PIPE, check=True).stdout


def stem_of(path):
    """Lower-case path without extension and without the dds/ prefix."""
    p = path.lower().replace('\\', '/')
    if p.startswith('dds/'):
        p = p[4:]
    base, dot, ext = p.rpartition('.')
    if dot and '/' not in ext:
        p = base
    return p


class Pack:
    def __init__(self, path):
        self.path = path
        self.zip = zipfile.ZipFile(path)
        self.f = open(path, 'rb')
        self.infos = [i for i in self.zip.infolist() if not i.is_dir()]
        self.by_name = {i.filename: i for i in self.infos}

    def raw(self, info):
        """(local-header-less compressed bytes) of an entry."""
        self.f.seek(info.header_offset)
        h = self.f.read(30)
        sig, _, _, _, _, _, _, _, _, nlen, xlen = struct.unpack('<IHHHHHIIIHH', h)
        assert sig == 0x04034b50, info.filename
        self.f.seek(info.header_offset + 30 + nlen + xlen)
        return self.f.read(info.compress_size)

    def read(self, info):
        """Uncompressed bytes (stored and deflate only; others give None)."""
        data = self.raw(info)
        if info.compress_type == zipfile.ZIP_STORED:
            return data
        if info.compress_type == zipfile.ZIP_DEFLATED:
            return zlib.decompress(data, -15)
        return None


def write_pack(dst, entries):
    """entries: [(name, info, raw_bytes_or_None, plain_bytes_or_None)] -> a zip
    with every entry copied raw (or stored, for new plain bytes)."""
    entries = sorted(entries, key=lambda e: e[0])
    central = []
    with open(dst + '.tmp', 'wb') as out:
        for name, info, raw, plain in entries:
            nb = name.encode('utf-8')
            if plain is not None:
                method, crc, csize, usize, data = 0, zlib.crc32(plain) & 0xffffffff, len(plain), len(plain), plain
            else:
                method, crc, csize, usize, data = info.compress_type, info.CRC, info.compress_size, info.file_size, raw
            flags = 0x800 if any(b > 127 for b in nb) else 0
            version = 63 if method == 93 else 20
            off = out.tell()
            out.write(struct.pack('<IHHHHHIIIHH', 0x04034b50, version, flags, method, EPOCH_DOSTIME, EPOCH_DOSDATE,
                                  crc, csize, usize, len(nb), 0))
            out.write(nb)
            out.write(data)
            central.append(struct.pack('<IHHHHHHIIIHHHHHII', 0x02014b50, (3 << 8) | version, version, flags, method,
                                       EPOCH_DOSTIME, EPOCH_DOSDATE, crc, csize, usize, len(nb), 0, 0, 0, 0,
                                       0o100644 << 16, off) + nb)
        start = out.tell()
        for c in central:
            out.write(c)
        size = out.tell() - start
        out.write(struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, len(central), len(central), size, start, 0))
    os.replace(dst + '.tmp', dst)
    return os.path.getsize(dst)


# ------------------------------------------------------------------ parsing

def bsp_refs(data):
    """Shader names and entity-lump paths of a Q3 (IBSP) map."""
    if data is None or len(data) < 8 or data[:4] not in (b'IBSP', b'RBSP', b'FBSP'):
        return set()
    out = set()
    nlumps = 17 if data[:4] == b'IBSP' else 18
    lumps = [struct.unpack_from('<ii', data, 8 + 8 * i) for i in range(nlumps)]
    eoff, elen = lumps[0]
    toff, tlen = lumps[1]
    for k in range(tlen // 72):
        name = data[toff + 72 * k: toff + 72 * k + 64].split(b'\0', 1)[0].decode('latin-1')
        out.add(stem_of(name))
    ents = data[eoff:eoff + elen].decode('latin-1', 'replace')
    for key, value in re.findall(r'"([^"]*)"\s+"([^"]*)"', ents):
        value = value.strip()
        if '/' in value and not value.startswith('*'):
            out.add(stem_of(value))
            if key.lower().startswith('noise') or key.lower() == 'sound':
                out.add('sound/' + stem_of(value))   # relative to sound/
    return out


def md3_refs(data):
    out = set()
    if not data or data[:4] != b'IDP3':
        return out
    try:
        (nsurf,) = struct.unpack_from('<i', data, 84)
        (osurf,) = struct.unpack_from('<i', data, 100)
        off = osurf
        for _ in range(nsurf):
            nshaders, = struct.unpack_from('<i', data, off + 76)
            oshaders, = struct.unpack_from('<i', data, off + 92)
            oend, = struct.unpack_from('<i', data, off + 104)
            for k in range(nshaders):
                s = data[off + oshaders + 68 * k: off + oshaders + 68 * k + 64].split(b'\0', 1)[0].decode('latin-1')
                if s:
                    out.add(stem_of(s))
            off += oend
    except struct.error:
        pass
    return out


def iqm_refs(data):
    out = set()
    if not data or data[:16] != b'INTERQUAKEMODEL\0':
        return out
    try:
        ntext, otext = struct.unpack_from('<II', data, 28)
        for s in data[otext:otext + ntext].split(b'\0'):
            s = s.decode('latin-1')
            if '/' in s or '.' in s:
                out.add(stem_of(s))
    except struct.error:
        pass
    return out


def text_paths(text):
    """Every token of a shader, skin or obj material that looks like a path."""
    return {stem_of(t) for t in re.findall(r'[A-Za-z0-9_\-./]+', text) if '/' in t}


def parse_shaders(text):
    """{shader name stem: (set of path stems, sky stems)}"""
    out = {}
    text = re.sub(r'//[^\n]*', '', text)
    i = 0
    n = len(text)
    tok = re.compile(r'\s*([^\s{}]+|\{|\})')
    while i < n:
        m = tok.match(text, i)
        if not m:
            break
        name = m.group(1)
        i = m.end()
        m = tok.match(text, i)
        if not m or m.group(1) != '{':
            continue
        depth = 0
        j = m.start(1)
        start = j
        while j < n:
            c = text[j]
            if c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        body = text[start:j]
        i = j + 1
        paths = text_paths(body)
        skies = set()
        for sm in re.finditer(r'skyparms\s+(\S+)', body, re.I):
            if sm.group(1) != '-':
                skies.add(stem_of(sm.group(1)))
        out.setdefault(stem_of(name), (set(), set()))
        out[stem_of(name)][0].update(paths)
        out[stem_of(name)][1].update(skies)
    return out


# ----------------------------------------------------------------- tracing

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    report = '--report' in sys.argv
    if len(args) < 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    outdir, maps_path, others = args[0], args[1], args[2:]
    os.makedirs(outdir, exist_ok=True)
    maps = Pack(maps_path)
    packs = [maps] + [Pack(p) for p in others]
    stem = os.path.basename(maps_path)[:-4]

    # Every file of the maps pk3, by stem (a stem may have several files:
    # dds/x.dds and x.tga, or x.ktx in the slim packs).
    by_stem = {}
    for info in maps.infos:
        by_stem.setdefault(stem_of(info.filename), []).append(info)

    shaders = {}
    for p in packs:
        for info in p.infos:
            if info.filename.lower().endswith('.shader'):
                data = p.read(info)
                if data is None:
                    data = zstd_decompress(p.raw(info))
                for k, v in parse_shaders(data.decode('latin-1')).items():
                    cur = shaders.setdefault(k, (set(), set()))
                    cur[0].update(v[0])
                    cur[1].update(v[1])

    def file_bytes(info):
        data = maps.read(info)
        if data is None:
            data = zstd_decompress(maps.raw(info))
        return data

    model_cache = {}

    def model_refs(info):
        if info.filename not in model_cache:
            low = info.filename.lower()
            refs = set()
            if low.endswith('.md3'):
                refs = md3_refs(file_bytes(info))
            elif low.endswith('.iqm'):
                refs = iqm_refs(file_bytes(info))
            elif low.endswith(('.skin', '.mtl', '.obj')) or low.endswith('.shader'):
                refs = text_paths(file_bytes(info).decode('latin-1'))
            model_cache[info.filename] = refs
        return model_cache[info.filename]

    # Skins of a model: models/x.md3_0.skin, models/x.iqm_1.skin ...
    skins_of = {}
    for info in maps.infos:
        low = info.filename.lower()
        m = re.match(r'(.+\.(?:md3|iqm|obj|dpm|mdl))_\d+\.skin$', low)
        if m:
            skins_of.setdefault(m.group(1), []).append(info)

    def closure(seeds):
        seen_stems = set()
        files = set()
        todo = list(seeds)
        while todo:
            s = todo.pop()
            if s in seen_stems:
                continue
            seen_stems.add(s)
            if s in shaders:
                paths, skies = shaders[s]
                todo.extend(paths)
                for sky in skies:
                    todo.extend(sky + side for side in SKY_SIDES)
            for suffix in COMPANIONS:
                for info in by_stem.get(s + suffix, ()):
                    if info.filename in files:
                        continue
                    files.add(info.filename)
                    low = info.filename.lower()
                    if low.endswith(MODEL_EXT) or low.endswith(('.skin', '.mtl')):
                        todo.extend(model_refs(info))
                        for sk in skins_of.get(low, ()):
                            files.add(sk.filename)
                            todo.extend(model_refs(sk))
        return files

    # What the game itself names (QuakeC, configs, the other packs' models
    # and shaders) stays in the core whatever the maps use.
    global_seeds = set()
    for p in packs[1:]:
        for info in p.infos:
            low = info.filename.lower()
            if low.endswith(('.dat', '.cfg', '.shader', '.skin', '.txt', '.md3', '.iqm', '.def')):
                data = p.read(info)
                if data is None:
                    data = zstd_decompress(p.raw(info))
                if low.endswith('.md3'):
                    global_seeds |= md3_refs(data)
                elif low.endswith('.iqm'):
                    global_seeds |= iqm_refs(data)
                else:
                    global_seeds |= text_paths(data.decode('latin-1'))
    for info in maps.infos:
        if re.match(r'^maps/_', info.filename, re.I) and info.filename.lower().endswith('.bsp'):
            global_seeds |= bsp_refs(file_bytes(info))
    core_files = closure(global_seeds)

    bsps = sorted(i.filename for i in maps.infos
                  if re.match(r'^maps/[^/]+\.bsp$', i.filename, re.I) and not os.path.basename(i.filename).startswith('_'))
    users = {}
    own = {}
    for bsp in bsps:
        name = os.path.basename(bsp)[:-4]
        seeds = bsp_refs(file_bytes(maps.by_name[bsp]))
        files = closure(seeds)
        # The map's own files: maps/<name>.*, maps/<name>/..., and what the
        # game finds by the map's name (gfx/<name>_mini radar images).
        low_name = name.lower()
        for info in maps.infos:
            low = info.filename.lower()
            if (low.startswith('maps/' + low_name + '.') or low.startswith('maps/' + low_name + '/')
                    or low.startswith('gfx/' + low_name + '_')):
                files.add(info.filename)
        own[name] = files
        for f in files:
            users.setdefault(f, set()).add(name)

    # Big asset kinds no map and nothing in the game was traced to: texture
    # sets for mappers, mostly. They go into an "extra" pack, fetched with
    # "Download all maps"; everything else untraced stays in the core.
    ASSET_DIRS = ('dds/', 'textures/', 'env/', 'models/', 'sound/')
    per_map = {name: [] for name in own}
    core = []
    extra = []
    for info in maps.infos:
        fn = info.filename
        if EDITOR_ONLY.match(fn):
            continue
        u = users.get(fn, set())
        if fn in core_files:
            core.append(info)
        elif len(u) == 1:
            per_map[next(iter(u))].append(info)
        elif not u and fn.lower().startswith(ASSET_DIRS):
            extra.append(info)
        else:
            core.append(info)

    sizes = {name: sum(i.compress_size for i in per_map[name]) for name in per_map}
    if report:
        mbs = lambda infos: sum(i.compress_size for i in infos) / 1e6
        shared = [i for i in core if len(users.get(i.filename, ())) > 1]
        print(f'{os.path.basename(maps_path)}: {mbs(maps.infos):.1f} MB, {len(maps.infos)} files, {len(bsps)} maps')
        print(f'  core {mbs(core):.1f} MB (shared by 2+ maps {mbs(shared):.1f} MB), extra {mbs(extra):.1f} MB')
        for name in sorted(sizes, key=sizes.get, reverse=True):
            print(f'  {name:20s} {sizes[name]/1e6:7.1f} MB {len(per_map[name]):5d} files')
        return

    written = {}
    index_entries = []
    for info in core:
        index_entries.append((info.filename, info, maps.raw(info), None))
    result = {'format': 1, 'source': os.path.basename(maps_path), 'index': f'{stem}-0core.pk3', 'maps': {}}
    if extra:
        result['extra'] = f'{stem}-0extra.pk3'
        written[result['extra']] = write_pack(os.path.join(outdir, result['extra']),
                                              [(i.filename, i, maps.raw(i), None) for i in extra])
    for name in sorted(per_map):
        pack_name = f'{stem}-{name.lower()}.pk3'
        entries = [(i.filename, i, maps.raw(i), None) for i in per_map[name]]
        size = write_pack(os.path.join(outdir, pack_name), entries)
        written[pack_name] = size
        result['maps'][name] = {'pack': pack_name, 'size': size}
        mi = 'maps/' + name + '.mapinfo'
        if mi in maps.by_name:
            text = file_bytes(maps.by_name[mi]).decode('utf-8', 'replace')
            mb = max(1, round(size / 1e6))
            if re.search(r'^title[ \t]', text, re.M):
                text = re.sub(r'^(title[ \t]+)(.*)$', lambda m: f'{m.group(1)}{m.group(2).rstrip()} (download {mb} MB)', text, count=1, flags=re.M)
            else:
                text = f'title {name} (download {mb} MB)\n' + text
            index_entries.append((mi, None, None, text.encode('utf-8')))
        index_entries.append(('maps/' + name + '.bsp', None, None, b''))
        for ext in ('.jpg', '.png', '.tga'):
            shot = maps.by_name.get('maps/' + name + ext)
            if shot:
                index_entries.append((shot.filename, shot, maps.raw(shot), None))
    write_pack(os.path.join(outdir, result['index']), index_entries)
    with open(os.path.join(outdir, 'split.json'), 'w') as f:
        json.dump(result, f, indent=1, sort_keys=True)
    print(f'{os.path.basename(maps_path)}: core {os.path.getsize(os.path.join(outdir, result["index"]))/1e6:.0f} MB,'
          f' {len(per_map)} map packs {sum(v for k, v in written.items() if k != result.get("extra"))/1e6:.0f} MB,'
          f' extra {written.get(result.get("extra"), 0)/1e6:.0f} MB')


if __name__ == '__main__':
    main()
