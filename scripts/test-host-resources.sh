#!/bin/bash
# Under AndySU a live update replaces the dex and assets but not resources.arsc:
# the installed host keeps the table of the release it was built from. aapt
# numbers resources by name, so R.* ids compiled into a newer dex point at other
# entries of an older host's table (1.2.151-1.2.155 crashed on start and rolled
# back). App code goes through HostRes and builds its views in code instead.
set -euo pipefail
cd "$(dirname "$0")/.."
src=android/app/src/main/java/io/github/dixonsolutions/xonotictouch
kt=android/app/src/main/kotlin/io/github/dixonsolutions/xonotictouch
bad=$(grep -rnE '(^|[^.[:alnum:]_])R\.[a-z]+\.[A-Za-z_]' "$src" "$kt" --include='*.java' --include='*.kt' | grep -v '^[^:]*HostRes\.java:' || true)
if [ -n "$bad" ]; then
    echo "App code uses R.* ids, which point into the installed host's resource table:" >&2
    echo "$bad" >&2
    echo "Use HostRes.string(context, \"name\") and build views in code." >&2
    exit 1
fi
test ! -e android/app/src/main/res/layout || {
    echo "res/layout is back; under AndySU layouts come from the host's table." >&2; exit 1; }
grep -q "assets.srcDirs += \[\"\$buildDir/generated/xt-strings\"\]" android/app/build.gradle
echo "host resources: ok"
