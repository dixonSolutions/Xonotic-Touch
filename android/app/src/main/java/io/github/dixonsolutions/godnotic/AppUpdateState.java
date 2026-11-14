package io.github.dixonsolutions.godnotic;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * The decisions behind Settings &gt; Updates on an install without an AndySU host: the plain APK
 * from a release, a store build, a local test build. There the app's own updater
 * ({@link AppUpdater}: the GitHub release feed and Android's package installer) delivers
 * updates, and {@link UpdateBridge} publishes what it is doing. Plain Java, no Android classes,
 * so it runs as a JVM unit test (src/test/java/.../AppUpdateStateTest.java).
 *
 * <p>The status file keeps its eight lines (touch_update_util.qh); everything the screen needs
 * on top of them comes as key=value lines after the eighth, which older menus never read.
 */
final class AppUpdateState {
    private AppUpdateState() {}

    /** What the app updater follows: the repository's latest full release (never a pre-release). */
    static final String CHANNEL = "stable";

    static final String NET_UNMETERED = "unmetered";
    static final String NET_METERED = "metered";
    static final String NET_OFFLINE = "offline";

    /** Results of a check, as the screen words them (the menu's UIB_UpdChecked). */
    static final String RESULT_UPTODATE = "uptodate";
    static final String RESULT_AVAILABLE = "available";
    static final String RESULT_ERROR = "error";

    /**
     * Whether a download nobody pressed a button for may start on this connection. Offline never;
     * unmetered always; metered (mobile data, a metered Wi-Fi) only with Wi-Fi only switched off.
     * A download the player asked for does not come through here: it goes on any connection.
     */
    static boolean mayDownloadUnasked(boolean wifiOnly, String net) {
        if (NET_OFFLINE.equals(net)) return false;
        if (NET_UNMETERED.equals(net)) return true;
        return !wifiOnly;
    }

    /**
     * Why a release the check found is not downloading: "auto" (automatic updates are off),
     * "wifi" (Wi-Fi only, on a metered connection), "offline", or "" (it is, or nothing waits).
     */
    static String hold(String state, boolean auto, boolean wifiOnly, String net) {
        if (!"available".equals(state) && !"paused".equals(state)) return "";
        if (!auto) return "auto";
        if (NET_OFFLINE.equals(net)) return "offline";
        if (wifiOnly && NET_METERED.equals(net)) return "wifi";
        return "";
    }

    /** How a check ended: it found a release, it found none, or it could not ask. */
    static String checkResult(boolean found, boolean failed) {
        if (failed) return RESULT_ERROR;
        return found ? RESULT_AVAILABLE : RESULT_UPTODATE;
    }

    /** Whole minutes since a check, 0 for one in the future (a clock that moved back). */
    static long minutesSince(long atMs, long nowMs) {
        return Math.max(0, (nowMs - atMs) / 60_000);
    }

    /** The live APK of a release ("Godnotic-live-1.2.205-arm64.apk") brings an AndySU host. */
    static boolean isLiveAsset(String name) {
        return name != null && name.startsWith("Godnotic-live-");
    }

    /** The ABI from the directory Android unpacked the native libraries to (".../lib/arm64"). */
    static String abiOfLibDir(String dirName, String fallback) {
        if (dirName == null) dirName = "";
        switch (dirName) {
            case "arm64": return "arm64-v8a";
            case "arm": return "armeabi-v7a";
            case "x86_64": case "x86": return dirName;
            default: return fallback == null ? "" : fallback;
        }
    }

    /** The lines after the eighth, in a fixed order so equal states write equal files. */
    static final class Extras {
        private final Map<String, String> values = new LinkedHashMap<>();

        Extras put(String key, String value) {
            if (value != null && !value.isEmpty()) {
                values.put(key, value.replace('\n', ' ').replace('\r', ' ').trim());
            }
            return this;
        }

        Extras put(String key, long value) {
            values.put(key, Long.toString(value));
            return this;
        }

        Extras flag(String key, boolean on) {
            values.put(key, on ? "on" : "off");
            return this;
        }

        String get(String key) {
            String v = values.get(key);
            return v == null ? "" : v;
        }

        String encode() {
            StringBuilder b = new StringBuilder();
            for (Map.Entry<String, String> e : values.entrySet()) {
                b.append(e.getKey()).append('=').append(e.getValue()).append('\n');
            }
            return b.toString();
        }
    }

    /**
     * Download speed and time left from the bytes in the update cache, sampled whenever the
     * download reports (every half megabyte or so). Over the last few samples, so a stall shows.
     */
    static final class Rate {
        private final long[] t = new long[8];
        private final long[] b = new long[8];
        private int n;
        long speed;
        long eta = -1;

        void reset() {
            n = 0;
            speed = 0;
            eta = -1;
        }

        void sample(long nowMs, long bytes, long total) {
            if (n > 0 && bytes < b[n - 1]) reset(); // started over
            if (n == t.length) {
                System.arraycopy(t, 1, t, 0, n - 1);
                System.arraycopy(b, 1, b, 0, n - 1);
                n--;
            }
            t[n] = nowMs;
            b[n] = bytes;
            n++;
            speed = 0;
            eta = -1;
            if (n < 2) return;
            long dt = t[n - 1] - t[0];
            long db = b[n - 1] - b[0];
            if (dt <= 0) return;
            speed = Math.max(0, db * 1000 / dt);
            if (speed > 0 && total > 0) eta = (Math.max(0, total - bytes) + speed - 1) / speed;
        }
    }
}
