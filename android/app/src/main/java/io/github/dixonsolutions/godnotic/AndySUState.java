package io.github.dixonsolutions.godnotic;

import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/**
 * The decisions behind Settings &gt; Updates on an AndySU install, kept free of Android classes so
 * they run as plain JVM unit tests (src/test/java/.../AndySUStateTest.java). {@link AndySUBridge}
 * feeds it what the host and the system report and writes what it returns.
 *
 * <p>What comes from where (host API 3, see docs/ANDROID.md "Updates screen"):
 * <ul>
 * <li>installed version, status, staged release: the host ({@code HostApi.version()},
 *     {@code UpdateClient.status()}, {@code core("state").pending});</li>
 * <li>apply live: the host's persisted apply policy ({@code setApplyPolicy}, live / next_start);</li>
 * <li>automatic updates: the host's persisted update mode ({@code setMode}, level 3);</li>
 * <li>Wi-Fi only: app-side. The host's own network rule is frozen into its config
 *     ({@code network = "any"}), so the app moves the host to {@code check_only} while the
 *     connection is metered ({@link #targetMode});</li>
 * <li>download progress: app-side. The host reports only "downloading" and the byte count
 *     still to fetch; the app watches the {@code .part} files it appends to ({@link Progress}).</li>
 * </ul>
 */
final class AndySUState {
    private AndySUState() {}

    /** Bump only with the menu's TOUCH_ANDYSU_FORMAT. Lines 9 on are key=value extras. */
    static final int FORMAT = 1;

    static final String MODE_AUTO = "auto";
    static final String MODE_DOWNLOAD_ONLY = "download_only";
    static final String MODE_CHECK_ONLY = "check_only";
    static final String MODE_OFF = "off";

    /** The host's states (UpdateClient), spelled the way the menu reads them. */
    static String normalize(String hostState) {
        if (hostState == null || hostState.isEmpty()) return "idle";
        switch (hostState) {
            case "up_to_date": return "uptodate";
            case "shell_update": return "shell-update";
            default: return hostState;
        }
    }

    /** States in which the host is working and a manual mode must not be taken back yet. */
    static boolean busy(String state) {
        return "checking".equals(state) || "downloading".equals(state) || "applying".equals(state);
    }

    /** States that end a check: when one arrives, that is the "last checked" time and result. */
    static boolean endsCheck(String state) {
        switch (state) {
            case "uptodate": case "available": case "staged": case "shell-update":
            case "deferred": case "error":
                return true;
            default:
                return false;
        }
    }

    /**
     * The mode the host should run in, from the player's two switches.
     *
     * <ul>
     * <li>Automatic updates off: {@code check_only}. Background checks still say a release is
     *     there; nothing downloads or applies until the player asks.</li>
     * <li>Automatic, any network: {@code auto}.</li>
     * <li>Automatic, Wi-Fi only, in the foreground: {@code auto} on an unmetered connection,
     *     {@code check_only} on a metered one.</li>
     * <li>Automatic, Wi-Fi only, in the background: the app no longer sees the network, and the
     *     host's periodic job runs on any network, so {@code check_only} — except while a release is
     *     already staged: the host promotes a staged release at the next start only in
     *     {@code auto} mode, and staging is where the download already happened.</li>
     * </ul>
     */
    static String targetMode(boolean auto, boolean wifiOnly, boolean metered, boolean staged,
                             boolean foreground) {
        if (!auto) return MODE_CHECK_ONLY;
        if (!wifiOnly) return MODE_AUTO;
        if (foreground) return metered ? MODE_CHECK_ONLY : MODE_AUTO;
        return staged ? MODE_AUTO : MODE_CHECK_ONLY;
    }

    /**
     * The mode for a check the player asked for, held until it settles. Null: the target mode
     * already does it. {@code download} is "Download now": the player agreed to this connection.
     */
    static String manualMode(boolean auto, boolean wifiOnly, boolean metered, boolean download) {
        if (download) return auto ? MODE_AUTO : MODE_DOWNLOAD_ONLY;
        if (wifiOnly && metered) return null;            // check only; "Download now" offers the rest
        return auto ? null : MODE_DOWNLOAD_ONLY;        // fetch it, never apply it unasked
    }

    /**
     * Why a release the host knows about is not downloading: "wifi" (waiting for an unmetered
     * connection), "auto" (automatic updates are off) or "" (it is, or there is none).
     */
    static String hold(String state, boolean auto, boolean wifiOnly, boolean metered) {
        if (!"available".equals(state)) return "";
        if (!auto) return "auto";
        if (wifiOnly && metered) return "wifi";
        return "";
    }

    /** A record line holds one line of text. */
    static String oneLine(String value) {
        return value == null ? "" : value.replace('\n', ' ').replace('\r', ' ').trim();
    }

    // ------------------------------------------------------------------ record

    /** Everything the menu shows, as written to touch/andysu-update-status.txt. */
    static final class Record {
        String state = "unavailable";
        String installed = "";
        String latest = "";
        int percent;
        String message = "";
        boolean auto;
        String mode = "off";
        /** key=value lines after the eight the first menus read; they ignore what follows. */
        final Map<String, String> extras = new java.util.LinkedHashMap<>();

        Record put(String key, String value) {
            if (value != null && !value.isEmpty()) extras.put(key, oneLine(value));
            return this;
        }

        Record put(String key, long value) {
            extras.put(key, Long.toString(value));
            return this;
        }

        Record flag(String key, Boolean value) {
            extras.put(key, value == null ? "na" : (value ? "on" : "off"));
            return this;
        }

        String encode() {
            StringBuilder b = new StringBuilder();
            b.append(FORMAT).append('\n')
                    .append(oneLine(state)).append('\n')
                    .append(oneLine(installed)).append('\n')
                    .append(oneLine(latest)).append('\n')
                    .append(Math.max(0, Math.min(100, percent))).append('\n')
                    .append(oneLine(message)).append('\n')
                    .append(auto ? "on" : "off").append('\n')
                    .append(oneLine(mode)).append('\n');
            for (Map.Entry<String, String> e : extras.entrySet()) {
                b.append(e.getKey()).append('=').append(e.getValue()).append('\n');
            }
            return b.toString();
        }
    }

    // ------------------------------------------------------------------ progress

    /** Size of a downloaded blob by its key, or -1. */
    interface BlobSize {
        long of(String key);
    }

    /**
     * Download progress from the host's {@code blobs/sha256/<key>.part} files. The host appends to
     * one part file per blob or delta and renames it to the blob when it verifies, so: bytes done =
     * what each part file grew by since the download started, counting a part that disappeared at
     * the size of the blob it became (or the last size seen, for a delta whose result has another
     * key). The total is the host's {@code download_bytes}: what was left to fetch when the check
     * ran.
     */
    static final class Progress {
        private final Map<String, Long> base = new HashMap<>();
        private final Map<String, Long> seen = new HashMap<>();
        private final long[] sampleT = new long[8];
        private final long[] sampleB = new long[8];
        private int samples;
        private boolean started;
        long total;
        long done;
        /** Bytes per second over the last few seconds, 0 before two samples. */
        long speed;
        /** Seconds left, -1 when unknown. */
        long eta = -1;

        void reset() {
            base.clear();
            seen.clear();
            samples = 0;
            started = false;
            total = 0;
            done = 0;
            speed = 0;
            eta = -1;
        }

        boolean started() {
            return started;
        }

        /** The first sample of a download: what is already on disk does not count as progress. */
        void start(long totalBytes, Map<String, Long> parts) {
            reset();
            started = true;
            total = Math.max(0, totalBytes);
            base.putAll(parts);
            seen.putAll(parts);
        }

        void sample(long nowMs, Map<String, Long> parts, BlobSize blobs) {
            if (!started) return;
            for (Map.Entry<String, Long> e : parts.entrySet()) {
                Long was = seen.get(e.getKey());
                seen.put(e.getKey(), was == null ? e.getValue() : Math.max(was, e.getValue()));
                if (!base.containsKey(e.getKey())) base.put(e.getKey(), 0L);
            }
            long sum = 0;
            for (Map.Entry<String, Long> e : seen.entrySet()) {
                String key = e.getKey();
                long now;
                if (parts.containsKey(key)) {
                    now = parts.get(key);
                } else {
                    long blob = blobs == null ? -1 : blobs.of(key);
                    now = Math.max(e.getValue(), blob);
                    seen.put(key, now);
                }
                sum += Math.max(0, now - base.get(key));
            }
            done = total > 0 ? Math.min(sum, total) : sum;
            addSample(nowMs, done);
        }

        private void addSample(long t, long bytes) {
            if (samples == sampleT.length) {
                System.arraycopy(sampleT, 1, sampleT, 0, samples - 1);
                System.arraycopy(sampleB, 1, sampleB, 0, samples - 1);
                samples--;
            }
            sampleT[samples] = t;
            sampleB[samples] = bytes;
            samples++;
            speed = 0;
            eta = -1;
            if (samples < 2) return;
            long dt = sampleT[samples - 1] - sampleT[0];
            long db = sampleB[samples - 1] - sampleB[0];
            if (dt <= 0) return;
            speed = Math.max(0, db * 1000 / dt);
            if (speed > 0 && total > 0) eta = (Math.max(0, total - done) + speed - 1) / speed;
        }

        int percent() {
            return total > 0 ? (int) Math.min(100, done * 100 / total) : 0;
        }
    }

    // ------------------------------------------------------------------ text

    /** "12.3 MB", "850 KB", "3 B": decimal units, as Android's file sizes are. */
    static String bytes(long n) {
        if (n < 1000) return n + " B";
        if (n < 1000 * 1000) return String.format(Locale.ROOT, "%d KB", n / 1000);
        if (n < 1000L * 1000 * 1000) return String.format(Locale.ROOT, "%.1f MB", n / 1e6);
        return String.format(Locale.ROOT, "%.2f GB", n / 1e9);
    }

    /** "45 s", "3 min", "1 h 5 min". */
    static String duration(long seconds) {
        if (seconds < 0) return "";
        if (seconds < 60) return seconds + " s";
        long min = (seconds + 59) / 60;
        if (min < 60) return min + " min";
        return (min / 60) + " h " + (min % 60) + " min";
    }

    /** "12.3 MB of 40.0 MB · 2.1 MB/s · 13 s left", as far as it is known. */
    static String progressLine(Progress p) {
        StringBuilder b = new StringBuilder(bytes(p.done));
        if (p.total > 0) b.append(" of ").append(bytes(p.total));
        if (p.speed > 0) b.append(" · ").append(bytes(p.speed)).append("/s");
        if (p.eta >= 0) b.append(" · ").append(duration(p.eta)).append(" left");
        return b.toString();
    }
}
