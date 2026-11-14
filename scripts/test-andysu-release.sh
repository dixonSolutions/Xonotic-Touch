#!/usr/bin/env bash
# No network, secrets, Android SDK or build required.
set -euo pipefail
cd "$(dirname "$0")/.."
python3 - <<'PY'
import pathlib, re
workflow = pathlib.Path('.github/workflows/build-and-publish.yml').read_text()
source = pathlib.Path('android/app/src/main/java/io/github/dixonsolutions/godnotic/ReleaseAsset.java').read_text()
pairs = re.findall(r'case "([^"]+)": suffix = "([^"]+)"', source)
assert len(pairs) == 3
for abi, suffix in pairs:
    assert f'- abi: {abi}\n            suffix: {suffix}' in workflow
    name = f'Godnotic-live-1.2.123-{suffix}.apk'
    for old_abi, _ in pairs:
        assert old_abi not in name, (old_abi, name)
assert 'Godnotic-live-$VERSION-$SUFFIX.apk' in workflow
assert 'sdk < 30' in source
print('Live asset names do not match any old-client ABI string')
PY
if command -v javac >/dev/null && command -v java >/dev/null; then
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT
  mkdir -p "$tmp/io/github/dixonsolutions/godnotic"
  cat > "$tmp/io/github/dixonsolutions/godnotic/ReleaseAssetTest.java" <<'JAVA'
package io.github.dixonsolutions.godnotic;
class ReleaseAssetTest {
    static void check(boolean value) { if (!value) throw new AssertionError(); }
    public static void main(String[] args) {
        String[][] abis = {{"arm64-v8a", "arm64"}, {"armeabi-v7a", "arm32"}, {"x86_64", "x64"}};
        for (String[] a : abis) {
            String live = "Godnotic-live-1.2.3-" + a[1] + ".apk";
            String plain = "Godnotic-1.2.3-" + a[0] + ".apk";
            check(ReleaseAsset.matches(live, "1.2.3", a[0], 30, true));
            check(!ReleaseAsset.matches(live, "1.2.3", a[0], 29, true));
            check(!ReleaseAsset.matches(live, "1.2.3", a[0], 30, false));
            check(ReleaseAsset.matches(plain, "1.2.3", a[0], 29, false));
            check(!ReleaseAsset.matches(plain, "1.2.3", a[0], 30, true));
            for (String[] b : abis) if (a != b)
                check(!ReleaseAsset.matches(live, "1.2.3", b[0], 30, true));
        }
        check(!ReleaseAsset.matches("Godnotic-live-1.2.3-x64.apk", "1.2.3", "x86", 30, true));
    }
}
JAVA
  javac -d "$tmp" android/app/src/main/java/io/github/dixonsolutions/godnotic/ReleaseAsset.java "$tmp/io/github/dixonsolutions/godnotic/ReleaseAssetTest.java"
  java -cp "$tmp" io.github.dixonsolutions.godnotic.ReleaseAssetTest
else
  echo 'Java unavailable: selection execution skipped; naming rules checked'
fi
if command -v andysu >/dev/null; then
  andysu remote --toml andysu.toml >/dev/null
else
  echo 'AndySU CLI unavailable: CLI config check skipped'
fi
