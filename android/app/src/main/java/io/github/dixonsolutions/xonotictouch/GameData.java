package io.github.dixonsolutions.xonotictouch;

import android.content.Context;
import android.util.Base64;
import android.util.Log;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.io.OutputStream;
import java.io.SequenceInputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
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
    /** "crc size name" per file of the last unpack (installBundle). */
    private static final String BUNDLE_MANIFEST = ".bundle-manifest";

    private static final String AUTOBUILD = "https://beta.xonotic.org/autobuild";
    private static final String AUTOBUILD_LOGIN = "xonotic:g-23";

    /**
     * One archive covers everything. Xonotic-latest.zip ships the data, maps,
     * music, nexcompat and font pk3s together; the -mappingsupport and -high
     * builds duplicate them, which is 1.6 GB of download for no new files.
     */
    private static final String ASSET_ARCHIVE = "Xonotic-latest.zip";

    /** The pk3s that have to be on disk before the game is playable. */
    private static final String[] REQUIRED_PK3S = {
        "-data.pk3", "-maps.pk3", "-music.pk3",
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

    /** The directory passed to the engine as {@code -userdir}. */
    static File userDir(File baseDir) {
        return new File(baseDir, "userdata");
    }

    /**
     * Where the engine resolves a QuakeC {@code fopen(name, FILE_WRITE)}.
     *
     * Two things stack up to make this not the path the QuakeC source reads
     * as. darkplaces writes into the userdir's copy of the gamedir, not the
     * basedir's, whenever a userdir is set — and it is. On top of that
     * {@code VM_fopen} prefixes a {@code data/} of its own, which is the
     * doubling behind Xonotic's own "saved in data/data/" messages.
     *
     * Reads search this directory first, so it is also where a file the menu
     * has to read belongs. Anything on the Java side that has to meet QuakeC
     * halfway spells the path from here rather than working it out again.
     */
    static File engineWriteDir(File baseDir) {
        return new File(new File(userDir(baseDir), "data"), "data");
    }

    /**
     * The config chain the engine runs on top of Xonotic's own, and the file
     * at the end of it.
     *
     * Linux gets this from packaging/start.sh: the Touch defaults in
     * {@code xonotic.cfg} (touch/xonotic.cfg in the tree), then the player's
     * {@code config.cfg} again so their settings win, then a generated
     * {@code touch/startup.cfg}. Android launched the engine with none of it,
     * so it ran as stock Xonotic with a touch HUD: none of the Touch defaults,
     * and none of the settings that make public servers joinable --
     * {@code cl_csqc_download 2} (take the server's CSQC unless this is its
     * exact build) and {@code cl_curl_enabled 1} (fetch the server's map).
     *
     * startup.cfg runs last, after config.cfg, so a value archived by an
     * older build cannot pin either of those off. It is written into the
     * userdir's gamedir, which the engine searches before anything else.
     *
     * {@code -customgamename} is deliberately not passed: on its own it also
     * renames the game in master server queries, which then list no public
     * servers at all. start.sh passes it for the window title and puts the
     * query name back with {@code -customgamenetworkfiltername Xonotic}.
     */
    static List<String> startupArguments(Context context, File baseDir) {
        writeStartupConfig(baseDir, DeviceProfile.detect(context));
        return Arrays.asList(
                "+exec", "xonotic.cfg",
                "+exec", "config.cfg",
                "+exec", "autoexec.cfg",
                "+exec", "touch/startup.cfg",
                // BootActivity does not start the engine until every pack is
                // on disk, so the in-game download wizard has nothing to do.
                "+set", "_touch_assets_ready", "1");
    }

    private static void writeStartupConfig(File baseDir, DeviceProfile device) {
        File touchDir = new File(new File(userDir(baseDir), "data"), "touch");
        File cfg = new File(touchDir, "startup.cfg");
        StringBuilder text = new StringBuilder()
                .append("// Generated by the Android launcher (GameData.java) on every start;\n")
                .append("// the Linux equivalent is written by packaging/start.sh.\n")
                // QC statement tracing and developer output went to logcat
                // on every frame when an older build left them archived on.
                .append("developer 0\n")
                .append("prvm_traceqc 0\n")
                .append("prvm_statementprofiling 0\n")
                .append("prvm_timeprofiling 0\n")
                .append("cl_csqc_download 2\n")
                .append("cl_curl_enabled 1\n")
                .append("exec touch/profiles/standard.cfg\n")
                // The performance profile goes on once, on the first start
                // (touch_perf_version, archived; xonotic.cfg defaults it to
                // 0 before config.cfg runs). It used to be the battery
                // profile on every start: 30 fps, and every graphics choice
                // in the menus undone at the next launch. Which profile goes
                // on now depends on the phone (DeviceProfile): balanced (60 fps
                // with vsync) for most, the 30 fps saver where 60 cannot hold,
                // full-size textures where there is memory for them.
                // Version 3 removed the 3D view cap from the defaults: installs
                // on 1 or 2 get it reset to full resolution, once. Version 4
                // turns the performance hint off, once, on installs that saved
                // it on.
                .append(device.startupCvars())
                .append("alias _touch_perf_apply0 \"" + device.firstRunProfileCommands() + "; seta touch_perf_version 3\"\n")
                .append("alias _touch_perf_apply1 \"r_viewscale_maxheight 0; seta touch_perf_version 3\"\n")
                .append("alias _touch_perf_apply2 \"r_viewscale_maxheight 0; seta touch_perf_version 3\"\n")
                .append("alias _touch_perf_apply3 \"sys_perfhint 0; seta touch_perf_version 4\"\n")
                .append("alias _touch_perf_apply4 \"\"\n")
                .append("_touch_perf_apply$touch_perf_version\n");
        // The player's saved layout, sensitivity included. Both savers (the
        // in-game customize screen and the menu's Touch tab) write it with
        // fopen, which lands in engineWriteDir -- one data/ deeper than a
        // bare `exec touch.layout.cfg` looks. That name never found it, and
        // the schema check then took the missing file for an old one and
        // saved the defaults over it on every cold start. Android cold-starts
        // after every update, so that is when the settings went.
        if (new File(engineWriteDir(baseDir), "touch.layout.cfg").isFile()) {
            text.append("exec data/touch.layout.cfg\n");
        } else if (new File(new File(userDir(baseDir), "data"), "touch.layout.cfg").isFile()) {
            text.append("exec touch.layout.cfg\n");
        }
        try {
            mkdirs(touchDir);
            writeAll(cfg, text.toString());
        } catch (IOException e) {
            // Not fatal: the engine still starts, just without these values.
            Log.w(TAG, "Could not write " + cfg, e);
        }
    }

    /**
     * One preparation at a time, process-wide. Two runs overlapping write the
     * same pk3 .part files and each renames its copy into place, which can
     * leave a corrupt pk3 that every later launch takes for a finished one.
     * A second caller waits here and then finds the work already done.
     */
    private static final Object PREPARE_LOCK = new Object();

    void prepare(Progress progress) throws IOException {
        synchronized (PREPARE_LOCK) {
            File base = baseDir();
            if (!base.isDirectory() && !base.mkdirs()) {
                throw new IOException("Cannot create " + base);
            }
            installBundle(progress);
            downloadAssets(progress);
        }
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
        long started = System.currentTimeMillis();

        // The touch masks moved from .tga to .png. Unpacking only adds and
        // overwrites, and the engine tries .tga first, so a previous build's
        // masks would keep winning over the new ones.
        File[] staleMasks = new File(baseDir(), "data/xonotic-data.pk3dir/gfx/touch")
                .listFiles((dir, name) -> name.endsWith(".tga"));
        if (staleMasks != null) {
            for (File mask : staleMasks) {
                if (!mask.delete()) {
                    Log.w(TAG, "Could not remove old " + mask);
                }
            }
        }

        // Only what changed is written. The bundle is about 94 MB in some 500
        // files, and between two builds all but the game code (the progs,
        // a few MB) is the same: rewriting all of it after every update is
        // what kept the player at "Unpacking game data" for seconds. An entry
        // whose CRC and size match the last unpack, with that file still in
        // place at that size, is skipped.
        File manifestFile = new File(baseDir(), BUNDLE_MANIFEST);
        Map<String, String> previous = readManifest(manifestFile);
        Map<String, String> current = new HashMap<>();
        int[] written = new int[1];
        try (InputStream raw = context.getAssets().open(BUNDLE_ASSET)) {
            unzip(new ZipInputStream(raw), baseDir(), null, progress, status, previous, current, written);
        }

        writeManifest(manifestFile, current);
        writeAll(stamp, version);
        Log.i(TAG, "Bundle unpacked: " + written[0] + " of " + current.size() + " files changed, "
                + (System.currentTimeMillis() - started) + " ms");
    }

    // -------------------------------------------------------------- download

    /** Parallel byte ranges while the mirror honours them; one stream otherwise. */
    private static final int DOWNLOAD_STREAMS = 4;
    private static final int DOWNLOAD_ATTEMPTS = 4;
    private static final long DOWNLOAD_RETRY_MS = 3000;
    private static final String ASSET_STAGING = "download";

    boolean assetsMissing() {
        File data = dataDir();
        for (String suffix : REQUIRED_PK3S) {
            if (!hasPk3(data, suffix)) {
                return true;
            }
        }
        return false;
    }

    private void downloadAssets(Progress progress) throws IOException {
        File data = dataDir();
        if (!data.isDirectory() && !data.mkdirs()) {
            throw new IOException("Cannot create " + data);
        }
        if (!assetsMissing()) {
            return;
        }

        String status = context.getString(R.string.boot_downloading);
        Log.i(TAG, "Fetching " + ASSET_ARCHIVE);
        // The archive is staged in parts rather than streamed into the pk3s: a
        // stopped run then resumes from the bytes on disk, and an unzip that
        // fails can be retried without fetching the gigabyte again.
        ArchiveFetch archive = new ArchiveFetch(new File(context.getFilesDir(), ASSET_STAGING), progress, status);
        archive.fetch();
        try (InputStream raw = archive.open()) {
            unzip(new ZipInputStream(raw), data, "Xonotic/data/", progress, status);
        }
        archive.discard();

        if (assetsMissing()) {
            throw new IOException(ASSET_ARCHIVE + " did not yield a required pk3");
        }
    }

    /**
     * The asset archive as parallel byte ranges, each on disk as its own part
     * file. The meta file pins the server's ETag and size: a changed archive
     * discards the parts rather than splicing two builds together.
     */
    private final class ArchiveFetch {
        private final File dir;
        private final Progress progress;
        private final String status;
        private final String url = AUTOBUILD + "/" + ASSET_ARCHIVE;
        private boolean ranged;
        private int streams;
        private long total;
        private long[] starts;
        private long[] lengths;
        private long done;
        private long lastReport;

        ArchiveFetch(File dir, Progress progress, String status) {
            this.dir = dir;
            this.progress = progress;
            this.status = status;
        }

        void fetch() throws IOException {
            mkdirs(dir);
            String etag;
            HttpURLConnection probe = request("bytes=0-0");
            try {
                int code = probe.getResponseCode();
                if (code != HttpURLConnection.HTTP_OK && code != HttpURLConnection.HTTP_PARTIAL) {
                    throw new IOException("HTTP " + code + " for " + ASSET_ARCHIVE);
                }
                etag = String.valueOf(probe.getHeaderField("ETag"));
                String contentRange = probe.getHeaderField("Content-Range");
                ranged = code == HttpURLConnection.HTTP_PARTIAL && contentRange != null
                        && contentRange.contains("/");
                total = ranged
                        ? Long.parseLong(contentRange.substring(contentRange.lastIndexOf('/') + 1).trim())
                        : Long.parseLong(probe.getHeaderField("Content-Length"));
            } catch (NumberFormatException e) {
                throw new IOException("No size from " + url, e);
            } finally {
                probe.disconnect();
            }
            if (total <= 0) {
                throw new IOException("No size from " + url);
            }
            streams = ranged ? DOWNLOAD_STREAMS : 1;

            String identity = etag + "\n" + total + "\n" + streams;
            File meta = new File(dir, "meta");
            if (!meta.isFile() || !identity.equals(readAll(meta))) {
                clearParts();
                writeAll(meta, identity);
            }

            starts = new long[streams];
            lengths = new long[streams];
            for (int n = 0; n < streams; n++) {
                starts[n] = total * n / streams;
                lengths[n] = total * (n + 1) / streams - starts[n];
            }
            done = 0;
            for (int n = 0; n < streams; n++) {
                done += ranged ? Math.min(partFile(n).length(), lengths[n]) : 0;
            }
            if (done < total) {
                fetchParts();
            }
        }

        InputStream open() throws IOException {
            List<InputStream> parts = new ArrayList<>();
            for (int n = 0; n < streams; n++) {
                parts.add(new FileInputStream(partFile(n)));
            }
            return new BufferedInputStream(
                    new SequenceInputStream(Collections.enumeration(parts)), 256 * 1024);
        }

        void discard() {
            clearParts();
            File meta = new File(dir, "meta");
            if (!meta.delete() && meta.exists()) {
                Log.w(TAG, "Could not remove " + meta);
            }
            if (!dir.delete() && dir.exists()) {
                Log.w(TAG, "Could not remove " + dir);
            }
        }

        private void fetchParts() throws IOException {
            ExecutorService pool = Executors.newFixedThreadPool(streams, runnable -> {
                Thread thread = new Thread(runnable, "xonotic-download");
                thread.setDaemon(true);
                return thread;
            });
            List<Future<?>> jobs = new ArrayList<>();
            try {
                for (int n = 0; n < streams; n++) {
                    final int part = n;
                    jobs.add(pool.submit(() -> {
                        fetchPart(part);
                        return null;
                    }));
                }
                for (Future<?> job : jobs) {
                    job.get();
                }
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw new InterruptedIOException("Download cancelled");
            } catch (ExecutionException e) {
                Throwable cause = e.getCause();
                if (cause instanceof IOException) {
                    throw (IOException) cause;
                }
                throw new IOException(String.valueOf(cause), cause);
            } finally {
                pool.shutdownNow();
            }
        }

        private void fetchPart(int n) throws IOException {
            for (int attempt = 1; ; attempt++) {
                try {
                    transfer(n);
                    return;
                } catch (IOException e) {
                    if (attempt >= DOWNLOAD_ATTEMPTS || Thread.currentThread().isInterrupted()) {
                        throw e;
                    }
                    Log.w(TAG, "Part " + n + " stopped, retrying", e);
                    try {
                        Thread.sleep(DOWNLOAD_RETRY_MS);
                    } catch (InterruptedException interrupted) {
                        Thread.currentThread().interrupt();
                        throw new InterruptedIOException("Download cancelled");
                    }
                }
            }
        }

        private void transfer(int n) throws IOException {
            File part = partFile(n);
            long have = ranged ? part.length() : 0;
            if (have > lengths[n]) {
                have = 0;
            }
            if (have == lengths[n]) {
                return;
            }
            long from = starts[n] + have;
            long to = starts[n] + lengths[n] - 1;
            HttpURLConnection connection = request(ranged ? "bytes=" + from + "-" + to : null);
            try {
                int code = connection.getResponseCode();
                if (code != (ranged ? HttpURLConnection.HTTP_PARTIAL : HttpURLConnection.HTTP_OK)) {
                    throw new IOException("HTTP " + code + " for " + ASSET_ARCHIVE);
                }
                byte[] buffer = new byte[64 * 1024];
                long remaining = lengths[n] - have;
                try (InputStream in = connection.getInputStream();
                     FileOutputStream out = new FileOutputStream(part, have > 0)) {
                    int read;
                    while (remaining > 0
                            && (read = in.read(buffer, 0, (int) Math.min(buffer.length, remaining))) > 0) {
                        if (Thread.currentThread().isInterrupted()) {
                            throw new InterruptedIOException("Download cancelled");
                        }
                        out.write(buffer, 0, read);
                        remaining -= read;
                        advance(read);
                    }
                }
                if (remaining != 0) {
                    throw new IOException("Connection closed early on part " + n);
                }
            } finally {
                connection.disconnect();
            }
        }

        private HttpURLConnection request(String range) throws IOException {
            HttpURLConnection connection = (HttpURLConnection) new URL(url).openConnection();
            connection.setRequestProperty("Authorization", autobuildAuthHeader());
            connection.setRequestProperty("Accept-Encoding", "identity");
            if (range != null) {
                connection.setRequestProperty("Range", range);
            }
            connection.setConnectTimeout(30_000);
            connection.setReadTimeout(60_000);
            connection.setInstanceFollowRedirects(true);
            return connection;
        }

        private synchronized void advance(long count) {
            done += count;
            // One UI post per megabyte; per-chunk posts flood the looper.
            if (done - lastReport >= 1024 * 1024 || done >= total) {
                lastReport = done;
                progress.update(status, (done >> 20) + " / " + (total >> 20) + " MB",
                        (int) (done * 100 / total));
            }
        }

        private File partFile(int n) {
            return new File(dir, "part." + n);
        }

        private void clearParts() {
            File[] entries = dir.listFiles();
            if (entries == null) {
                return;
            }
            for (File entry : entries) {
                if (!entry.delete() && entry.exists()) {
                    Log.w(TAG, "Could not remove " + entry);
                }
            }
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
        unzip(zip, dest, strip, progress, status, null, null, null);
    }

    /**
     * @param previous signatures from the last unpack ("crc:size" by name), to
     *                 skip entries that have not changed; null writes all
     * @param current  filled with this bundle's signatures; may be null
     * @param written  written[0] counts the files actually written; may be null
     */
    private static void unzip(ZipInputStream zip, File dest, String strip,
                              Progress progress, String status, Map<String, String> previous,
                              Map<String, String> current, int[] written) throws IOException {
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
            // The zip tool writes CRC and size in each local header, so they
            // are known before the data is read; -1 where not, and such an
            // entry is always written.
            String signature = (entry.getCrc() >= 0 && entry.getSize() >= 0)
                    ? entry.getCrc() + ":" + entry.getSize() : null;
            if (current != null && signature != null) {
                current.put(name, signature);
            }
            if (previous != null && signature != null && signature.equals(previous.get(name))
                    && out.isFile() && out.length() == entry.getSize()) {
                // Unchanged: getNextEntry() steps over the data.
                continue;
            }
            if (written != null) {
                written[0]++;
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

    private static Map<String, String> readManifest(File file) {
        Map<String, String> map = new HashMap<>();
        if (!file.isFile()) {
            return map;
        }
        try (java.io.BufferedReader in = new java.io.BufferedReader(new java.io.FileReader(file))) {
            String line;
            while ((line = in.readLine()) != null) {
                int space = line.indexOf(' ');
                if (space > 0) {
                    map.put(line.substring(space + 1), line.substring(0, space));
                }
            }
        } catch (IOException e) {
            // A manifest we cannot read only costs a full unpack.
            Log.w(TAG, "Could not read " + file, e);
            map.clear();
        }
        return map;
    }

    private static void writeManifest(File file, Map<String, String> map) throws IOException {
        StringBuilder text = new StringBuilder();
        for (Map.Entry<String, String> e : map.entrySet()) {
            text.append(e.getValue()).append(' ').append(e.getKey()).append('\n');
        }
        writeAll(file, text.toString());
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
