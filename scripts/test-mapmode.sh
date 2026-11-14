#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Create > game mode > map > Play starts that map in that mode, on the native front-end
# path (menu VM hidden), after a menu VM restart under it, and in the QuakeC menu; and the
# map picker lists exactly the maps MapInfo can start (tests/mapmode/mapmode_test.py).
#
#   BASEDIR=<basedir with data/*.pk3 and the Touch xonotic-data.pk3dir> scripts/test-mapmode.sh
# Optional: ENGINE (default build/bin/xonotic), LD_LIBRARY_PATH for libjpeg.so.62,
# MODES ("dm tdm ctf ca duel ft dom ka kh lms"), PATHS ("native native-restart qcmenu"), JOBS (4).
set -euo pipefail
cd "$(dirname "$0")/.."
: "${BASEDIR:?set BASEDIR to a Xonotic basedir (data/*.pk3 plus the Touch pk3dir)}"
exec python3 tests/mapmode/mapmode_test.py
