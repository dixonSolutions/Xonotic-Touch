package io.github.dixonsolutions.godnotic;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.util.Log;

import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * File bridge between the Updates screen (QuakeC describes it; the native interface or the
 * QuakeC menu draws it) and AndySU's payload updater.
 *
 * <p>The menu writes one request token into {@code touch/andysu-update-request.txt}; this class
 * serves it and publishes {@code touch/andysu-update-status.txt} once a second (the format is
 * documented in qcsrc/menu/xonotic/touch_update_util.qh). The decisions live in
 * {@link AndySUState}; this class only gathers what the host, the network and the clock say.
 * Every host call is by reflection and feature-detected: an old frozen host without a method
 * simply does not get the switch that needs it.
 */
final class AndySUBridge {
    private static final String TAG = "Godnotic";

    static final String REQUEST_CHECK = "check";
    static final String REQUEST_APPLY = "apply";
    /** "Download now": fetch the release a check found, on this connection. */
    static final String REQUEST_DOWNLOAD = "download";
    static final String REQUEST_AUTO_ON = "auto-on";
    static final String REQUEST_AUTO_OFF = "auto-off";
    static final String REQUEST_WIFI_ON = "wifi-on";
    static final String REQUEST_WIFI_OFF = "wifi-off";
    static final String REQUEST_LIVE_ON = "live-on";
    static final String REQUEST_LIVE_OFF = "live-off";

    private static final String PREFS = "andysu_updates";
    private static final String PREF_AUTO = "auto";
    private static final String PREF_WIFI_ONLY = "wifi_only";
    private static final String PREF_CHECKED_AT = "checked_at";
    private static final String PREF_CHECKED_RESULT = "checked_result";

    /** A check the player asked for gives its manual mode back after this long at the latest. */
    private static final long MANUAL_SETTLE_MS = 20_000;
    private static final long MANUAL_MAX_MS = 30 * 60_000;

    private final Context context;
    private final File statusFile;
    private final File requestFile;
    private final File blobDir;
    private final SharedPreferences prefs;
    private final AndySUState.Progress progress = new AndySUState.Progress();
    private final boolean listening;

    private String requestError = "";
    private String lastRecord = "";
    private String lastStatusJson = "";
    private String noticeText = "";
    private String channel;
    private int startedActivities;

    /** The mode a player's own check runs in, until it settles; null when none is running. */
    private String manualMode;
    private long manualAt;
    private volatile boolean manualSawStatus;

    AndySUBridge(Context context, File baseDir) {
        this.context = context.getApplicationContext();
        File touch = new File(GameData.engineWriteDir(baseDir), "touch");
        statusFile = new File(touch, "andysu-update-status.txt");
        requestFile = new File(touch, "andysu-update-request.txt");
        // AndySU's root in both of its modes: <data>/no_backup/andysu
        blobDir = new File(new File(new File(this.context.getApplicationInfo().dataDir, "no_backup"), "andysu"),
                "blobs/sha256");
        prefs = this.context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        listening = AndySUCompat.present() && AndySUCompat.setStatusListener(this::onStatus);
        watchForeground();
    }

    String takeRequest() {
        if (!requestFile.isFile()) return null;
        String token = null;
        try {
            List<String> lines = Files.readAllLines(requestFile.toPath(), StandardCharsets.UTF_8);
            if (!lines.isEmpty()) token = lines.get(0).trim();
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "Could not read AndySU update request", e);
        }
        if (!requestFile.delete()) Log.w(TAG, "Could not clear " + requestFile);
        return token == null || token.isEmpty() ? null : token;
    }

    synchronized void serve(String request) {
        try {
            switch (request) {
                case REQUEST_CHECK:
                case REQUEST_DOWNLOAD: {
                    String manual = hasMode() ? AndySUState.manualMode(auto(), wifiOnly(), metered(),
                            REQUEST_DOWNLOAD.equals(request)) : null;
                    if (manual != null) {
                        AndySUCompat.setMode(manual);
                        manualMode = manual;
                        manualAt = System.currentTimeMillis();
                        manualSawStatus = false;
                    }
                    AndySUCompat.checkNow();
                    break;
                }
                case REQUEST_APPLY:
                    AndySUCompat.applyNow();
                    break;
                case REQUEST_AUTO_ON:
                case REQUEST_AUTO_OFF:
                    prefs.edit().putBoolean(PREF_AUTO, REQUEST_AUTO_ON.equals(request)).apply();
                    break;
                case REQUEST_WIFI_ON:
                case REQUEST_WIFI_OFF:
                    prefs.edit().putBoolean(PREF_WIFI_ONLY, REQUEST_WIFI_ON.equals(request)).apply();
                    break;
                case REQUEST_LIVE_ON:
                    AndySUCompat.setApplyPolicy("live");
                    break;
                case REQUEST_LIVE_OFF:
                    AndySUCompat.setApplyPolicy("next_start");
                    break;
                default:
                    return;
            }
            requestError = "";
        } catch (ReflectiveOperationException | RuntimeException e) {
            requestError = "Could not contact the AndySU updater: " + AndySUState.oneLine(e.getMessage());
            Log.w(TAG, requestError, e);
        }
        publish();
    }

    /** Gather, decide, write. Called once a second by the update service thread. */
    synchronized void publish() {
        AndySUState.Record r = new AndySUState.Record();
        r.message = requestError;
        r.installed = AndySUState.oneLine(AndySUCompat.payloadVersion());
        if (!AndySUCompat.present()) {
            r.message = "Payload updates are available only in an AndySU-enabled install.";
            write(r);
            notice(null);
            return;
        }
        try {
            int level = AndySUCompat.apiLevel();
            String statusJson = AndySUCompat.updateStatus();
            if (statusJson == null) statusJson = "{}";
            JSONObject status = new JSONObject(statusJson);
            String state = AndySUState.normalize(status.optString("state", "idle"));
            String release = status.optString("release", status.optString("id", ""));
            String pending = pendingRelease(r.installed);
            // The host's status keeps saying "staged" after that release has committed.
            if ("staged".equals(state) && release.equals(r.installed)) state = "uptodate";
            // After a restart the status starts over at idle; the core still has the staged release.
            if ("idle".equals(state) && !pending.isEmpty()) {
                state = "staged";
                release = pending;
            }
            String staged = "staged".equals(state) ? (release.isEmpty() ? pending : release) : pending;

            if (!listening && !statusJson.equals(lastStatusJson)) onStatus(statusJson);
            lastStatusJson = statusJson;

            boolean modeControl = hasMode();
            boolean metered = metered();
            boolean auto = auto();
            boolean wifiOnly = wifiOnly();
            String mode = modeControl ? AndySUCompat.mode() : "";
            if (modeControl) mode = steerMode(mode, state, auto, wifiOnly, metered, !staged.isEmpty());
            Boolean live = null;
            if (AndySUCompat.has("applyPolicy")) {
                try {
                    live = "live".equals(AndySUCompat.applyPolicy());
                } catch (ReflectiveOperationException | RuntimeException e) {
                    live = null;
                }
            }

            if ("downloading".equals(state)) {
                Map<String, Long> parts = parts();
                if (!progress.started()) progress.start(status.optLong("download_bytes", 0), parts);
                progress.sample(System.currentTimeMillis(), parts, key -> {
                    File blob = new File(blobDir, key);
                    return blob.isFile() ? blob.length() : -1;
                });
            } else {
                progress.reset();
            }

            r.state = state;
            r.latest = "available".equals(state) || "downloading".equals(state) || "shell-update".equals(state)
                    ? release : staged;
            r.percent = progress.percent();
            if (r.message.isEmpty()) {
                r.message = status.optString("message", status.optString("reason", status.optString("code", "")));
            }
            if ("shell-update".equals(state) && r.message.isEmpty()) {
                r.message = "The host APK needs an update before this payload can run.";
            }
            r.auto = modeControl ? auto : "auto".equals(mode);
            r.mode = modeControl ? mode : "unknown";
            r.put("staged", staged)
                    .put("available", "available".equals(state) ? release : "")
                    .flag("auto", modeControl ? auto : null)
                    .flag("wifi", modeControl ? wifiOnly : null)
                    .flag("live", live)
                    .put("net", metered ? "metered" : (Net.kind(context) == Net.Kind.OFFLINE ? "offline" : "unmetered"))
                    .put("hold", AndySUState.hold(state, auto, wifiOnly, metered))
                    .put("api", level)
                    .put("channel", channel())
                    .put("abi", abi());
            long size = status.optLong("download_bytes", 0);
            if (size > 0) r.put("size", size);
            if (progress.started()) {
                r.put("done", progress.done).put("total", progress.total).put("speed", progress.speed)
                        .put("eta", progress.eta);
            }
            long checkedAt = prefs.getLong(PREF_CHECKED_AT, 0);
            if (checkedAt > 0) {
                r.put("checked_min", Math.max(0, (System.currentTimeMillis() - checkedAt) / 60_000))
                        .put("checked_result", prefs.getString(PREF_CHECKED_RESULT, ""));
            }
            notice("downloading".equals(state) ? release : null);
        } catch (Exception e) {
            r.state = "incompatible";
            r.message = "This AndySU host does not expose the payload update API.";
            Log.i(TAG, r.message, e);
            notice(null);
        }
        write(r);
    }

    /**
     * Keep the host in the mode the switches ask for. A check the player started keeps its own
     * mode until the host is done with it (or has had MANUAL_SETTLE_MS to begin). Moving to
     * check_only in the middle of an automatic download stops it: that is what Wi-Fi only asks
     * when the connection turns metered.
     */
    private String steerMode(String mode, String state, boolean auto, boolean wifiOnly, boolean metered,
                             boolean staged) throws ReflectiveOperationException {
        long now = System.currentTimeMillis();
        if (manualMode != null && !AndySUState.busy(state)
                && (manualSawStatus || now - manualAt > MANUAL_SETTLE_MS)) {
            manualMode = null;
        }
        if (manualMode != null && now - manualAt > MANUAL_MAX_MS) manualMode = null;
        String want = manualMode != null ? manualMode
                : AndySUState.targetMode(auto, wifiOnly, metered, staged, foreground());
        if (want.equals(mode)) return mode;
        AndySUCompat.setMode(want);
        Log.i(TAG, "AndySU update mode " + mode + " -> " + want);
        return want;
    }

    /** From the host (main thread) for every status it sets; from publish() when it has no listener. */
    private void onStatus(String statusJson) {
        manualSawStatus = true;
        try {
            String state = AndySUState.normalize(new JSONObject(statusJson).optString("state", ""));
            if (AndySUState.endsCheck(state)) {
                prefs.edit().putLong(PREF_CHECKED_AT, System.currentTimeMillis())
                        .putString(PREF_CHECKED_RESULT, state).apply();
            }
        } catch (Exception e) {
            Log.w(TAG, "Unreadable AndySU status " + statusJson, e);
        }
    }

    private boolean hasMode() {
        return AndySUCompat.apiLevel() >= 3 && AndySUCompat.has("setMode", String.class);
    }

    /** Automatic updates; on first use, whatever the host was set to (an older menu's switch). */
    private boolean auto() {
        if (!prefs.contains(PREF_AUTO)) {
            boolean on = true;
            try {
                String mode = AndySUCompat.mode();
                on = !"off".equals(mode) && !"check_only".equals(mode);
            } catch (ReflectiveOperationException | RuntimeException ignored) {
                // keep the default
            }
            prefs.edit().putBoolean(PREF_AUTO, on).apply();
        }
        return prefs.getBoolean(PREF_AUTO, true);
    }

    /** On by default: like the game data, updates wait for Wi-Fi unless the player says otherwise. */
    private boolean wifiOnly() {
        return prefs.getBoolean(PREF_WIFI_ONLY, true);
    }

    private boolean metered() {
        Net.Kind kind = Net.kind(context);
        return kind != Net.Kind.UNMETERED && kind != Net.Kind.OFFLINE;
    }

    private synchronized boolean foreground() {
        return startedActivities > 0;
    }

    private void watchForeground() {
        if (!(context instanceof Application)) return;
        ((Application) context).registerActivityLifecycleCallbacks(new Application.ActivityLifecycleCallbacks() {
            @Override public void onActivityStarted(Activity a) {
                synchronized (AndySUBridge.this) { startedActivities++; }
            }
            @Override public void onActivityStopped(Activity a) {
                boolean gone;
                synchronized (AndySUBridge.this) {
                    startedActivities = Math.max(0, startedActivities - 1);
                    gone = startedActivities == 0;
                }
                // Leaving the game: settle the background mode now; the poll may not run again.
                if (gone) new Thread(AndySUBridge.this::publish, "godnotic-update-mode").start();
            }
            @Override public void onActivityCreated(Activity a, Bundle b) {}
            @Override public void onActivityResumed(Activity a) {}
            @Override public void onActivityPaused(Activity a) {}
            @Override public void onActivitySaveInstanceState(Activity a, Bundle b) {}
            @Override public void onActivityDestroyed(Activity a) {}
        });
    }

    /** The staged release the core holds, unless it is the one running. */
    private static String pendingRelease(String installed) {
        try {
            String json = AndySUCompat.coreState();
            if (json == null) return "";
            JSONObject state = new JSONObject(json).optJSONObject("ok");
            JSONObject pending = state == null ? null : state.optJSONObject("pending");
            String id = pending == null ? "" : pending.optString("id", "");
            return id.equals(installed) ? "" : id;
        } catch (Exception e) {
            return "";
        }
    }

    private Map<String, Long> parts() {
        Map<String, Long> parts = new HashMap<>();
        File[] files = blobDir.listFiles();
        if (files == null) return parts;
        for (File f : files) {
            String name = f.getName();
            if (name.endsWith(".part")) parts.put(name.substring(0, name.length() - 5), f.length());
        }
        return parts;
    }

    /** The channel this host follows, from its frozen config (assets/andysu/config.json). */
    private String channel() {
        if (channel != null) return channel;
        channel = "";
        try (InputStream in = context.getAssets().open("andysu/config.json")) {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            for (int n; (n = in.read(buf)) > 0 && out.size() < 256 * 1024; ) out.write(buf, 0, n);
            channel = new JSONObject(out.toString("UTF-8")).optString("channel", "");
        } catch (Exception ignored) {
            // not a host build, or no config: no channel to show
        }
        return channel;
    }

    private String abi() {
        String dir = context.getApplicationInfo().nativeLibraryDir;
        String name = dir == null ? "" : new File(dir).getName();
        switch (name) {
            case "arm64": return "arm64-v8a";
            case "arm": return "armeabi-v7a";
            case "x86_64": case "x86": return name;
            default: return android.os.Build.SUPPORTED_ABIS.length > 0 ? android.os.Build.SUPPORTED_ABIS[0] : "";
        }
    }

    /** The system notification follows the download: shown while one runs, gone after. */
    private void notice(String version) {
        String line = AndySUState.progressLine(progress);
        String text = version == null ? "" : version + "\n" + line;
        if (text.equals(noticeText)) return;
        noticeText = text;
        if (version == null) UpdateNotice.cancel(context);
        else UpdateNotice.show(context, version, progress.percent(), progress.total > 0, line);
    }

    private void write(AndySUState.Record r) {
        String record = r.encode();
        if (record.equals(lastRecord) && statusFile.isFile()) return;
        try {
            File dir = statusFile.getParentFile();
            if (dir != null && !dir.isDirectory() && !dir.mkdirs()) return;
            File tmp = new File(statusFile.getPath() + ".tmp");
            Files.write(tmp.toPath(), record.getBytes(StandardCharsets.UTF_8));
            if (!tmp.renameTo(statusFile)) {
                Log.w(TAG, "Could not replace " + statusFile);
                //noinspection ResultOfMethodCallIgnored
                tmp.delete();
            } else {
                lastRecord = record;
            }
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "Could not publish AndySU update status", e);
        }
    }
}
