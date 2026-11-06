package io.github.dixonsolutions.xonotictouch;

import android.content.Context;
import android.util.Log;

import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;

/** File bridge between the QuakeC Updates screen and AndySU's payload updater. */
final class AndySUBridge {
    private static final String TAG = "XonoticTouch";
    private static final int FORMAT = 1;

    static final String REQUEST_CHECK = "check";
    static final String REQUEST_APPLY = "apply";
    static final String REQUEST_AUTO_ON = "auto-on";
    static final String REQUEST_AUTO_OFF = "auto-off";

    private final File statusFile;
    private final File requestFile;
    private String requestError = "";
    private String lastRecord = "";

    AndySUBridge(Context context, File baseDir) {
        File touch = new File(GameData.engineWriteDir(baseDir), "touch");
        statusFile = new File(touch, "andysu-update-status.txt");
        requestFile = new File(touch, "andysu-update-request.txt");
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

    void serve(String request) {
        try {
            switch (request) {
                case REQUEST_CHECK:
                    AndySUCompat.checkNow();
                    requestError = "";
                    break;
                case REQUEST_APPLY:
                    AndySUCompat.applyNow();
                    requestError = "";
                    break;
                case REQUEST_AUTO_ON:
                    AndySUCompat.setMode("auto");
                    requestError = "";
                    break;
                case REQUEST_AUTO_OFF:
                    AndySUCompat.setMode("off");
                    requestError = "";
                    break;
                default:
                    return;
            }
        } catch (ReflectiveOperationException | RuntimeException e) {
            requestError = "Could not contact the AndySU updater: " + oneLine(e.getMessage());
            Log.w(TAG, requestError, e);
        }
        publish();
    }

    void publish() {
        String state = "unavailable";
        String installed = nullSafe(AndySUCompat.payloadVersion());
        String latest = "";
        String message = requestError;
        String mode = "off";
        int percent = 0; // AndySU reports state transitions, not byte percentages.

        if (AndySUCompat.present()) {
            try {
                mode = AndySUCompat.mode();
                JSONObject status = new JSONObject(AndySUCompat.updateStatus());
                state = normalize(status.optString("state", "idle"));
                latest = status.optString("release", status.optString("id", ""));
                // The host's status keeps saying "staged" after that release has committed.
                if ("staged".equals(state) && latest.equals(installed)) state = "uptodate";
                if (message.isEmpty()) {
                    message = status.optString("message",
                            status.optString("reason", status.optString("code", "")));
                }
                if ("shell-update".equals(state)) {
                    message = message.isEmpty()
                            ? "The host APK needs an update before this payload can run."
                            : message;
                }
            } catch (Exception e) {
                state = "incompatible";
                message = "This AndySU host does not expose the payload update API.";
                Log.i(TAG, message, e);
            }
        } else {
            message = "Payload updates are available only in an AndySU-enabled install.";
        }

        String record = FORMAT + "\n" + state + "\n" + installed + "\n" + nullSafe(latest)
                + "\n" + percent + "\n" + oneLine(message) + "\n"
                + ("auto".equals(mode) ? "on" : "off") + "\n" + mode + "\n";
        if (record.equals(lastRecord) && statusFile.isFile()) {
            return;
        }
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

    private static String normalize(String state) {
        if ("up_to_date".equals(state)) return "uptodate";
        if ("shell_update".equals(state)) return "shell-update";
        return state;
    }

    private static String nullSafe(String value) {
        return value == null ? "" : oneLine(value.trim());
    }

    private static String oneLine(String value) {
        return value == null ? "" : value.replace('\n', ' ').replace('\r', ' ');
    }
}
