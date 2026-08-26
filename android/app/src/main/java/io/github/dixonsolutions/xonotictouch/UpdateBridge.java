package io.github.dixonsolutions.xonotictouch;

import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;

/**
 * The menu's half of the updater.
 *
 * {@link AppUpdater} can only ask its question once, at boot, in a dialog that
 * is gone before the engine starts. Everything a player wants afterwards — what
 * build am I on, how far behind, check again, stop asking — has to be reachable
 * from inside the game, and the game's menu is QuakeC: no JNI, no way to call
 * into any of this.
 *
 * So the two halves talk through two files in the gamedir, which is the same
 * handshake the asset downloader already uses and for the same reason. This
 * class owns {@code update-status.txt} and only writes it; the menu owns
 * {@code update-request.txt} and only writes that. Neither reads its own file
 * back, so there is no shared state to race over — a torn read costs one stale
 * second and the menu re-reads every second anyway.
 *
 * The file format is documented once, in
 * {@code qcsrc/menu/xonotic/touch_update_util.qh}. Keep the two in step.
 */
final class UpdateBridge {

    private static final String TAG = "XonoticTouch";

    /** Bump only alongside the menu's TOUCH_UPDATE_FORMAT. */
    private static final int FORMAT = 1;

    static final String STATE_IDLE = "idle";
    static final String STATE_CHECKING = "checking";
    static final String STATE_UPTODATE = "uptodate";
    static final String STATE_AVAILABLE = "available";
    static final String STATE_DOWNLOADING = "downloading";
    static final String STATE_INSTALLING = "installing";
    static final String STATE_NEEDS_PERMISSION = "needs-permission";
    static final String STATE_ERROR = "error";

    private final Context context;
    private final File statusFile;
    private final File requestFile;

    UpdateBridge(Context context, File baseDir) {
        this.context = context.getApplicationContext();
        // Where the menu's fopen() actually reaches, which is not where its
        // path reads as -- see GameData.engineWriteDir. Under the basedir the
        // status file was still found, by a lower-priority read fallback, so
        // the screen showed a version while every button on it wrote a request
        // into a directory nothing was watching.
        File touch = new File(GameData.engineWriteDir(baseDir), "touch");
        this.statusFile = new File(touch, "update-status.txt");
        this.requestFile = new File(touch, "update-request.txt");
    }

    /** A request the menu left for us, or null. Consumed: read once, then gone. */
    String takeRequest() {
        if (!requestFile.isFile()) {
            return null;
        }
        String token = null;
        try {
            List<String> lines = Files.readAllLines(requestFile.toPath(), StandardCharsets.UTF_8);
            if (!lines.isEmpty()) {
                token = lines.get(0).trim();
            }
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "Could not read update request", e);
        }
        // Delete even on a failed read: a request we cannot parse would
        // otherwise be retried on every poll for the life of the install.
        if (!requestFile.delete()) {
            Log.w(TAG, "Could not clear " + requestFile);
        }
        return (token == null || token.isEmpty()) ? null : token;
    }

    void publishIdle(String installed) {
        publish(STATE_IDLE, installed, "", 0, 0, "");
    }

    void publishChecking(String installed) {
        publish(STATE_CHECKING, installed, "", 0, 0, "");
    }

    void publishUpToDate(String installed) {
        publish(STATE_UPTODATE, installed, installed, 0, 0,
                context.getString(R.string.update_status_current));
    }

    /**
     * A check that could not reach the release feed.
     *
     * Deliberately not {@link #publishUpToDate}: both come back from
     * {@link AppUpdater#findUpdate()} as the same null, and publishing them
     * alike tells a player on a flaky connection that they are current. Any
     * release an earlier check found is kept, so one bad check does not take
     * away the install it was offering.
     */
    void publishCheckFailed(String installed, AppUpdater.Update known) {
        String message = context.getString(R.string.update_status_check_failed);
        if (known == null) {
            publish(STATE_ERROR, installed, "", 0, 0, message);
            return;
        }
        publish(STATE_AVAILABLE, installed, known.version,
                versionsBehind(installed, known.version), 0, message);
    }

    void publishAvailable(String installed, AppUpdater.Update update) {
        publish(STATE_AVAILABLE, installed, update.version,
                versionsBehind(installed, update.version), 0,
                context.getString(R.string.update_status_available, update.version));
    }

    void publishDownloading(String installed, String latest, int percent) {
        publish(STATE_DOWNLOADING, installed, latest,
                versionsBehind(installed, latest), percent, "");
    }

    void publishInstalling(String installed, String latest) {
        publish(STATE_INSTALLING, installed, latest,
                versionsBehind(installed, latest), 100,
                context.getString(R.string.update_status_installing));
    }

    void publishNeedsPermission(String installed, String latest) {
        publish(STATE_NEEDS_PERMISSION, installed, latest,
                versionsBehind(installed, latest), 100,
                context.getString(R.string.update_status_permission));
    }

    void publishError(String installed, String message) {
        publish(STATE_ERROR, installed, "", 0, 0, message);
    }

    /**
     * How many releases behind the installed build is.
     *
     * Tags are {@code X.Y.Z} and only the patch component moves between builds,
     * so the patch delta is the release count. Anything that does not fit that
     * shape — a locally built APK, a dirty version string — falls back to 1:
     * "there is an update" is still true, and a wrong count is worse than a
     * vague one.
     */
    static int versionsBehind(String installed, String latest) {
        String[] a = installed.split("\\.");
        String[] b = latest.split("\\.");
        if (a.length != 3 || b.length != 3) {
            return AppUpdater.compareVersions(latest, installed) > 0 ? 1 : 0;
        }
        try {
            if (!a[0].equals(b[0]) || !a[1].equals(b[1])) {
                return AppUpdater.compareVersions(latest, installed) > 0 ? 1 : 0;
            }
            return Math.max(0, Integer.parseInt(b[2].trim()) - Integer.parseInt(a[2].trim()));
        } catch (NumberFormatException e) {
            return AppUpdater.compareVersions(latest, installed) > 0 ? 1 : 0;
        }
    }

    private void publish(String state, String installed, String latest,
                         int behind, int percent, String message) {
        StringBuilder out = new StringBuilder()
                .append(FORMAT).append('\n')
                .append(state).append('\n')
                .append(nullSafe(installed)).append('\n')
                .append(nullSafe(latest)).append('\n')
                .append(Math.max(0, behind)).append('\n')
                .append(Math.max(0, Math.min(100, percent))).append('\n')
                .append(oneLine(message)).append('\n')
                .append(AppUpdater.isAutoInstall(context) ? "on" : "off").append('\n');

        try {
            File dir = statusFile.getParentFile();
            if (dir != null && !dir.isDirectory() && !dir.mkdirs()) {
                Log.w(TAG, "Could not create " + dir);
                return;
            }
            // Write and rename: the menu polls this file on its own clock, and a
            // half-written one would be read as a truncated record rather than
            // simply being missed.
            File tmp = new File(statusFile.getPath() + ".tmp");
            Files.write(tmp.toPath(), out.toString().getBytes(StandardCharsets.UTF_8));
            if (!tmp.renameTo(statusFile)) {
                Log.w(TAG, "Could not replace " + statusFile);
                //noinspection ResultOfMethodCallIgnored
                tmp.delete();
            }
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "Could not publish update status", e);
        }
    }

    private static String nullSafe(String s) {
        return s == null ? "" : s.trim();
    }

    /** The format is line-based, so a newline in a message would shift every field after it. */
    private static String oneLine(String s) {
        return nullSafe(s).replace('\n', ' ').replace('\r', ' ');
    }
}
