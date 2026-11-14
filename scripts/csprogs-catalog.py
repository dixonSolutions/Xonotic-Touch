#!/usr/bin/env python3
"""Make the files touch_csprogs fetches (engine/darkplaces/touch_csprogs.c,
docs/JOIN_SPEED.md): for each csprogs.dat given (a file, or a pk3 holding one
at its root), write OUT/csprogs.dat.SIZE.CRC.deflate (raw deflate, as
DarkPlaces deflates downloads) and print the tcp_known line for it.

    scripts/csprogs-catalog.py OUT LABEL=PATH [LABEL=PATH ...]
    scripts/csprogs-catalog.py --check OUT      # every .deflate in OUT inflates to its name's size and CRC

SIZE is the raw size and CRC DarkPlaces' CRC-16 (CRC_Block: CCITT, initial
0xffff), the pair a server announces in csqc_progsize / csqc_progcrc.
"""
import hashlib
import os
import re
import sys
import zipfile
import zlib


def crc16(data):
    table = []
    for i in range(256):
        c = i << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
        table.append(c)
    c = 0xFFFF
    for b in data:
        c = ((c << 8) & 0xFFFF) ^ table[(c >> 8) ^ b]
    return c


def raw_deflate(data):
    z = zlib.compressobj(9, zlib.DEFLATED, -15)
    return z.compress(data) + z.flush()


def read_progs(path):
    if path.endswith(".pk3") or path.endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            return z.read("csprogs.dat")
    with open(path, "rb") as f:
        return f.read()


def check(out):
    bad = 0
    for name in sorted(os.listdir(out)):
        m = re.fullmatch(r"csprogs\.dat\.(\d+)\.(\d+)\.deflate", name)
        if not m:
            continue
        data = zlib.decompress(open(os.path.join(out, name), "rb").read(), -15)
        ok = len(data) == int(m.group(1)) and crc16(data) == int(m.group(2))
        print("%s %s sha256=%s" % ("ok  " if ok else "BAD ", name, hashlib.sha256(data).hexdigest()))
        bad += not ok
    return 1 if bad else 0


def main(argv):
    if len(argv) == 3 and argv[1] == "--check":
        return check(argv[2])
    if len(argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    out = argv[1]
    os.makedirs(out, exist_ok=True)
    for arg in argv[2:]:
        label, _, path = arg.partition("=")
        data = read_progs(path)
        size, crc = len(data), crc16(data)
        name = "csprogs.dat.%d.%d.deflate" % (size, crc)
        packed = raw_deflate(data)
        with open(os.path.join(out, name), "wb") as f:
            f.write(packed)
        print('\t{%d, %d, "%s", "%s"},  // %s, %d bytes deflated' % (size, crc, hashlib.sha256(data).hexdigest(), label, name, len(packed)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
