package io.github.dixonsolutions.xonotictouch;

import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
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

    private static final String TAG = "Godnotic";

    /** Bundled payload staged into the APK by scripts/android-stage-assets.sh. */
    private static final String BUNDLE_ASSET = "xonotic-slim-data.zip";
    private static final String BUNDLE_STAMP = ".bundle-version";
    /** "crc size name" per file of the last unpack (installBundle). */
    private static final String BUNDLE_MANIFEST = ".bundle-manifest";

    /**
     * The pinned full data set in Godnotic's own data repository. Not a moving
     * upstream build: the packs change only when a new data release is made.
     */
    private static final String FULL_DATA =
            "https://github.com/dixonSolutions/Godnotic-data/releases/download/data-20261107";

    /**
     * One archive covers everything: the data, maps, music and font pk3s.
     * PackFetch takes the pk3s out of it without storing the archive.
     */
    private static final String ASSET_ARCHIVE = "Godnotic-data-20261107.zip";
    /** The folder in the archive whose pk3s make up the game data. */
    private static final String ARCHIVE_DATA = "Xonotic/data/";

    /**
     * The pk3s that have to be on disk before the game is playable. Music is
     * optional (the setup screen asks, default off); the game is silent
     * without it and the launcher's "Download music" shortcut fetches it later.
     */
    private static final String[] REQUIRED_PK3S = {
        "-data.pk3", "-maps.pk3",
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

    /**
     * touch_perf_version 5, once per install. Until 1.2.124 the GLES renderer
     * streamed its per-frame vertex data with glBufferSubData, which Mali and
     * other tile-based GPUs answer by copying the whole buffer, and the
     * particle atlas decoded to half-opaque blocks. Players turned particles
     * off (every explosion with them) to get a playable frame rate, and the
     * engine doubled r_buffermegs_* each time a frame overflowed them and
     * archived the result, which made each copy bigger still: 16 MB on a
     * Galaxy Tab Active5 that then ran at 7 fps. Both are fixed in the engine;
     * this puts the settings they left behind back to the defaults.
     */
    static final String V5_RESET =
            "cl_particles 1; r_buffermegs_vertex 4; r_buffermegs_index16 1; r_buffermegs_index32 1";

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
                // it on. Version 5 puts particles back on and the per-frame
                // vertex buffers back to their default size, once: both were
                // switched off or grown while the GLES renderer was too slow
                // to play (see V5_RESET).
                .append(device.startupCvars())
                .append("alias _touch_perf_apply0 \"" + device.firstRunProfileCommands() + "; seta touch_perf_version 5\"\n")
                .append("alias _touch_perf_apply1 \"r_viewscale_maxheight 0; sys_perfhint 0; " + V5_RESET + "; seta touch_perf_version 5\"\n")
                .append("alias _touch_perf_apply2 \"r_viewscale_maxheight 0; sys_perfhint 0; " + V5_RESET + "; seta touch_perf_version 5\"\n")
                .append("alias _touch_perf_apply3 \"sys_perfhint 0; " + V5_RESET + "; seta touch_perf_version 5\"\n")
                .append("alias _touch_perf_apply4 \"" + V5_RESET + "; seta touch_perf_version 5\"\n")
                .append("alias _touch_perf_apply5 \"\"\n")
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

    /** The release the unpacked bundle belongs to (the stamp written when an unpack finished), or null. */
    String bundleVersion() {
        File stamp = new File(baseDir(), BUNDLE_STAMP);
        try {
            return stamp.isFile() ? readAll(stamp) : null;
        } catch (IOException e) {
            return null;
        }
    }

    /** Forget what was unpacked: the next {@link #prepare} unpacks the whole bundle again. */
    void invalidateBundle() {
        for (String name : new String[] { BUNDLE_STAMP, BUNDLE_MANIFEST }) {
            File file = new File(baseDir(), name);
            if (file.exists() && !file.delete()) {
                Log.w(TAG, "Could not delete " + file);
            }
        }
    }

    /**
     * In-process rollback: the game data must belong to the release that runs again, not to
     * {@code failedVersion}, which has been unpacking over it. When the failed release's unpack
     * finished (its stamp is there) the manifest it left is exact and only what differs is
     * written; otherwise files of both releases may be mixed, so everything is unpacked again.
     */
    void prepareAfterRollback(Progress progress, String failedVersion) throws IOException {
        synchronized (PREPARE_LOCK) {
            if (!failedVersion.equals(bundleVersion())) {
                invalidateBundle();
            }
            prepare(progress);
        }
    }

    /**
     * Gets the APK's slim data set into place. The pk3 download is separate
     * ({@link #planDownload}, {@link #download}): it asks the player first.
     */
    void prepare(Progress progress) throws IOException {
        synchronized (PREPARE_LOCK) {
            File base = baseDir();
            if (!base.isDirectory() && !base.mkdirs()) {
                throw new IOException("Cannot create " + base);
            }
            installBundle(progress);
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

        String status = HostRes.string(context, "boot_unpacking");
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

    /** Where 1.2.x before the streamed download staged the whole archive. */
    private static final String OLD_STAGING = "download";

    /**
     * True while the game cannot start: a core pk3 is missing, or a setup
     * started and has not got every non-optional pack in yet (the fonts, the
     * compatibility packs).
     */
    boolean assetsMissing() {
        File data = dataDir();
        for (String suffix : REQUIRED_PK3S) {
            if (!hasPk3(data, suffix)) {
                return true;
            }
        }
        for (String role : PackFetch.Meta.read(data).missing(data)) {
            if (!role.contains("-music")) {
                return true;
            }
        }
        return false;
    }

    boolean musicInstalled() {
        return hasPk3(dataDir(), "-music.pk3");
    }

    /** Music was asked for by a setup that has not finished it. */
    boolean musicWanted() {
        File data = dataDir();
        for (String role : PackFetch.Meta.read(data).want) {
            if (role.contains("-music")) {
                return true;
            }
        }
        return false;
    }

    /** Reads the archive's file list (two small requests); network, so off the UI thread. */
    PackFetch.Plan planDownload() throws IOException {
        File data = dataDir();
        mkdirs(data);
        // A 1.2.x install that stopped mid-download left the archive's parts
        // here, up to 1.2 GB that nothing reads any more.
        File old = new File(context.getFilesDir(), OLD_STAGING);
        File[] parts = old.listFiles();
        if (parts != null) {
            for (File part : parts) {
                if (!part.delete()) {
                    Log.w(TAG, "Could not remove " + part);
                }
            }
        }
        if (old.exists() && !old.delete()) {
            Log.w(TAG, "Could not remove " + old);
        }
        // The slim set stores its textures as ASTC; a GPU without it gets the
        // full set (GpuProbe). The slim archive being unusable must not block
        // the setup either: the full set has every pack too.
        String full = FULL_DATA + "/" + ASSET_ARCHIVE;
        boolean astc = GpuProbe.astc();
        String slim = astc ? assetLine("godnotic-data-url.txt") : null;
        if (slim != null) {
            try {
                PackFetch.Plan plan = PackFetch.plan(slim, null, ARCHIVE_DATA);
                Log.i(TAG, "Game data set: slim (ASTC) " + slim);
                return plan;
            } catch (IOException e) {
                Log.w(TAG, "Slim data set unusable (" + e.getMessage() + "); using the full set");
            }
        }
        Log.i(TAG, "Game data set: full " + full + (astc ? "" : " (GPU without ASTC)"));
        return PackFetch.plan(full, null, ARCHIVE_DATA);
    }

    private String assetLine(String name) {
        try (InputStream in = context.getAssets().open(name)) {
            byte[] b = new byte[4096];
            int n = in.read(b);
            String line = n > 0 ? new String(b, 0, n, java.nio.charset.StandardCharsets.UTF_8).trim() : "";
            return line.startsWith("https://") ? line : null;
        } catch (IOException e) {
            Log.w(TAG, "No " + name + " in the assets");
            return null;
        }
    }

    /** "2 min", "40 s": what is left at the current speed, for the progress line. */
    static String timeLeft(long bytesLeft, double bytesPerSecond) {
        if (bytesPerSecond < 1024) {
            return "…";
        }
        long s = (long) (bytesLeft / bytesPerSecond);
        if (s < 60) {
            return Math.max(1, s) + " s";
        }
        if (s < 3600) {
            return ((s + 30) / 60) + " min";
        }
        return (s / 3600) + " h " + ((s % 3600) / 60) + " min";
    }

    /** Fetches the planned pk3s (music when asked) into the data dir. Interrupt to stop. */
    void download(PackFetch.Plan plan, boolean music, Progress progress) throws IOException {
        synchronized (PREPARE_LOCK) {
            String status = HostRes.string(context, "boot_downloading");
            long started = System.currentTimeMillis();
            // mark[1] < 0: no sample yet. The first report carries what an
            // earlier run already fetched, which is no speed at all.
            long[] mark = { started, -1 };
            double[] rate = { 0 };
            Log.i(TAG, "Fetching pk3s from " + plan.url + (music ? " with music" : ""));
            new PackFetch(dataDir(), plan, (done, total, file, verifying) -> {
                long now = System.currentTimeMillis();
                synchronized (rate) {
                    if (mark[1] < 0) {
                        mark[0] = now;
                        mark[1] = done;
                    } else if (now - mark[0] >= 1000) {
                        double r = (done - mark[1]) * 1000.0 / (now - mark[0]);
                        rate[0] = rate[0] == 0 ? r : rate[0] * 0.7 + r * 0.3;
                        mark[0] = now;
                        mark[1] = done;
                    }
                }
                int percent = total > 0 ? (int) (done * 100 / total) : -1;
                String detail = verifying
                        ? HostRes.string(context, "boot_verifying", file)
                        : HostRes.string(context, "boot_progress", megabytes(done), megabytes(total),
                                String.format(java.util.Locale.US, "%.1f", rate[0] / (1024 * 1024)),
                                timeLeft(total - done, rate[0]));
                progress.update(status, detail, percent);
            }).run(music);
            if (assetsMissing()) {
                throw new IOException(plan.url + " did not yield a required pk3");
            }
            Log.i(TAG, "Game data downloaded in " + (System.currentTimeMillis() - started) / 1000 + " s");
        }
    }

    /** Whole megabytes, for the progress line. */
    static String megabytes(long bytes) {
        return String.valueOf(bytes >> 20);
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
     * @param strip    when non-null, only entries under this prefix are extracted,
     *                 and the prefix is dropped from their destination path.
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
            if (Thread.currentThread().isInterrupted()) {
                // Cancel on the boot screen: stop between files; each one
                // already written is whole (written under a temporary name).
                throw new InterruptedIOException("Unpacking cancelled");
            }
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
                    // A pk3 goes to a temporary name, so it can stop mid-file.
                    if (strip != null && Thread.currentThread().isInterrupted()) {
                        throw new InterruptedIOException("Unpacking cancelled");
                    }
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
        // Under AndySU the installed APK is a frozen host; the running payload's version is what
        // the bundled data belongs to.
        String payload = AndySUCompat.payloadVersion();
        if (payload != null) return payload;
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
