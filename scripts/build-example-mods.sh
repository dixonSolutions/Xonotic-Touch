#!/bin/bash
# Builds every example mod in touch/lua/examples into an installable zip, the
# same way every time (sorted entries, fixed dates and modes), so the archives
# and their sha256 do not change unless a file does:
#
#   build/mods/<name>.zip       client mods  (touch/lua/examples/mods/<name>)
#   build/plugins/<name>.zip    server mods  (touch/lua/examples/plugins/<name>)
#   build/mods/SHA256SUMS, build/plugins/SHA256SUMS
#
#   scripts/build-example-mods.sh [outdir]      (default: build)
#
# Install one with lua_mod_install build/mods/<name>.zip (or the Mods menu).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/build}"
python3 - "$ROOT/touch/lua/examples" "$OUT" <<'PY'
import hashlib, os, sys, zipfile
src, out = sys.argv[1], sys.argv[2]
for kind in ("mods", "plugins"):
    base = os.path.join(src, kind)
    dest = os.path.join(out, kind)
    os.makedirs(dest, exist_ok=True)
    sums = []
    for name in sorted(os.listdir(base)):
        folder = os.path.join(base, name)
        if not os.path.isfile(os.path.join(folder, "init.lua")):
            continue
        path = os.path.join(dest, name + ".zip")
        files = []
        for d, dirs, fs in os.walk(folder):
            dirs.sort()
            for f in sorted(fs):
                full = os.path.join(d, f)
                files.append((name + "/" + os.path.relpath(full, folder).replace(os.sep, "/"), full))
        files.sort()
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
            for arc, full in files:
                info = zipfile.ZipInfo(arc, date_time=(1980, 1, 1, 0, 0, 0))
                info.external_attr = 0o644 << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                with open(full, "rb") as f:
                    z.writestr(info, f.read(), compresslevel=9)
        h = hashlib.sha256(open(path, "rb").read()).hexdigest()
        sums.append(f"{h}  {name}.zip")
        print(f"{kind}/{name}.zip  {h}")
    with open(os.path.join(dest, "SHA256SUMS"), "w") as f:
        f.write("\n".join(sums) + "\n")
PY
