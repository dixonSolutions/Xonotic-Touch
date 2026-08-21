package io.github.dixonsolutions.xonotictouch;

import android.content.Context;
import android.util.Base64;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Owns the engine's basedir on Android.
 *
 * The APK carries only the slim data set — game logic, configs and the menu skin
 * — for the same reason the Click package does: the full asset set is about a
 * gigabyte and has no business inside an app-store download. Everything else
 * comes from the Xonotic autobuild server on first launch, which is the same
 * source scripts/fetch-assets-posix.sh uses on Ubuntu Touch.
 */
final class GameData {

    private static final String TAG = "XonoticTouch";

    /** Bundled payload staged into the APK by scripts/android-stage-assets.sh. */
    private static final String BUNDLE_ASSET = "xonotic-slim-data.zip";
    private static final String BUNDLE_STAMP = ".bundle-version";

    private static final String AUTOBUILD = "https://beta.xonotic.org/autobuild";
    private static final String AUTOBUILD_LOGIN = "xonotic:g-23";

    /** zip name -> the pk3 suffix it satisfies. */
    private static final String[][] DOWNLOADS = {
        {"Xonotic-latest.zip", "-data.pk3"},
        {"Xonotic-latest-mappingsupport.zip", "-maps.pk3"},
        {"Xonotic-latest-high.zip", "-music.pk3"},
    };

    interface Progress {
        /** @param percent 0-100, or -1 when the total size is unknown. */
        void update(String status, String detail, int percent);
    }

    private final Context context;

    GameData(Context context) {
        this.context = context;
    }

    /** The directory passed to the engine as {@code -basedir}. */
    File baseDir() {
        File external = context.getExternalFilesDir(null);
        // getExternalFilesDir() returns null while external storage is being
        // ejected; internal storage still gives us a working, private basedir.
        return external != null ? external : context.getFilesDir();
    }

    File dataDir() {
        return new File(baseDir(), "data");
    }

    void prepare(Progress progress) throws IOException {
        File base = baseDir();
        if (!base.isDirectory() && !base.mkdirs()) {
            throw new IOException("Cannot create " + base);
        }
        installBundle(progress);
        downloadAssets(progress);
    }

    // ---------------------------------------------------------------- bundle

    /**
     * Unpacks the APK's slim data set. Re-runs after an app update so a new
     * build's QuakeC progs never sit next to the previous build's configs.
     */
    private void installBundle(Progress progress) throws IOException {
        String version = appVersion();
        File stamp = new File(baseDir(), BUNDLE_STAMP);
        if (stamp.isFile() && version.equals(readAll(stamp))) {
            return;
        }

        String status = context.getString(R.string.boot_unpacking);
        progress.update(status, "", -1);
        Log.i(TAG, "Unpacking " + BUNDLE_ASSET + " for " + version);

        try (InputStream raw = context.getAssets().open(BUNDLE_ASSET)) {
            unzip(new ZipInputStream(raw), baseDir(), null, progress, status);
        }

        writeAll(stamp, version);
    }

    // -------------------------------------------------------------- download

    private void downloadAssets(Progress progress) throws IOException {
        File data = dataDir();
        if (!data.isDirectory() && !data.mkdirs()) {
            throw new IOException("Cannot create " + data);
        }

        String status = context.getString(R.string.boot_downloading);
        for (String[] download : DOWNLOADS) {
            if (hasPk3(data, download[1])) {
                continue;
            }
            File zip = new File(context.getCacheDir(), download[0]);
            try {
                fetch(AUTOBUILD + "/" + download[0], zip, progress, status, download[0]);
                progress.update(status, "Installing " + download[0], -1);
                try (InputStream raw = new FileInputStream(zip)) {
                    // The archive nests everything under Xonotic/; only the pk3
                    // payloads matter, and they belong in <basedir>/data.
                    unzip(new ZipInputStream(raw), data, "Xonotic/data/", progress, status);
                }
            } finally {
                // Never leave a partial archive behind: the next launch would
                // treat a truncated file as a complete one.
                if (!zip.delete() && zip.exists()) {
                    Log.w(TAG, "Could not delete " + zip);
                }
            }
        }
    }

    private void fetch(String url, File target, Progress progress, String status, String name)
            throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(url).openConnection();
        try {
            connection.setRequestProperty("Authorization", autobuildAuthHeader());
            connection.setConnectTimeout(30_000);
            connection.setReadTimeout(60_000);
            connection.setInstanceFollowRedirects(true);

            int code = connection.getResponseCode();
            if (code != HttpURLConnection.HTTP_OK) {
                throw new IOException("HTTP " + code + " for " + url);
            }
            long total = connection.getContentLength();

            try (InputStream in = connection.getInputStream();
                 OutputStream out = new FileOutputStream(target)) {
                byte[] buffer = new byte[64 * 1024];
                long done = 0;
                long lastReport = 0;
                int read;
                while ((read = in.read(buffer)) > 0) {
                    out.write(buffer, 0, read);
                    done += read;
                    // One UI post per megabyte; per-chunk posts flood the looper.
                    if (done - lastReport >= 1024 * 1024) {
                        lastReport = done;
                        progress.update(status, name + " — " + (done >> 20) + " MB",
                                total > 0 ? (int) (done * 100 / total) : -1);
                    }
                }
            }
        } finally {
            connection.disconnect();
        }
    }

    /**
     * The autobuild mirror sits behind a shared HTTP login that Xonotic
     * publishes alongside the download link; scripts/fetch-assets-posix.sh sends
     * the same one. android.util.Base64, not java.util, because the latter only
     * arrived in API 26 and this app supports 21.
     */
    private static String autobuildAuthHeader() {
        byte[] login = AUTOBUILD_LOGIN.getBytes(StandardCharsets.UTF_8);
        return "Basic " + Base64.encodeToString(login, Base64.NO_WRAP);
    }

    // ------------------------------------------------------------- utilities

    private static boolean hasPk3(File dir, String suffix) {
        File[] entries = dir.listFiles();
        if (entries == null) {
            return false;
        }
        for (File entry : entries) {
            String name = entry.getName();
            if (name.startsWith("xonotic-") && name.endsWith(suffix)) {
                return true;
            }
        }
        return false;
    }

    /**
     * @param strip when non-null, only entries under this prefix are extracted,
     *              and the prefix is dropped from their destination path.
     */
    private static void unzip(ZipInputStream zip, File dest, String strip,
                              Progress progress, String status) throws IOException {
        String canonicalDest = dest.getCanonicalPath() + File.separator;
        byte[] buffer = new byte[64 * 1024];
        int files = 0;
        ZipEntry entry;
        while ((entry = zip.getNextEntry()) != null) {
            String name = entry.getName();
            if (strip != null) {
                if (!name.startsWith(strip)) {
                    continue;
                }
                name = name.substring(strip.length());
            }
            if (name.isEmpty()) {
                continue;
            }

            File out = new File(dest, name);
            // Zip-slip guard: an entry named ../../foo would otherwise escape
            // the app's private directory.
            if (!out.getCanonicalPath().startsWith(canonicalDest)) {
                throw new IOException("Refusing zip entry outside " + dest + ": " + entry.getName());
            }

            if (entry.isDirectory()) {
                mkdirs(out);
                continue;
            }
            mkdirs(out.getParentFile());
            File target = strip == null
                    ? out
                    : new File(out.getParentFile(), out.getName() + ".part");
            try (OutputStream sink = new FileOutputStream(target)) {
                int read;
                while ((read = zip.read(buffer)) > 0) {
                    sink.write(buffer, 0, read);
                }
            } catch (IOException e) {
                if (strip != null && !target.delete() && target.exists()) {
                    Log.w(TAG, "Could not delete " + target);
                }
                throw e;
            }
            if (strip != null) {
                if (out.exists() && !out.delete()) {
                    throw new IOException("Cannot replace " + out);
                }
                if (!target.renameTo(out)) {
                    throw new IOException("Cannot move " + target + " to " + out);
                }
            }
            if (++files % 200 == 0) {
                progress.update(status, files + " files", -1);
            }
        }
    }

    private static void mkdirs(File dir) throws IOException {
        if (dir != null && !dir.isDirectory() && !dir.mkdirs()) {
            throw new IOException("Cannot create " + dir);
        }
    }

    private String appVersion() {
        try {
            return context.getPackageManager()
                    .getPackageInfo(context.getPackageName(), 0).versionName;
        } catch (Exception e) {
            Log.w(TAG, "Cannot read package version", e);
            return "unknown";
        }
    }

    private static String readAll(File file) throws IOException {
        byte[] bytes = new byte[(int) file.length()];
        try (InputStream in = new FileInputStream(file)) {
            int off = 0;
            int read;
            while (off < bytes.length && (read = in.read(bytes, off, bytes.length - off)) > 0) {
                off += read;
            }
        }
        return new String(bytes, StandardCharsets.UTF_8).trim();
    }

    private static void writeAll(File file, String text) throws IOException {
        try (OutputStream out = new FileOutputStream(file)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        }
    }
}
