package io.github.dixonsolutions.xonotictouch;

/** Names shared by the release workflow and the APK updater. */
final class ReleaseAsset {
    private ReleaseAsset() {}

    static boolean matches(String name, String version, String abi, int sdk, boolean live) {
        if (!live) return name.endsWith(".apk") && name.contains(abi);
        if (sdk < 30) return false;
        String suffix;
        switch (abi) {
            case "arm64-v8a": suffix = "arm64"; break;
            case "armeabi-v7a": suffix = "arm32"; break;
            case "x86_64": suffix = "x64"; break;
            default: return false;
        }
        return name.equals("XonoticTouch-live-" + version + "-" + suffix + ".apk");
    }
}
