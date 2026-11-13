#!/bin/sh
# Checks scripts/repack-phone-assets.py on a small made-up pk3: entries come
# back byte for byte (python's zipfile reads zstd entries since 3.14), zstd is
# used only where it pays, an entry the source already deflates smaller is
# copied compressed, symlinks stay symlinks, and -- when ffmpeg and
# astcenc are around -- a DXT1 texture becomes a well-formed ASTC KTX file
# whose symlink follows it.
#
#   scripts/test-phone-assets.sh   (needs python 3.14 and numpy)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
PY=${PYTHON:-python3}

"$PY" -I - "$tmp/in.pk3" <<'EOF'
import struct, sys, zipfile
def dxt1(w, h):
    # header, then one block per 4x4: red/blue endpoints, a gradient of indices
    hdr = b"DDS " + struct.pack("<7I", 124, 0x1007 | 0x20000, h, w, w * h // 2, 0, 1) + b"\0" * 44
    hdr += struct.pack("<2I4s5I", 32, 4, b"DXT1", 0, 0, 0, 0, 0) + struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    blk = struct.pack("<HHI", 0xF800, 0x001F, 0xE4E4E4E4)
    return hdr + blk * ((w // 4) * (h // 4))
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("maps/test.bsp", b"IBSP" + bytes(range(256)) * 400)
    z.writestr("gfx/pic.jpg", b"\xff\xd8\xff" + bytes(5000))
    z.writestr("tiny.cfg", b"set a 1\n")
    z.writestr("small.cfg", b'bind x "say hi"\n' * 5)
    z.writestr("dds/textures/wall.dds", dxt1(64, 32))
    z.writestr("sound/x.ogg", b"OggS" + bytes(100))
    for name, target in (("dds/textures/wall2.dds", "wall.dds"), ("maps/alias.bsp", "test.bsp")):
        i = zipfile.ZipInfo(name)
        i.external_attr = (0o120777 << 16)
        i.create_system = 3
        z.writestr(i, target)
EOF

check() {
	"$PY" -I - "$tmp/in.pk3" "$1" "$2" <<'EOF'
import json, os, struct, sys, zipfile
src, outdir, textures = sys.argv[1:4]
man = json.load(open(os.path.join(outdir, "phone-manifest.json")))
rec = man["packs"][0]
out = os.path.join(outdir, rec["file"])
assert rec["install_as"] == "in.pk3" and rec["size"] == os.path.getsize(out), rec
a, b = zipfile.ZipFile(src), zipfile.ZipFile(out)
bi = {i.filename: i for i in b.infolist()}
assert b.testzip() is None
for i in a.infolist():
    name = i.filename
    if textures == "astc" and name.startswith("dds/"):
        name = name[:-3] + "ktx"
    o = bi.pop(name)
    link = (i.external_attr >> 16) & 0o170000 == 0o120000
    assert ((o.external_attr >> 16) & 0o170000 == 0o120000) == link, name
    if name.endswith(".ktx"):
        d = b.read(o)
        if link:
            assert d == b"wall.ktx", d
            continue
        assert d[:12] == b"\xabKTX 11\xbb\r\n\x1a\n", d[:12]
        fmt, base, w, h = struct.unpack_from("<4I", d, 28)
        levels = struct.unpack_from("<I", d, 56)[0]
        assert (fmt, base, w, h, levels) == (0x93B7, 0x1907, 64, 32, 7), (hex(fmt), hex(base), w, h, levels)
        continue
    assert b.read(o) == a.read(i), name
    # an entry the source deflates well is copied compressed, not stored raw
    want = 93 if name in ("maps/test.bsp", "gfx/pic.jpg") \
        else 8 if name in ("dds/textures/wall.dds", "small.cfg", "sound/x.ogg") else 0
    assert o.compress_type == want, (name, o.compress_type)
assert not bi, bi
print("ok:", textures, rec["size"], "bytes")
EOF
}

"$PY" -I "$here/repack-phone-assets.py" --textures keep --audio keep -j 2 -o "$tmp/keep" "$tmp/in.pk3" 2>"$tmp/log" || { cat "$tmp/log"; exit 1; }
check "$tmp/keep" keep

astcenc=${ASTCENC:-$(command -v astcenc-avx2 || command -v astcenc-sse4.1 || command -v astcenc || true)}
if [ -n "$astcenc" ] && command -v ffmpeg >/dev/null 2>&1; then
	ASTCENC=$astcenc "$PY" -I "$here/repack-phone-assets.py" -j 2 -o "$tmp/astc" "$tmp/in.pk3" 2>"$tmp/log" || { cat "$tmp/log"; exit 1; }
	check "$tmp/astc" astc
else
	echo "skip: ASTC conversion (no astcenc or ffmpeg)"
fi
