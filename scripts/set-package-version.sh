#!/bin/bash
# Stamp Flatpak metainfo (and optional files) with a CI package version.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
VERSION="${1:?usage: set-package-version.sh <version>}"
DATE="${PACKAGE_DATE:-$(date -u +%Y-%m-%d)}"
METAINFO="$ROOT/flatpak/io.github.dixonSolutions.XonoticTouch.metainfo.xml"

test -f "$METAINFO"

python3 - "$METAINFO" "$VERSION" "$DATE" <<'PY'
import re
import sys
from pathlib import Path

path, version, date = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text(encoding="utf-8")
entry = f'''    <release version="{version}" date="{date}">
      <description>
        <p>Automated CI build {version}.</p>
      </description>
    </release>
'''
if re.search(r"<releases>\s*</releases>", text):
    text = re.sub(
        r"<releases>\s*</releases>",
        f"<releases>\n{entry}  </releases>",
        text,
        count=1,
    )
elif "<releases>" in text:
    text = text.replace("<releases>", f"<releases>\n{entry}", 1)
else:
    raise SystemExit("metainfo.xml missing <releases> block")
path.write_text(text, encoding="utf-8")
print(f"Stamped metainfo version {version} ({date})")
PY
