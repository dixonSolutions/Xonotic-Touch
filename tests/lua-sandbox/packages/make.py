#!/usr/bin/env python3
"""Builds the package fixtures for scripts/test-lua-packages.sh into a folder.

Good packages (a mod in two versions, a plugin) and one archive per attack the
installer must refuse (docs/PLUGIN_STANDARD.md, "Packages"). Prints nothing;
the test script knows the names.

  python3 make.py <outdir>
"""
import io
import os
import struct
import sys
import zipfile

out = sys.argv[1]
os.makedirs(out, exist_ok=True)

MOD_V1 = """id: pkgmod
title: Package test mod
version: 1.0
author: Tests
description: Installed by the package tests.
api: 2
realm: client
permissions: events, display
reason.events: draws on the hud event
reason.display: draws a box
setting.size: number 16 8 64 | Text size
"""
MOD_V2 = MOD_V1.replace("version: 1.0", "version: 2.0").replace(
    "permissions: events, display", "permissions: events, display, storage").replace(
    "reason.display: draws a box", "reason.display: draws a box\nreason.storage: remembers the size")
INIT = 'xt.on("hud", function() end)  -- needs events\nprint("PKGMOD loaded api " .. xt.apiversion)\n'


def write(name, entries, method=zipfile.ZIP_DEFLATED):
    """entries: list of (arcname, bytes) or (ZipInfo, bytes)"""
    with zipfile.ZipFile(os.path.join(out, name), "w", method) as z:
        for arc, data in entries:
            if isinstance(arc, zipfile.ZipInfo):
                z.writestr(arc, data)
            else:
                zi = zipfile.ZipInfo(arc)
                zi.compress_type = method
                zi.external_attr = 0o100644 << 16
                zi.create_system = 3
                z.writestr(zi, data)


def raw_name(arc):
    zi = zipfile.ZipInfo("placeholder")
    zi.filename = arc  # keep it exactly: zipfile would otherwise clean it up
    zi.compress_type = zipfile.ZIP_DEFLATED
    return zi


# --- good ---------------------------------------------------------------
write("pkgmod-1.zip", [("pkgmod/mod.txt", MOD_V1), ("pkgmod/init.lua", INIT), ("pkgmod/README", "hi\n")])
write("pkgmod-2.zip", [("pkgmod/mod.txt", MOD_V2), ("pkgmod/init.lua", INIT)])
write("pkgflat.zip", [("mod.txt", MOD_V1.replace("pkgmod", "pkgflat")), ("init.lua", INIT), ("lib/util.lua", "return {}\n")])
os.makedirs(os.path.join(out, "folder", "pkgfolder"), exist_ok=True)
with open(os.path.join(out, "folder", "pkgfolder", "mod.txt"), "w") as f:
    f.write(MOD_V1.replace("pkgmod", "pkgfolder"))
with open(os.path.join(out, "folder", "pkgfolder", "init.lua"), "w") as f:
    f.write(INIT)

# --- attacks ------------------------------------------------------------
base = [("mod.txt", MOD_V1.replace("pkgmod", "evil")), ("init.lua", INIT)]
write("evil-traversal.zip", base + [(raw_name("../evil.lua"), "x")])
write("evil-traversal2.zip", base + [(raw_name("lib/../../evil.lua"), "x")])
write("evil-absolute.zip", base + [(raw_name("/tmp/evil.lua"), "x")])
write("evil-backslash.zip", base + [(raw_name("..\\evil.lua"), "x")])
write("evil-drive.zip", base + [(raw_name("C:/evil.lua"), "x")])
link = zipfile.ZipInfo("link.lua")
link.create_system = 3
link.external_attr = (0o120777 << 16)
write("evil-symlink.zip", base + [(link, "/etc/passwd")])
write("evil-hidden.zip", base + [(".hidden.lua", "x")])
write("evil-type.zip", base + [("payload.so", b"\x7fELF")])
write("evil-data.zip", base + [("data/settings.txt", "x")])
write("evil-dupcase.zip", base + [("Util.lua", "a"), ("util.lua", "b")])
write("evil-big.zip", base + [("big.lua", b"-" * (1024 * 1024 + 10))])
write("evil-total.zip", base + [(f"part{i}.lua", b"-" * (1000 * 1024)) for i in range(9)])
write("evil-many.zip", base + [(f"f{i}.txt", "x") for i in range(300)])
write("evil-noinit.zip", [("a/init.lua", INIT), ("b/init.lua", INIT)])
write("evil-badid.zip", [("mod.txt", "id: ../x\napi: 2\n"), ("init.lua", INIT)])
write("evil-api.zip", [("mod.txt", "id: evilapi\napi: 9\n"), ("init.lua", INIT)])
with open(os.path.join(out, "evil-notzip.zip"), "wb") as f:
    f.write(b"this is not a zip archive at all" * 10)


# a bomb that lies: the directory says 100 bytes, the data inflates to 600 KB
def lying_bomb():
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for arc, data in base:
            z.writestr(arc, data)
        z.writestr("bomb.lua", b"-" * (600 * 1024))
    b = bytearray(buf.getvalue())
    # patch the central directory entry of bomb.lua: uncompressed size 100
    p = b.find(b"PK\x01\x02" , 0)
    while p >= 0:
        nlen = struct.unpack_from("<H", b, p + 28)[0]
        if b[p + 46:p + 46 + nlen] == b"bomb.lua":
            struct.pack_into("<I", b, p + 24, 100)
        p = b.find(b"PK\x01\x02", p + 4)
    return bytes(b)


with open(os.path.join(out, "evil-bomb.zip"), "wb") as f:
    f.write(lying_bomb())


# an encrypted entry (the flag bit in both headers)
def encrypted():
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as z:
        for arc, data in base:
            z.writestr(arc, data)
    b = bytearray(buf.getvalue())
    p = b.find(b"PK\x01\x02")
    struct.pack_into("<H", b, p + 8, struct.unpack_from("<H", b, p + 8)[0] | 1)
    return bytes(b)


with open(os.path.join(out, "evil-encrypted.zip"), "wb") as f:
    f.write(encrypted())

# a plugin package in the client folder: wrong realm
write("evil-realm.zip", [("mod.txt", "id: srvonly\napi: 2\nrealm: server\n"), ("init.lua", INIT)])
