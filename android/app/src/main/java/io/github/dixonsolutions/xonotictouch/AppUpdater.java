package io.github.dixonsolutions.xonotictouch;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

/**
 * On-launch update check against the project's GitHub Releases.
 *
 * Sideloaded apps get no store to update them, so the app watches its own
 * release feed. It never installs silently — Android always puts its own
 * confirmation in front of a package install, and that is the right place for
 * the decision to sit.
 *
 * Nothing here touches the game's data directory. An update is an ordinary
 * same-signature upgrade, so saved configs, downloaded maps and screenshots
 * survive it; the downloaded APK itself lives in the cache directory, which the
 * system may evict at will.
 */
final class AppUpdater {

    private static final String TAG = "XonoticTouch";

    /** Only release assets of this repo are ever downloaded or installed. */
    private static final String REPO = "dixonSolutions/Xonotic-Touch";

    private static final String RELEASES_URL =
        "https://api.github.com/repos/" + REPO + "/releases/latest";

    /** Every download URL this repo's release assets can have starts with this. */
    private static final String DOWNLOAD_PREFIX = "https://github.com/" + REPO + "/";

    private static final String PREFS = "updates";
    private static final String PREF_ENABLED = "check_on_launch";
    private static final String PREF_SKIPPED_TAG = "skipped_tag";

    private static final String INSTALL_ACTION =
        "io.github.dixonsolutions.xonotictouch.INSTALL_STATUS";

    /** A release newer than what is installed, with the asset for this device. */
    static final class Update {
        final String version;
        final String url;
        final long size;

        Update(String version, String url, long size) {
            this.version = version;
            this.url = url;
            this.size = size;
        }
    }

    private final Context context;

    AppUpdater(Context context) {
        this.context = context.getApplicationContext();
    }

    // ----------------------------------------------------------------- check

    static boolean isEnabled(Context context) {
        return prefs(context).getBoolean(PREF_ENABLED, true);
    }

    static void setEnabled(Context context, boolean enabled) {
        prefs(context).edit().putBoolean(PREF_ENABLED, enabled).apply();
    }

    void skip(Update update) {
        prefs(context).edit().putString(PREF_SKIPPED_TAG, update.version).apply();
    }

    /**
     * @return the newer release, or null when up to date, skipped, offline, or
     *         the feed has no build for this device's ABI. Never throws: a
     *         failed update check must not stand between the player and the
     *         game.
     */
    Update findUpdate() {
        if (!isEnabled(context)) {
            return null;
        }
        try {
            JSONObject release = new JSONObject(fetch(RELEASES_URL));
            String tag = release.optString("tag_name", "");
            String version = tag.startsWith("v") ? tag.substring(1) : tag;
            if (version.isEmpty() || release.optBoolean("draft") || release.optBoolean("prerelease")) {
                return null;
            }
            if (compareVersions(version, installedVersion()) <= 0) {
                return null;
            }
            if (version.equals(prefs(context).getString(PREF_SKIPPED_TAG, null))) {
                return null;
            }

            JSONArray assets = release.optJSONArray("assets");
            if (assets == null) {
                return null;
            }
            for (String abi : Build.SUPPORTED_ABIS) {
                for (int i = 0; i < assets.length(); i++) {
                    JSONObject asset = assets.getJSONObject(i);
                    String name = asset.optString("name", "");
                    String url = asset.optString("browser_download_url", "");
                    if (!name.endsWith(".apk") || !name.contains(abi)) {
                        continue;
                    }
                    if (!isTrustedApkUrl(url)) {
                        Log.w(TAG, "Ignoring release asset with an untrusted URL: " + url);
                        continue;
                    }
                    return new Update(version, url, asset.optLong("size", -1));
                }
            }
            Log.i(TAG, "Release " + version + " has no build for " + Build.SUPPORTED_ABIS[0]);
        } catch (Exception e) {
            Log.i(TAG, "Update check skipped: " + e);
        }
        return null;
    }

    // --------------------------------------------------------------- install

    /**
     * Downloads and hands the APK to the system installer.
     *
     * @return false when the user has not granted permission to install from
     *         this app; the settings screen for that has been opened.
     */
    boolean install(Update update, GameData.Progress progress, Runnable onFailure)
            throws IOException {
        /*
         * Checked again at the point of use, not only where the URL was picked.
         * These bytes go straight into a PackageInstaller session, so this check is
         * the difference between updating this app and installing an arbitrary APK,
         * and it is cheap enough to not depend on a caller having done it.
         */
        if (!isTrustedApkUrl(update.url)) {
            throw new IOException("refusing to install an untrusted APK URL");
        }
        if (!canInstallPackages()) {
            requestInstallPermission();
            return false;
        }

        PackageInstaller installer = context.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params =
            new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        if (update.size > 0) {
            params.setSize(update.size);
        }

        int sessionId = installer.createSession(params);
        boolean committed = false;
        try (PackageInstaller.Session session = installer.openSession(sessionId)) {
            HttpURLConnection connection = (HttpURLConnection) new URL(update.url).openConnection();
            connection.setConnectTimeout(30_000);
            connection.setReadTimeout(60_000);
            connection.setInstanceFollowRedirects(true);
            try {
                int code = connection.getResponseCode();
                if (code != HttpURLConnection.HTTP_OK) {
                    throw new IOException("HTTP " + code + " downloading " + update.url);
                }
                long total = update.size > 0 ? update.size : connection.getContentLength();
                String status = context.getString(R.string.update_downloading);

                // Stream straight into the session — a 30 MB detour through the
                // cache directory buys nothing.
                try (InputStream in = connection.getInputStream();
                     OutputStream out = session.openWrite("payload", 0, total)) {
                    byte[] buffer = new byte[64 * 1024];
                    long done = 0;
                    long lastReport = 0;
                    int read;
                    while ((read = in.read(buffer)) > 0) {
                        out.write(buffer, 0, read);
                        done += read;
                        if (done - lastReport >= 1024 * 1024) {
                            lastReport = done;
                            progress.update(status, (done >> 20) + " MB",
                                    total > 0 ? (int) (done * 100 / total) : -1);
                        }
                    }
                    session.fsync(out);
                }
            } finally {
                connection.disconnect();
            }

            session.commit(installStatusIntent(onFailure).getIntentSender());
            committed = true;
        } catch (IOException | RuntimeException e) {
            if (!committed) {
                try {
                    installer.abandonSession(sessionId);
                } catch (RuntimeException abandonError) {
                    e.addSuppressed(abandonError);
                }
            }
            throw e;
        }
        return true;
    }

    private boolean canInstallPackages() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return true;
        }
        return context.getPackageManager().canRequestPackageInstalls();
    }

    private void requestInstallPermission() {
        Intent intent = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                Uri.parse("package:" + context.getPackageName()));
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            context.startActivity(intent);
        } catch (Exception e) {
            Log.w(TAG, "No settings screen for install permission", e);
        }
    }

    /**
     * The installer reports back through a broadcast. The reply we care about is
     * STATUS_PENDING_USER_ACTION, which carries the system's confirm dialog for
     * us to launch — without that the session sits there and nothing happens.
     */
    private PendingIntent installStatusIntent(Runnable onFailure) {
        BroadcastReceiver receiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context ctx, Intent intent) {
                int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS,
                        PackageInstaller.STATUS_FAILURE);
                if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                    Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
                    if (confirm != null) {
                        try {
                            confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                            ctx.startActivity(confirm);
                            return;
                        } catch (RuntimeException e) {
                            Log.w(TAG, "Could not open install confirmation", e);
                        }
                    }
                    finish(ctx, true);
                    return;
                }
                if (status != PackageInstaller.STATUS_SUCCESS) {
                    Log.w(TAG, "Install failed (" + status + "): "
                            + intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
                }
                finish(ctx, status != PackageInstaller.STATUS_SUCCESS);
            }

            private void finish(Context ctx, boolean failed) {
                try {
                    ctx.unregisterReceiver(this);
                } catch (IllegalArgumentException ignored) {
                    // Already gone; the process may be restarting for the upgrade.
                }
                if (failed) {
                    onFailure.run();
                }
            }
        };
        IntentFilter filter = new IntentFilter(INSTALL_ACTION);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            context.registerReceiver(receiver, filter);
        }

        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            flags |= PendingIntent.FLAG_MUTABLE;
        }
        Intent intent = new Intent(INSTALL_ACTION).setPackage(context.getPackageName());
        return PendingIntent.getBroadcast(context, 0, intent, flags);
    }

    // ------------------------------------------------------------- utilities

    private static SharedPreferences prefs(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    String installedVersion() {
        try {
            return context.getPackageManager()
                    .getPackageInfo(context.getPackageName(), 0).versionName;
        } catch (PackageManager.NameNotFoundException e) {
            return "0";
        }
    }

    /**
     * True only for an `.apk` release asset served by GitHub for this repo.
     *
     * The URL arrives inside a TLS response from api.github.com, so this is defence
     * in depth rather than the only thing standing in the way — but what it guards
     * is an unattended package install, and the cost of the guard is one string
     * comparison.
     */
    static boolean isTrustedApkUrl(String url) {
        return url != null
                && url.startsWith(DOWNLOAD_PREFIX)
                && url.endsWith(".apk")
                && !url.contains("..");
    }

    /** Numeric dotted-version compare; non-numeric parts sort as 0. */
    static int compareVersions(String a, String b) {
        String[] left = a.split("\\.");
        String[] right = b.split("\\.");
        int parts = Math.max(left.length, right.length);
        for (int i = 0; i < parts; i++) {
            int l = i < left.length ? number(left[i]) : 0;
            int r = i < right.length ? number(right[i]) : 0;
            if (l != r) {
                return l < r ? -1 : 1;
            }
        }
        return 0;
    }

    private static int number(String part) {
        int end = 0;
        while (end < part.length() && Character.isDigit(part.charAt(end))) {
            end++;
        }
        if (end == 0) {
            return 0;
        }
        try {
            return Integer.parseInt(part.substring(0, end));
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    private static String fetch(String url) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(url).openConnection();
        try {
            connection.setRequestProperty("Accept", "application/vnd.github+json");
            connection.setRequestProperty("User-Agent", "XonoticTouch");
            connection.setConnectTimeout(10_000);
            connection.setReadTimeout(15_000);
            int code = connection.getResponseCode();
            if (code != HttpURLConnection.HTTP_OK) {
                throw new IOException("HTTP " + code + " for " + url);
            }
            try (InputStream in = connection.getInputStream()) {
                java.io.ByteArrayOutputStream buffer = new java.io.ByteArrayOutputStream();
                byte[] chunk = new byte[8192];
                int read;
                while ((read = in.read(chunk)) > 0) {
                    buffer.write(chunk, 0, read);
                }
                return buffer.toString(StandardCharsets.UTF_8.name());
            }
        } finally {
            connection.disconnect();
        }
    }
}
