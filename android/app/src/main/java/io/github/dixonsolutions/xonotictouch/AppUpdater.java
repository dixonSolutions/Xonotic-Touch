package io.github.dixonsolutions.xonotictouch;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.net.ConnectivityManager;
import android.net.NetworkInfo;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
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
 * survive it.
 *
 * The new APK is downloaded into an update cache first ({@link #cacheDir()}),
 * not streamed into the installer. A download that stops -- no signal, the app
 * closed, the phone off -- carries on from the bytes already there the next
 * time (an HTTP Range request), instead of starting the 40 MB over. A finished
 * one is checked against the size and SHA-256 GitHub lists for the asset, and
 * can wait there to be installed the next time the app starts.
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
    /** Whether a release we find installs without stopping to ask first. */
    private static final String PREF_AUTO_INSTALL = "auto_install";
    /**
     * The release the last successful check found.
     *
     * The check that finds an update runs in BootActivity, and the menu's
     * Install and Skip are served from XonoticActivity, which never saw it —
     * so without somewhere durable to put it, both buttons act on nothing on
     * exactly the launches where there is something to act on.
     */
    private static final String PREF_FOUND_VERSION = "found_version";
    private static final String PREF_FOUND_URL = "found_url";
    private static final String PREF_FOUND_SIZE = "found_size";
    private static final String PREF_FOUND_NAME = "found_name";
    private static final String PREF_FOUND_SHA256 = "found_sha256";

    private static final String INSTALL_ACTION =
        "io.github.dixonsolutions.xonotictouch.INSTALL_STATUS";

    private static final class InstallStatusIntent {
        final PendingIntent pendingIntent;
        final BroadcastReceiver receiver;

        InstallStatusIntent(PendingIntent pendingIntent, BroadcastReceiver receiver) {
            this.pendingIntent = pendingIntent;
            this.receiver = receiver;
        }
    }

    /** A release newer than what is installed, with the asset for this device. */
    static final class Update {
        final String version;
        final String url;
        final long size;
        /** The asset's file name: what it is cached as. */
        final String name;
        /** Lowercase hex SHA-256 GitHub lists for the asset, or "" when it does not. */
        final String sha256;

        Update(String version, String url, long size, String name, String sha256) {
            this.version = version;
            this.url = url;
            this.size = size;
            this.name = name;
            this.sha256 = sha256 == null ? "" : sha256;
        }
    }

    private final Context context;

    /** Whether the last {@link #findUpdate()} got an answer out of the feed. */
    private boolean checkFailed;

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

    /**
     * Opt-out, so a player who never opens the update screen still gets fixes.
     *
     * Turning this off does not stop the check -- the in-game Updates screen
     * still has to be able to say how far behind you are -- it stops the
     * install happening without being asked for. Android confirms every
     * package install either way, so even "automatic" is one tap, not none.
     */
    static boolean isAutoInstall(Context context) {
        return prefs(context).getBoolean(PREF_AUTO_INSTALL, true);
    }

    static void setAutoInstall(Context context, boolean enabled) {
        prefs(context).edit().putBoolean(PREF_AUTO_INSTALL, enabled).apply();
    }

    void skip(Update update) {
        prefs(context).edit().putString(PREF_SKIPPED_TAG, update.version).apply();
    }

    /**
     * @return the newer release, or null when up to date, skipped, offline, or
     *         the feed has no build for this device's ABI. Never throws: a
     *         failed update check must not stand between the player and the
     *         game. Use {@link #lastCheckFailed()} to tell the null that means
     *         "nothing to install" from the one that means "could not ask".
     */
    Update findUpdate() {
        checkFailed = true;
        Update found = queryFeed();
        // Only a check that got an answer may overwrite what the last one
        // recorded. An offline retry knows nothing, and must not forget the
        // release an earlier check found for the menu's Install and Skip.
        if (!checkFailed) {
            rememberFound(found);
        }
        return found;
    }

    /**
     * Whether the last {@link #findUpdate()} reached the release feed at all.
     *
     * "There is nothing newer" and "I could not ask" come back as the same
     * null, and a caller that publishes them alike tells the player they are
     * current when nobody knows.
     */
    boolean lastCheckFailed() {
        return checkFailed;
    }

    /**
     * The release the last successful check found, still filtered by what has
     * happened since: an update already installed, or since skipped, is gone.
     */
    Update lastFound() {
        SharedPreferences prefs = prefs(context);
        String version = prefs.getString(PREF_FOUND_VERSION, "");
        String url = prefs.getString(PREF_FOUND_URL, "");
        if (version.isEmpty() || !isTrustedApkUrl(url)) {
            return null;
        }
        if (version.equals(prefs.getString(PREF_SKIPPED_TAG, null))
                || compareVersions(version, installedVersion()) <= 0) {
            return null;
        }
        return new Update(version, url, prefs.getLong(PREF_FOUND_SIZE, -1),
                prefs.getString(PREF_FOUND_NAME, assetName(url)),
                prefs.getString(PREF_FOUND_SHA256, ""));
    }

    private void rememberFound(Update update) {
        SharedPreferences.Editor edit = prefs(context).edit();
        if (update == null) {
            edit.remove(PREF_FOUND_VERSION).remove(PREF_FOUND_URL).remove(PREF_FOUND_SIZE)
                .remove(PREF_FOUND_NAME).remove(PREF_FOUND_SHA256);
        } else {
            edit.putString(PREF_FOUND_VERSION, update.version)
                .putString(PREF_FOUND_URL, update.url)
                .putLong(PREF_FOUND_SIZE, update.size)
                .putString(PREF_FOUND_NAME, update.name)
                .putString(PREF_FOUND_SHA256, update.sha256);
        }
        edit.apply();
    }

    private Update queryFeed() {
        if (!isEnabled(context)) {
            return null;
        }
        try {
            JSONObject release = new JSONObject(fetch(RELEASES_URL));
            // Past this point every null is the feed's answer rather than our
            // failure to get one.
            checkFailed = false;
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
                    // GitHub lists "sha256:<hex>" for assets uploaded since
                    // mid-2025; older ones have no digest and are checked
                    // by size alone.
                    String digest = asset.optString("digest", "");
                    String sha256 = digest.startsWith("sha256:")
                            ? digest.substring(7).toLowerCase(java.util.Locale.ROOT) : "";
                    return new Update(version, url, asset.optLong("size", -1), name, sha256);
                }
            }
            Log.i(TAG, "Release " + version + " has no build for " + Build.SUPPORTED_ABIS[0]);
        } catch (Exception e) {
            // Including one thrown after the fetch: a release we could not
            // finish reading is not a release we can say anything about.
            checkFailed = true;
            Log.i(TAG, "Update check skipped: " + e);
        }
        return null;
    }

    // ----------------------------------------------------------------- cache

    /**
     * Where downloaded updates wait. No-backup storage rather than the cache
     * directory: the system clears the cache under storage pressure, which is
     * exactly when re-downloading 40 MB hurts most, and a backup has no use for
     * a build that will be stale by the time it is restored.
     */
    File cacheDir() {
        return new File(context.getNoBackupFilesDir(), "updates");
    }

    // getName() keeps a name with a path in it inside the cache directory.
    private File cachedApk(Update update) {
        return new File(cacheDir(), new File(update.name).getName());
    }

    private File partFile(Update update) {
        return new File(cacheDir(), new File(update.name).getName() + ".part");
    }

    /** Downloaded and verified: ready to install without touching the network. */
    boolean isReady(Update update) {
        File apk = cachedApk(update);
        return apk.isFile() && (update.size <= 0 || apk.length() == update.size);
    }

    /** Bytes of this update already in the cache, finished or not. */
    long cachedBytes(Update update) {
        if (isReady(update)) {
            return cachedApk(update).length();
        }
        File part = partFile(update);
        return part.isFile() ? part.length() : 0;
    }

    /** How much of the update is cached, 0-100; 0 when the size is unknown. */
    int cachedPercent(Update update) {
        if (update.size <= 0) {
            return isReady(update) ? 100 : 0;
        }
        return (int) Math.min(100, cachedBytes(update) * 100 / update.size);
    }

    /** The release the last check found, if it is downloaded and waiting. */
    Update readyUpdate() {
        Update found = lastFound();
        return (found != null && isReady(found)) ? found : null;
    }

    /**
     * Drop everything in the cache that is not the release the last check
     * found: builds already installed, skipped, or overtaken by a newer one.
     * A half-downloaded copy of the current release stays, to be resumed.
     */
    void cleanCache() {
        File[] files = cacheDir().listFiles();
        if (files == null) {
            return;
        }
        Update keep = lastFound();
        for (File file : files) {
            String name = file.getName();
            if (keep != null && (name.equals(keep.name) || name.equals(keep.name + ".part"))) {
                continue;
            }
            if (!file.delete()) {
                Log.w(TAG, "Could not remove stale update " + file);
            }
        }
    }

    /**
     * Whether a download now would be on Wi-Fi or another connection that is
     * not metered. Background downloads wait for one; a download the player
     * asked for does not.
     */
    static boolean onUnmeteredNetwork(Context context) {
        ConnectivityManager connectivity =
                (ConnectivityManager) context.getSystemService(Context.CONNECTIVITY_SERVICE);
        if (connectivity == null) {
            return false;
        }
        NetworkInfo active = connectivity.getActiveNetworkInfo();
        return active != null && active.isConnected() && !connectivity.isActiveNetworkMetered();
    }

    /**
     * Download the update into the cache, carrying on from whatever an earlier
     * attempt left there. Returns once the APK is complete and verified.
     *
     * Interrupting the calling thread stops it with an InterruptedIOException
     * and keeps the bytes so far.
     */
    void download(Update update, GameData.Progress progress) throws IOException {
        if (!isTrustedApkUrl(update.url)) {
            throw new IOException("refusing to download an untrusted APK URL");
        }
        if (isReady(update)) {
            return;
        }
        File dir = cacheDir();
        if (!dir.isDirectory() && !dir.mkdirs()) {
            throw new IOException("cannot create " + dir);
        }
        File part = partFile(update);
        String status = context.getString(R.string.update_downloading);

        for (int attempt = 0; attempt < 2; attempt++) {
            long have = part.isFile() ? part.length() : 0;
            if (update.size > 0 && have > update.size) {
                // More than the asset holds: not this asset. Start over.
                deleteQuietly(part);
                have = 0;
            }
            HttpURLConnection connection = (HttpURLConnection) new URL(update.url).openConnection();
            connection.setConnectTimeout(30_000);
            connection.setReadTimeout(60_000);
            connection.setInstanceFollowRedirects(true);
            connection.setRequestProperty("User-Agent", "XonoticTouch");
            if (have > 0) {
                connection.setRequestProperty("Range", "bytes=" + have + "-");
            }
            try {
                int code = connection.getResponseCode();
                boolean append;
                if (code == HttpURLConnection.HTTP_PARTIAL && have > 0) {
                    append = true;
                } else if (code == HttpURLConnection.HTTP_OK) {
                    // The server ignored the range: what we have is no use.
                    append = false;
                    have = 0;
                } else if (code == 416 && have > 0) {
                    // Nothing past what we have. Either it is all there, or the
                    // file changed under us; the checks below decide which.
                    break;
                } else {
                    throw new IOException("HTTP " + code + " downloading " + update.url);
                }
                long total = update.size > 0 ? update.size
                        : (connection.getContentLength() > 0 ? have + connection.getContentLength() : -1);
                try (InputStream in = connection.getInputStream();
                     OutputStream out = new FileOutputStream(part, append)) {
                    byte[] buffer = new byte[64 * 1024];
                    long done = have;
                    long lastReport = -1;
                    int read;
                    while ((read = in.read(buffer)) > 0) {
                        if (Thread.currentThread().isInterrupted()) {
                            throw new InterruptedIOException("update download stopped");
                        }
                        out.write(buffer, 0, read);
                        done += read;
                        if (lastReport < 0 || done - lastReport >= 512 * 1024) {
                            lastReport = done;
                            progress.update(status, (done >> 20) + " / " + (total > 0 ? (total >> 20) : "?") + " MB",
                                    total > 0 ? (int) (done * 100 / total) : -1);
                        }
                    }
                }
                break;
            } finally {
                connection.disconnect();
            }
        }

        if (update.size > 0 && part.length() != update.size) {
            if (part.length() > update.size) {
                deleteQuietly(part);
            }
            throw new IOException("update download incomplete (" + part.length() + " of " + update.size + " bytes)");
        }
        if (!update.sha256.isEmpty()) {
            String actual = sha256(part);
            if (!update.sha256.equals(actual)) {
                // Corrupt or tampered with: never hand it to the installer, and
                // do not resume from it either.
                deleteQuietly(part);
                throw new IOException("update download failed its checksum; it will start over");
            }
        }
        File apk = cachedApk(update);
        deleteQuietly(apk);
        if (!part.renameTo(apk)) {
            throw new IOException("cannot finish " + apk);
        }
        progress.update(status, (apk.length() >> 20) + " MB", 100);
    }

    // --------------------------------------------------------------- install

    /**
     * Downloads (or resumes downloading) the update and hands it to the system
     * installer.
     *
     * @return false when the user has not granted permission to install from
     *         this app; the settings screen for that has been opened.
     */
    boolean install(Update update, GameData.Progress progress, Runnable onFailure)
            throws IOException {
        download(update, progress);
        return installCached(update, onFailure);
    }

    /**
     * Hands an already downloaded update to the system installer.
     *
     * @return false when the user has not granted permission to install from
     *         this app; the settings screen for that has been opened.
     */
    boolean installCached(Update update, Runnable onFailure) throws IOException {
        /*
         * Checked again at the point of use, not only where the URL was picked.
         * These bytes go into a PackageInstaller session, so this check is the
         * difference between updating this app and installing an arbitrary APK,
         * and it is cheap enough to not depend on a caller having done it.
         */
        if (!isTrustedApkUrl(update.url)) {
            throw new IOException("refusing to install an untrusted APK URL");
        }
        if (!isReady(update)) {
            throw new IOException("the update is not downloaded yet");
        }
        if (!canInstallPackages()) {
            requestInstallPermission();
            return false;
        }

        File apk = cachedApk(update);
        PackageInstaller installer = context.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params =
            new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setSize(apk.length());

        // Android 12+ lets the app that installed a package update it without
        // the confirm dialog, once it is that package's installer of record.
        // The first in-app update still asks (a sideload's installer is the
        // file manager or browser); every one after it just happens.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            params.setRequireUserAction(
                PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED);
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            params.setInstallReason(PackageManager.INSTALL_REASON_USER);
        }

        int sessionId = installer.createSession(params);
        boolean committed = false;
        InstallStatusIntent statusIntent = null;
        try (PackageInstaller.Session session = installer.openSession(sessionId)) {
            try (InputStream in = new FileInputStream(apk);
                 OutputStream out = session.openWrite("payload", 0, apk.length())) {
                byte[] buffer = new byte[64 * 1024];
                int read;
                while ((read = in.read(buffer)) > 0) {
                    out.write(buffer, 0, read);
                }
                session.fsync(out);
            }

            statusIntent = installStatusIntent(onFailure);
            session.commit(statusIntent.pendingIntent.getIntentSender());
            committed = true;
        } catch (IOException | RuntimeException e) {
            if (!committed) {
                if (statusIntent != null) {
                    unregisterInstallReceiver(statusIntent.receiver);
                }
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
    private InstallStatusIntent installStatusIntent(Runnable onFailure) {
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
                    finish(true);
                    return;
                }
                if (status != PackageInstaller.STATUS_SUCCESS) {
                    Log.w(TAG, "Install failed (" + status + "): "
                            + intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
                }
                finish(status != PackageInstaller.STATUS_SUCCESS);
            }

            private void finish(boolean failed) {
                unregisterInstallReceiver(this);
                if (failed) {
                    onFailure.run();
                }
            }
        };

        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            flags |= PendingIntent.FLAG_MUTABLE;
        }
        Intent intent = new Intent(INSTALL_ACTION).setPackage(context.getPackageName());
        PendingIntent pendingIntent = PendingIntent.getBroadcast(context, 0, intent, flags);

        IntentFilter filter = new IntentFilter(INSTALL_ACTION);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            context.registerReceiver(receiver, filter);
        }
        return new InstallStatusIntent(pendingIntent, receiver);
    }

    private void unregisterInstallReceiver(BroadcastReceiver receiver) {
        try {
            context.unregisterReceiver(receiver);
        } catch (IllegalArgumentException ignored) {
            // Already gone; the process may be restarting for the upgrade.
        }
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

    private static String sha256(File file) throws IOException {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            try (InputStream in = new FileInputStream(file)) {
                byte[] buffer = new byte[64 * 1024];
                int read;
                while ((read = in.read(buffer)) > 0) {
                    digest.update(buffer, 0, read);
                }
            }
            StringBuilder hex = new StringBuilder();
            for (byte b : digest.digest()) {
                hex.append(String.format(java.util.Locale.ROOT, "%02x", b));
            }
            return hex.toString();
        } catch (NoSuchAlgorithmException e) {
            throw new IOException("no SHA-256 on this device", e);
        }
    }

    private static void deleteQuietly(File file) {
        if (file.exists() && !file.delete()) {
            Log.w(TAG, "Could not delete " + file);
        }
    }

    /** The file name at the end of a release asset URL. */
    static String assetName(String url) {
        int slash = url == null ? -1 : url.lastIndexOf('/');
        return slash >= 0 ? url.substring(slash + 1) : "update.apk";
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
