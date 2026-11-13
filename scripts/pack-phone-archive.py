#!/usr/bin/env python3
"""Pack the phone packs into the one archive the app streams.

    scripts/pack-phone-archive.py -o OUT.zip pack-phone.pk3...

The app does not download files one by one: PackFetch reads the archive's
central directory and takes each pk3 out of its own byte range. The archive
therefore has to look like the autobuild's, entries under `Xonotic/data/`
named as the official packs -- the entry name is the name the pk3 is installed
under, and the engine loads any `xonotic-*.pk3` in its data directory.

So `xonotic-20261107-maps-phone.pk3` goes in as `xonotic-20261107-maps.pk3`.
The content stays the phone pack (ASTC textures, zstd entries, Opus audio); the
engine reads those formats straight out of an ordinary pk3.

Entries are stored, never deflated: a phone pack is compressed inside already,
and the app fetches byte ranges, which a deflated entry cannot resume.
"""

import argparse
import hashlib
import os
import sys
import zipfile


def official_name(pack):
    """The name a phone pack installs under: `xonotic-...-maps-phone.pk3` -> `xonotic-...-maps.pk3`."""
    base = os.path.basename(pack)
    stem, ext = os.path.splitext(base)
    if stem.endswith("-phone"):
        stem = stem[:-len("-phone")]
    return stem + ext


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("packs", nargs="+")
    ap.add_argument("-o", "--out", required=True, help="the archive to write")
    ap.add_argument("--folder", default="Xonotic/data/", help="folder inside the zip (Xonotic/data/)")
    args = ap.parse_args()
    folder = args.folder if args.folder.endswith("/") else args.folder + "/"

    seen = set()
    total = 0
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_STORED, allowZip64=False) as z:
        for pack in args.packs:
            name = folder + official_name(pack)
            if name in seen:
                sys.exit("duplicate pack name in the archive: " + name)
            seen.add(name)
            size = os.path.getsize(pack)
            total += size
            z.write(pack, name)
            print("  %-46s %7.1f MB" % (name, size / 1e6), file=sys.stderr)
    print("%s: %d packs, %.1f MB, sha256 %s"
          % (args.out, len(seen), total / 1e6, sha256(args.out)), file=sys.stderr)


if __name__ == "__main__":
    main()
