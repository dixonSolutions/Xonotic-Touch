#!/usr/bin/env python3
"""Split the phone pack set into a core pack, per-map packs, shared packs,
music and extras, and write the manifest clients drive the download from.

    scripts/split-data-packs.py --usage USAGE.tsv -o OUTDIR PHONEDIR [--archive]

PHONEDIR is an output directory of scripts/repack-phone-assets.py (its
phone-manifest.json says which pk3 is which official pack). USAGE.tsv says
which entries a running client opens (scripts/test-data-split.sh --trace
writes it from fs_opentrace logs of scripted matches on every map): one line
per entry, "<official pack>\\t<entry>\\t<users>", users being "menu" and/or
the maps whose matches opened it, comma-separated.

Every entry of every input pack lands in exactly one output pack:

  core    what the menu and any match open: the data pack's entries that a
          match on any map opened (all but a few are opened by every match:
          the QuakeC precaches every registered model and sound), the menu's
          files, every map's mapinfo, level shot and shader script (so the
          menu can list every map, installed or not), fonts, xoncompat.
  map-<m> the files only map <m> opened (or that only it can use: entries
          under maps/<m>/, sound/map_<m>/, ...): bsp, lightmaps, waypoints,
          its own textures, sounds and models.
  shared  files two or more maps opened, grouped by which maps use them so
          that a map's dependencies are few and fetch little it does not
          need. A map is playable once its pack and its shared packs are in.
  music   the music pack, whole.
  detail  the player models' LOD meshes: the QuakeC uses them only if they
          exist (fexists), so the first match runs without them.
  extras  everything no traced run opened: textures of the stock texture
          sets no stock map uses (community maps do), unused HUD and menu
          skins, the overkill player models, ... Never deleted: a server or
          mod can still ask for them, and the set stays complete.

No entry is in two packs, the union is the input set (symlinks follow their
targets into the same pack), and a pack mounted while the game runs can be
added at the lowest priority (fs_mountpack): nothing in it shadows anything.

Writes OUTDIR/godnotic-<date>-*.pk3, OUTDIR/godnotic-data.json (the
manifest, see docs/PHONE_ASSETS.md) and OUTDIR/split-sizes.md; with
--archive also OUTDIR/Godnotic-data-split.zip, every pack a stored entry
under Xonotic/data/, and the manifest gets each pack's byte range in it.
"""

import argparse
import collections
import gzip
import hashlib
import json
import os
import re
import struct
import sys
import time
import zipfile
import zlib

FORMAT = 1


def add_stored(z, path, name):
    """A stored entry with a fixed time and mode: the same packs always make
    the same archive, byte for byte (zipfile.write would take the file's
    mtime)."""
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_STORED
    info.external_attr = 0o644 << 16
    info.create_system = 3
    with open(path, "rb") as src, z.open(info, "w") as dst:
        for chunk in iter(lambda: src.read(1 << 22), b""):
            dst.write(chunk)


def log(*a):
    print(*a, file=sys.stderr, flush=True)


def role_of(install_as):
    """xonotic-20261107-maps.pk3 -> maps; font-unifont-20261107.pk3 -> font-unifont."""
    stem = install_as[:-4] if install_as.endswith(".pk3") else install_as
    m = re.match(r"xonotic-\d+-(.+)$", stem)
    if m:
        return m.group(1)
    return re.sub(r"-\d+$", "", stem)


def date_of(install_as):
    m = re.search(r"(\d{8})", install_as)
    return m.group(1) if m else "0"


def key_of(name):
    """Usage traces of a set where a texture stayed DDS still match the KTX."""
    if name.startswith("dds/") and (name.endswith(".dds") or name.endswith(".ktx")):
        return name[:-4]
    return name


# entries every client needs whatever it plays: the menu lists every map from
# these, and the engine parses every shader script at start-up
ALWAYS_CORE_MAPS = ["scripts/*.shader", "maps/*.mapinfo", "maps/*.jpg", "maps/*.png", "maps/*.tga",
                    "maps/*.txt", "maps/*.instantaction", "maps/*.cfg", "*.cfg", "*.txt"]
# never needed first: the QuakeC loads them only if they exist
DETAIL = [r"^models/player/.*_lod[12]\.[^/]+$"]


def glob1(name, pattern):
    """fnmatch where * stays inside one path component."""
    rx = "".join("[^/]*" if c == "*" else "[^/]" if c == "?" else re.escape(c) for c in pattern)
    return re.fullmatch(rx, name) is not None


def map_hint(name, maps):
    """The map an entry belongs to by its path, if any."""
    m = re.match(r"maps/([^/]+)/", name) or re.match(r"maps/([^/.]+)\.[^/]+$", name)
    if m and m.group(1) in maps:
        return m.group(1)
    for part in name.split("/")[:-1]:
        mm = re.match(r"(?:map_)?(.+)$", part)
        if part.startswith("map_") and mm.group(1) in maps:
            return mm.group(1)
    return None


def usage_from_traces(paths):
    """Usage table lines from fs_opentrace files named <map>-<anything>.tsv
    (a match on that map) or menu*.tsv (the menu alone)."""
    users = collections.defaultdict(set)
    for p in paths:
        base = os.path.basename(p)[:-4]
        who = "menu" if base.startswith("menu") else base.split("-")[0] if "--" not in base else base.split("--")[0]
        with open(p, encoding="utf-8", errors="replace") as f:
            for line in f:
                parts = line.rstrip("\n").split("\t")
                if len(parts) == 2:
                    users[(parts[0], parts[1])].add(who)
    return ["%s\t%s\t%s\n" % (k[0], k[1], ",".join(sorted(v))) for k, v in sorted(users.items())]


def read_usage(path):
    """{(role, key): set of users}"""
    usage = collections.defaultdict(set)
    op = gzip.open if path.endswith(".gz") else open
    with op(path, "rt", encoding="utf-8") as f:
        for line in f:
            if not line.strip() or line.startswith("#"):
                continue
            pack, name, users = line.rstrip("\n").split("\t")
            usage[(role_of(pack), key_of(name))].update(u for u in users.split(",") if u)
    return usage


def link_target(name, data):
    text = data.decode("utf-8", "replace")
    return os.path.normpath(os.path.join(os.path.dirname(name), text)).replace(os.sep, "/")


def is_link(info):
    return (info.external_attr >> 16) & 0o170000 == 0o120000


def raw_entry(f, info):
    """The entry's local payload as it is (no recompression)."""
    f.seek(info.header_offset)
    h = f.read(30)
    if h[:4] != b"PK\x03\x04":
        raise ValueError("bad local header: " + info.filename)
    n, e = struct.unpack_from("<HH", h, 26)
    f.seek(n + e, 1)
    data = f.read(info.compress_size)
    if len(data) != info.compress_size:
        raise ValueError("truncated: " + info.filename)
    return data


class PackWriter:
    """Copies entries between pk3s without recompressing (method, crc, sizes,
    unix mode and time kept; no zip64, as the repacker)."""

    def __init__(self, path):
        self.path = path
        self.f = open(path + ".part", "wb")
        self.central = []

    def copy(self, info, payload):
        raw = info.filename.encode("utf-8")
        flags = 0x800 if not info.filename.isascii() else 0
        need = 63 if info.compress_type == 93 else 20 if info.compress_type == 8 else 10
        y, mo, d, h, mi, s = info.date_time
        dostime = ((max(y, 1980) - 1980) << 25) | (mo << 21) | (d << 16) | (h << 11) | (mi << 5) | (s // 2)
        off = self.f.tell()
        if off >= 1 << 32:
            raise OverflowError("pack needs zip64")
        self.f.write(struct.pack("<IHHHHHIIIHH", 0x04034B50, need, flags, info.compress_type, dostime & 0xFFFF,
                                 dostime >> 16, info.CRC, len(payload), info.file_size, len(raw), 0))
        self.f.write(raw)
        self.f.write(payload)
        self.central.append(struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, (3 << 8) | need, need, flags,
                                        info.compress_type, dostime & 0xFFFF, dostime >> 16, info.CRC,
                                        len(payload), info.file_size, len(raw), 0, 0, 0, 0, info.external_attr,
                                        off) + raw)

    def close(self):
        start = self.f.tell()
        for c in self.central:
            self.f.write(c)
        size = self.f.tell() - start
        if len(self.central) >= 0xFFFF:
            raise OverflowError("pack needs zip64")
        self.f.write(struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(self.central), len(self.central), size, start, 0))
        self.f.close()
        os.replace(self.path + ".part", self.path)


def file_digests(path):
    h, c = hashlib.sha256(), 0
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
            c = zlib.crc32(chunk, c)
    return h.hexdigest(), "%08x" % c


def cluster_shared(entries, size, budget):
    """Group entries used by several maps into shared packs.

    Starts from one group per set of using maps and merges the pair that
    costs least, the cost being the bytes maps would then fetch without
    needing them (a map depends on every group holding one of its entries),
    until no merge costs less than `budget` bytes. Small groups merge first,
    so the result is a handful of packs, each of a size worth a request."""
    groups = collections.defaultdict(list)
    for name, users in entries.items():
        groups[frozenset(users)].append(name)
    cl = [[set(u), list(v), sum(size[n] for n in v)] for u, v in groups.items()]

    def cost(a, b):
        return len(a[0] - b[0]) * b[2] + len(b[0] - a[0]) * a[2]

    while len(cl) > 1:
        best = None
        for i in range(len(cl)):
            for j in range(i + 1, len(cl)):
                c = cost(cl[i], cl[j])
                # a tiny group is worth an extra fetch less than a request
                small = min(cl[i][2], cl[j][2])
                c = c - 8 * min(small, 256 << 10)
                if best is None or c < best[0]:
                    best = (c, i, j)
        if best[0] > budget:
            break
        _, i, j = best
        a, b = cl[i], cl.pop(j)
        a[0] |= b[0]
        a[1] += b[1]
        a[2] += b[2]
    cl.sort(key=lambda g: (-len(g[0]), -g[2]))
    return [(g[0], g[1]) for g in cl]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("phonedir", help="output directory of repack-phone-assets.py (phone-manifest.json)")
    ap.add_argument("-o", "--outdir", required=True)
    ap.add_argument("--usage", required=True, help="usage table (.tsv or .tsv.gz)")
    ap.add_argument("--default-map", default="stormkeep", help="the map of the first match (default %(default)s)")
    ap.add_argument("--map-order", default="", help="comma-separated maps to fetch first after the default one")
    ap.add_argument("--shared-budget", type=int, default=0,
                    help="merge shared groups while a merge makes maps fetch fewer unneeded bytes than this "
                         "(default: 3%% of the bytes maps share, about 1 MB for the ASTC set)")
    ap.add_argument("--archive", action="store_true", help="also write Godnotic-data-split.zip (stored entries)")
    ap.add_argument("--archive-name", default="Godnotic-data-split.zip")
    ap.add_argument("--set-id", default="", help="manifest set id (default: <date>-<sha of the inputs>)")
    ap.add_argument("--meta", action="append", default=[], metavar="KEY=VALUE")
    args = ap.parse_args()

    with open(os.path.join(args.phonedir, "phone-manifest.json")) as f:
        phone = json.load(f)
    usage = read_usage(args.usage)

    # every entry of every input pack: (role, name) -> (pack path, info)
    # The engine mounts pk3s in name order, a later one shadowing an earlier
    # one's file of the same name; the split keeps the file the engine used.
    sources, entries, date = {}, {}, "0"
    owner, shadowed = {}, []
    for rec in sorted(phone["packs"], key=lambda r: r["install_as"]):
        role = role_of(rec["install_as"])
        if role == "nexcompat":
            continue
        date = max(date, date_of(rec["install_as"]))
        path = os.path.join(args.phonedir, rec["file"])
        z = zipfile.ZipFile(path)
        sources[role] = (path, z)
        for info in z.infolist():
            if info.is_dir():
                continue
            if info.filename in owner:
                shadowed.append(owner[info.filename])
                del entries[owner[info.filename]]
            owner[info.filename] = (role, info.filename)
            entries[(role, info.filename)] = info
    for k in shadowed:
        log("  %s in %s is shadowed by a later pack: left out" % (k[1], k[0]))
    if "maps" not in sources or "data" not in sources:
        sys.exit("need the data and maps packs")
    size = {k: i.compress_size + len(k[1]) for k, i in entries.items()}

    maps = sorted(n[5:-4] for (r, n) in entries if r == "maps" and re.match(r"maps/[^/]+\.bsp$", n))
    if args.default_map not in maps:
        sys.exit("default map %s is not in the maps pack" % args.default_map)

    links = {}
    for (role, name), info in entries.items():
        if is_link(info):
            links[(role, name)] = (role, link_target(name, sources[role][1].read(info)))

    def users_of(k):
        return usage.get((k[0], key_of(k[1])), set())

    # ---- assignment
    where = {}
    shared = {}
    for k, info in entries.items():
        role, name = k
        users = users_of(k)
        if role == "music":
            where[k] = "music"
        elif role in ("xoncompat",) or role.startswith("font-"):
            where[k] = "core"
        elif role == "data":
            if any(re.match(p, name) for p in DETAIL) and "menu" not in users:
                where[k] = "detail"
            elif users:
                where[k] = "core"
            else:
                where[k] = "extras"
        elif role == "maps":
            mapusers = users - {"menu"}
            if "menu" in users or (any(glob1(name, p) for p in ALWAYS_CORE_MAPS)
                                   and not name.endswith(".bsp")):
                where[k] = "core"
            elif len(mapusers) == 1:
                where[k] = "map-" + next(iter(mapusers))
            elif len(mapusers) > 1:
                shared[k] = mapusers
            else:
                h = map_hint(name, maps)
                where[k] = ("map-" + h) if h else "extras"
        else:
            where[k] = "extras"

    # shared groups; a group every map needs goes to the core
    budget = args.shared_budget or int(0.03 * sum(size[k] for k in shared))
    groups = cluster_shared(shared, size, budget)
    deps = collections.defaultdict(set)
    gi = 0
    for users, names in groups:
        if len(users) >= max(3, len(maps) - 1):
            for n in names:
                where[n] = "core"
            continue
        gi += 1
        for n in names:
            where[n] = "shared-%d" % gi
        for m in users:
            deps["map-" + m].add("shared-%d" % gi)

    # symlinks: the link goes where its target goes; a target that a core
    # link needs goes to the core
    changed = True
    while changed:
        changed = False
        for lk, tk in links.items():
            if tk not in where:
                continue  # dangling: leave the link where it is
            if where[lk] == "core" and where[tk] != "core":
                where[tk] = "core"
                changed = True
            elif where[lk] != where[tk] and where[lk] != "core":
                where[lk] = where[tk]
                changed = True

    # every map pack exists, even an empty one, so "map m is installed" is one check
    for m in maps:
        deps.setdefault("map-" + m, set())

    # ---- order: core, the first map and what it needs, music, the other maps
    # (each after its shared packs), detail, extras
    order = ["core"]
    first = "map-" + args.default_map
    order += sorted(deps[first]) + [first, "music"]
    rest = [m.strip() for m in args.map_order.split(",") if m.strip()]
    rest += [m for m in maps if m not in rest]
    for m in rest:
        p = "map-" + m
        if p in order:
            continue
        order += [d for d in sorted(deps[p]) if d not in order] + [p]
    order += [p for p in sorted(set(where.values())) if p.startswith("shared-") and p not in order]
    order += ["detail", "extras"]

    # ---- write
    os.makedirs(args.outdir, exist_ok=True)
    install = {p: "godnotic-%s-%s.pk3" % (date, p) for p in order}
    content = collections.defaultdict(list)
    for k in sorted(entries, key=lambda k: (k[0], entries[k].header_offset)):
        content[where[k]].append(k)
    handles = {role: open(path, "rb") for role, (path, z) in sources.items()}
    packs = []
    t0 = time.time()
    for prio, p in enumerate(order):
        if not content[p] and not p.startswith("map-"):
            continue
        out = os.path.join(args.outdir, install[p])
        w = PackWriter(out)
        names = set()
        for k in content[p]:
            info = entries[k]
            if info.filename in names:
                sys.exit("%s twice in %s" % (info.filename, p))
            names.add(info.filename)
            w.copy(info, raw_entry(handles[k[0]], info))
        w.close()
        sha, crc = file_digests(out)
        kind = p.split("-")[0]
        rec = {"name": p, "file": install[p], "install_as": install[p], "kind": kind,
               "size": os.path.getsize(out), "sha256": sha, "crc32": crc, "priority": prio,
               "entries": len(content[p]), "depends": sorted(deps.get(p, ()))}
        if kind == "map":
            rec["maps"] = [p[4:]]
        packs.append(rec)
    for h in handles.values():
        h.close()
    log("split into %d packs in %.0f s" % (len(packs), time.time() - t0))

    srcsha = hashlib.sha256("".join(sorted(r.get("sha256", r["file"]) for r in phone["packs"])).encode()).hexdigest()[:12]
    first_play = ["core"] + sorted(deps[first]) + [first]
    manifest = {
        "format": FORMAT,
        "set": args.set_id or "%s-%s" % (date, srcsha),
        "meta": dict(m.split("=", 1) for m in args.meta if "=" in m),
        "default_map": args.default_map,
        "first_play": first_play,
        "maps": {m: {"pack": "map-" + m, "needs": ["core"] + sorted(deps["map-" + m]) + ["map-" + m]}
                 for m in maps},
        "packs": packs,
        "left_out": ["%s:%s" % k for k in shadowed],
    }
    if args.archive:
        apath = os.path.join(args.outdir, args.archive_name)
        with zipfile.ZipFile(apath + ".part", "w", zipfile.ZIP_STORED, allowZip64=False) as z:
            for rec in packs:
                add_stored(z, os.path.join(args.outdir, rec["file"]), "Xonotic/data/" + rec["install_as"])
        os.replace(apath + ".part", apath)
        with zipfile.ZipFile(apath) as z, open(apath, "rb") as f:
            for rec, info in zip(packs, z.infolist()):
                f.seek(info.header_offset)
                h = f.read(30)
                n, e = struct.unpack_from("<HH", h, 26)
                rec["offset"] = info.header_offset + 30 + n + e
                assert info.compress_type == 0 and info.file_size == rec["size"]
        sha, crc = file_digests(apath)
        manifest["archive"] = {"file": args.archive_name, "size": os.path.getsize(apath), "sha256": sha,
                               "folder": "Xonotic/data/"}
    with open(os.path.join(args.outdir, "godnotic-data.json"), "w") as f:
        json.dump(manifest, f, indent=1)
        f.write("\n")

    # ---- sizes
    by = {r["name"]: r for r in packs}
    with open(os.path.join(args.outdir, "split-sizes.md"), "w") as f:
        f.write("| pack | kind | MB | entries | needs |\n| --- | --- | ---: | ---: | --- |\n")
        for r in packs:
            f.write("| %s | %s | %.2f | %d | %s |\n" % (r["name"], r["kind"], r["size"] / 1e6, r["entries"],
                                                       ", ".join(r["depends"])))
        fp = sum(by[p]["size"] for p in first_play)
        total = sum(r["size"] for r in packs)
        f.write("\nfirst play (%s): %.1f MB of %.1f MB\n" % (" + ".join(first_play), fp / 1e6, total / 1e6))
        f.write("\n| map | own MB | with shared MB |\n| --- | ---: | ---: |\n")
        for m in maps:
            own = by["map-" + m]["size"]
            f.write("| %s | %.2f | %.2f |\n" % (m, own / 1e6, (own + sum(by[d]["size"] for d in deps["map-" + m])) / 1e6))
    log("first play %.1f MB of %.1f MB" % (sum(by[p]["size"] for p in first_play) / 1e6,
                                           sum(r["size"] for r in packs) / 1e6))


if __name__ == "__main__":
    main()
