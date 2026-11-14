package io.github.dixonsolutions.godnotic;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.net.HttpURLConnection;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.locks.ReentrantLock;

/**
 * One data directory's progressive download (docs/DATA_FETCH.md): which
 * manifest it follows, the two phases (first_play before the game starts,
 * everything else behind it), the status file the engine reads and the
 * request file it writes.
 *
 * The manifest that was accepted last is kept as {@code .godnotic-data.json},
 * so a start needs no network to know what is installed and what is left.
 * Without one (none published, or none ever fetched) {@link #manifest()} is
 * null and the caller does what it did before the split: the single archive.
 *
 * One session per data directory per process (GameData keeps it), and one run
 * at a time: {@link #firstPlay} and {@link #background} share a lock, which is
 * what keeps a part file to one writer.
 */
final class DataSession {

    static final String MANIFEST_FILE = ".godnotic-data.json";
    /** Connections of the first download, with nothing else running: as PackFetch. */
    static final int BOOT_CONNECTIONS = 8;
    /** The remainder runs beside a match: fewer, so the game's own packets get through. */
    static final int BACKGROUND_CONNECTIONS = 4;

    /** Where the background run lives: an Android service, a plain thread, a test. */
    interface Host extends DataFetch.Gate {
        /** False ends the background run; what is on disk is kept. */
        boolean keepGoing();

        /** Any network at all: the manifest (some tens of KB) is fetched on it. */
        boolean online();

        /** A pack was installed; the engine may be running. */
        void landed(String installAs);

        /** The state or the numbers changed: at most about once a second. */
        void changed();
    }

    interface Progress {
        void progress(long done, long total, String verifying);
    }

    /** What {@link #refresh} found. */
    enum Refresh {
        /** The published manifest is valid and now the one in use. */
        CURRENT,
        /** No answer (no network, a server error): the stored one stays in use, if there is one. */
        UNREACHABLE,
        /** Answered, but not with a manifest this client accepts (404, bad JSON, another format). */
        ABSENT,
    }

    private final File dir;
    private final File statusFile;
    private final File requestFile;
    private final DataFetch.Env env;
    private final DataFetch.Options options;
    private final DataInstall install;
    private final ReentrantLock runLock = new ReentrantLock();
    /** Requests written before this are an earlier session's. */
    private final long started = System.currentTimeMillis();
    private final List<String> landed = Collections.synchronizedList(new ArrayList<String>());

    private volatile DataFetch fetch;
    private volatile Host host;
    private volatile Thread backgroundThread;
    private volatile String state;
    private volatile String message;
    private volatile String want;
    private volatile List<DataManifest.Pack> wantNeeds = Collections.emptyList();
    private volatile long requestSerial = Long.MIN_VALUE;
    private String requestMap;
    private String lastStatus;

    DataSession(File dataDir, File touchDir, DataFetch.Env env) {
        this(dataDir, touchDir, env, new DataFetch.Options());
    }

    DataSession(File dataDir, File touchDir, DataFetch.Env env, DataFetch.Options options) {
        this.dir = dataDir;
        this.statusFile = new File(touchDir, DataStatus.STATUS_FILE);
        this.requestFile = new File(touchDir, DataStatus.REQUEST_FILE);
        this.env = env;
        this.options = options;
        this.install = new DataInstall(dataDir, env);
        File stored = new File(dataDir, MANIFEST_FILE);
        if (stored.isFile()) {
            try {
                use(DataManifest.parse(DataStatus.readText(stored, 8 << 20)));
            } catch (IOException | DataManifest.Invalid e) {
                env.log("The stored data manifest is unusable (" + e.getMessage() + ")");
            }
        }
    }

    private void use(DataManifest m) {
        install.adopt(m);
        fetch = new DataFetch(install, m, env, options, this::onLanded);
        String wanted = want;
        if (wanted != null) {
            setWant(wanted);
        }
    }

    // -------------------------------------------------------------- manifest

    DataManifest manifest() {
        return install.manifest();
    }

    DataInstall install() {
        return install;
    }

    /** "…/data-slim/Godnotic-data.zip" -> "…/data-slim/godnotic-data-astc.json". */
    static String manifestUrl(String archiveUrl, String profile) {
        if (archiveUrl == null) {
            return null;
        }
        int slash = archiveUrl.lastIndexOf('/');
        if (slash < "https://x".length()) {
            return null;
        }
        return archiveUrl.substring(0, slash + 1) + "godnotic-data-" + profile + ".json";
    }

    /**
     * Fetches the published manifest. A valid one is stored and becomes the
     * one in use; anything else leaves the stored one in charge: an install
     * made from a manifest stays what it is (its archive is immutable) when
     * the manifest cannot be read today.
     */
    Refresh refresh(String url) {
        String text;
        try {
            text = get(url);
        } catch (IOException e) {
            env.log("No data manifest from " + url + ": " + e.getMessage());
            return Refresh.UNREACHABLE;
        }
        if (text == null) {
            env.log("No data manifest at " + url);
            return Refresh.ABSENT;
        }
        DataManifest m;
        try {
            m = DataManifest.parse(text);
        } catch (DataManifest.Invalid e) {
            env.log("The data manifest at " + url + " is not usable: " + e.getMessage());
            return Refresh.ABSENT;
        }
        // Not under a running fetch: its packs and parts are of the manifest
        // it started with. The next start takes the new one.
        if (!runLock.tryLock()) {
            return install.manifest() != null ? Refresh.CURRENT : Refresh.UNREACHABLE;
        }
        try {
            DataManifest current = install.manifest();
            if (current != null && current.text.equals(text)) {
                return Refresh.CURRENT;
            }
            try {
                DataStatus.writeAtomic(new File(dir, MANIFEST_FILE), text);
            } catch (IOException e) {
                // Not kept means not used: a set this start installs from and
                // the next start knows nothing of would be two installs.
                env.log("Could not store the data manifest: " + e.getMessage());
                return Refresh.UNREACHABLE;
            }
            env.log("Data manifest: set " + m.set + ", " + m.packs.size() + " packs, "
                    + (m.totalBytes() >> 20) + " MB");
            use(m);
            return Refresh.CURRENT;
        } finally {
            runLock.unlock();
        }
    }

    /** The body of a 200; null for any other answer the server gave; throws when it gave none. */
    private String get(String url) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(Math.min(10_000, options.connectTimeoutMs));
        c.setReadTimeout(Math.min(15_000, options.readTimeoutMs));
        c.setInstanceFollowRedirects(true);
        c.setRequestProperty("Accept-Encoding", "identity");
        try {
            int code = c.getResponseCode();
            if (code >= 500) {
                throw new IOException("HTTP " + code);
            }
            if (code != HttpURLConnection.HTTP_OK) {
                return null;
            }
            try (InputStream in = c.getInputStream()) {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] buf = new byte[16 * 1024];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                    if (out.size() > (8 << 20)) {
                        return null;
                    }
                }
                return new String(out.toByteArray(), StandardCharsets.UTF_8);
            }
        } finally {
            c.disconnect();
        }
    }

    // ------------------------------------------------------------ first play

    /** The game may start: every first_play pack is in, or a complete old install keeps it whole. */
    boolean playable() {
        return install.playable();
    }

    /** Hashes files that are whole but not recorded (DataInstall.reconcile); not for the UI thread. */
    void reconcile() {
        install.reconcile();
    }

    /** What first_play still costs: {to fetch, already on disk in parts}. */
    long[] firstPlayNeed() {
        DataManifest m = install.manifest();
        DataFetch f = fetch;
        long left = 0;
        long resumed = 0;
        if (m != null && f != null) {
            for (DataManifest.Pack p : m.firstPlay) {
                if (!install.installed(p)) {
                    long part = f.doneBytes(Collections.singletonList(p));
                    left += p.size - part;
                    resumed += part;
                }
            }
        }
        return new long[] { left, resumed };
    }

    /** Bytes of the set that come after first_play, in the background. */
    long restBytes() {
        DataManifest m = install.manifest();
        long sum = 0;
        if (m != null) {
            for (DataManifest.Pack p : m.packs) {
                if (!m.firstPlay.contains(p) && !install.installed(p)) {
                    sum += p.size;
                }
            }
        }
        return sum;
    }

    /**
     * Installs first_play; returns when every one of its packs is in. The
     * progress is first_play's bytes alone. Interrupt the thread to stop:
     * what was fetched stays on disk and the next call carries on from it.
     */
    void firstPlay(Progress progress) throws IOException {
        stopBackground();
        try {
            runLock.lockInterruptibly();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new InterruptedIOException("Download stopped");
        }
        try {
            DataManifest m = install.manifest();
            DataFetch f = fetch;
            if (m == null || f == null) {
                throw new IOException("No data manifest");
            }
            install.reconcile();
            if (install.firstPlayInstalled()) {
                return;
            }
            // Retry on the boot screen is the player asking again.
            f.forgive(m.firstPlay);
            f.checkSpace(m.firstPlay);
            long total = 0;
            for (DataManifest.Pack p : m.firstPlay) {
                total += p.size;
            }
            final long all = total;
            Thread ticker = new Thread(() -> {
                while (!Thread.currentThread().isInterrupted()) {
                    progress.progress(f.doneBytes(m.firstPlay), all, f.verifying());
                    try {
                        Thread.sleep(250);
                    } catch (InterruptedException e) {
                        return;
                    }
                }
            }, "godnotic-data-progress");
            ticker.setDaemon(true);
            ticker.start();
            List<DataManifest.Pack> failed;
            try {
                failed = f.run(true, BOOT_CONNECTIONS, () -> true);
            } finally {
                ticker.interrupt();
            }
            if (!failed.isEmpty()) {
                throw new IOException("Damaged download of " + failed.get(0).installAs
                        + "; Retry fetches it again");
            }
            if (!install.firstPlayInstalled()) {
                throw new IOException("The first packs did not all arrive");
            }
            progress.progress(all, all, null);
        } finally {
            runLock.unlock();
        }
    }

    // ------------------------------------------------------------ background

    /**
     * Everything after first_play, without asking, until the set is complete
     * or the host says stop. Refreshes the manifest first (when {@code url}
     * is given and there is a network), so this is also where a complete old
     * install learns which set it stands for and where a new set is noticed.
     * Blocks; call it on a thread of its own.
     */
    void background(String url, Host h) {
        try {
            runLock.lockInterruptibly();
        } catch (InterruptedException e) {
            return;
        }
        Thread ticker = null;
        try {
            backgroundThread = Thread.currentThread();
            host = h;
            final Thread self = Thread.currentThread();
            ticker = new Thread(() -> tick(h, self), "godnotic-data-status");
            ticker.setDaemon(true);
            ticker.start();
            backgroundLoop(url, h);
        } finally {
            if (ticker != null) {
                ticker.interrupt();
            }
            host = null;
            backgroundThread = null;
            Thread.interrupted(); // the stop that ended the run is not the caller's to see
            if (state != null && !DataStatus.COMPLETE.equals(state) && !DataStatus.LEGACY.equals(state)) {
                // Stopped with packs left: the engine must not wait for a map
                // nobody is fetching.
                state = DataStatus.ERROR;
                message = "The download stopped; it carries on at the next start";
            }
            writeStatus();
            h.changed();
            runLock.unlock();
        }
    }

    private void backgroundLoop(String url, Host h) {
        boolean refreshed = url == null;
        long refreshAfter = 0;
        int refreshTries = 0;
        boolean reconciled = false;
        while (h.keepGoing() && !Thread.currentThread().isInterrupted()) {
            if (!refreshed && System.currentTimeMillis() >= refreshAfter && h.online()) {
                // refresh() takes the lock this thread already holds.
                Refresh r = refresh(url);
                // Three tries a run: a server that is down is asked again at the next start.
                refreshed = r != Refresh.UNREACHABLE || ++refreshTries >= 3;
                refreshAfter = System.currentTimeMillis() + 60_000;
                reconciled = false;
            }
            DataManifest m = install.manifest();
            DataFetch f = fetch;
            if (m == null || f == null) {
                if (refreshed || !h.online()) {
                    // No manifest is published (or none can be asked for now):
                    // the single archive's install is all there is.
                    state = null;
                    return;
                }
                if (!sleep(h, 2000)) {
                    return;
                }
                continue;
            }
            if (!reconciled) {
                install.reconcile();
                reconciled = true;
            }
            if (install.legacyCurrent()) {
                setState(DataStatus.LEGACY, null);
                if (refreshed || !h.online()) {
                    return;
                }
                if (!sleep(h, 2000)) {
                    return;
                }
                continue;
            }
            if (install.complete()) {
                setState(DataStatus.COMPLETE, null);
                if (refreshed || !h.online()) {
                    return;
                }
                if (!sleep(h, 2000)) {
                    return;
                }
                continue;
            }
            if (!h.mayFetch()) {
                setState(DataStatus.WAITING, null);
                if (!sleep(h, 1000)) {
                    return;
                }
                continue;
            }
            setState(DataStatus.FETCHING, null);
            try {
                List<DataManifest.Pack> failed = f.run(false, BACKGROUND_CONNECTIONS, h);
                if (install.complete() || install.legacyCurrent()) {
                    continue;
                }
                // Packs that failed their check three times, and the maps
                // that wait for them. They are not fetched again by
                // themselves in this process (an archive that does not match
                // its manifest would be downloaded for ever); a map the
                // player asks for is, and the next start tries all of them.
                setState(DataStatus.ERROR, failed.isEmpty() ? "Some packs could not be installed"
                        : "Damaged download of " + failed.get(0).installAs);
                long serial = requestSerial;
                while (serial == requestSerial) {
                    if (!sleep(h, 500)) {
                        return;
                    }
                }
            } catch (DataFetch.Waiting e) {
                setState(DataStatus.WAITING, null);
                if (!sleep(h, 1000)) {
                    return;
                }
            } catch (DataFetch.NoSpace e) {
                setState(DataStatus.ERROR, e.getMessage());
                if (!sleep(h, 60_000)) {
                    return;
                }
            } catch (IOException e) {
                if (e instanceof InterruptedIOException && !(e instanceof SocketTimeoutException)) {
                    return; // stopped
                }
                env.log("The background download stopped: " + e);
                if (h.online()) {
                    setState(DataStatus.ERROR, String.valueOf(e.getMessage()));
                } else {
                    setState(DataStatus.WAITING, null);
                }
                if (!sleep(h, 15_000)) {
                    return;
                }
            }
        }
    }

    /** False when the run is to end. */
    private boolean sleep(Host h, long ms) {
        long until = System.currentTimeMillis() + ms;
        while (true) {
            if (!h.keepGoing()) {
                return false;
            }
            long left = until - System.currentTimeMillis();
            if (left <= 0) {
                return true;
            }
            try {
                Thread.sleep(Math.min(left, 250));
            } catch (InterruptedException e) {
                return false;
            }
        }
    }

    /** Twice a second while the background run lives: the request file, the status file, the host's stop. */
    private void tick(Host h, Thread run) {
        long lastChanged = 0;
        while (!Thread.currentThread().isInterrupted()) {
            if (!h.keepGoing()) {
                run.interrupt();
                return;
            }
            pollRequest();
            writeStatus();
            long now = System.currentTimeMillis();
            if (now - lastChanged >= 1000) {
                lastChanged = now;
                h.changed();
            }
            try {
                Thread.sleep(500);
            } catch (InterruptedException e) {
                return;
            }
        }
    }

    /** Ends a background run of this process and waits for it; nothing to do when none runs. */
    void stopBackground() {
        Thread t = backgroundThread;
        if (t == null || t == Thread.currentThread()) {
            return;
        }
        t.interrupt();
        long deadline = System.currentTimeMillis() + 20_000;
        while (backgroundThread == t && System.currentTimeMillis() < deadline) {
            try {
                Thread.sleep(50);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return;
            }
        }
    }

    boolean backgroundRunning() {
        return backgroundThread != null;
    }

    // ----------------------------------------------------------------- wants

    /** Reads the engine's request file; a new request makes its map the next thing fetched. */
    void pollRequest() {
        DataStatus.Request r = DataStatus.readRequest(requestFile, started);
        if (r != null && (r.serial != requestSerial || !r.map.equals(requestMap))) {
            requestMap = r.map;
            requestSerial = r.serial;
            env.log("The game asks for map " + r.map);
            setWant(r.map);
        }
    }

    /** {@code map}'s missing packs go to the front of the queue; null, or a map with nothing missing, clears it. */
    void setWant(String map) {
        List<DataManifest.Pack> missing = install.missingNeeds(map);
        DataFetch f = fetch;
        if (map == null || missing.isEmpty()) {
            want = null;
            wantNeeds = Collections.emptyList();
            if (f != null) {
                f.want(null);
            }
            return;
        }
        wantNeeds = missing;
        want = map;
        if (f != null) {
            f.want(map);
        }
    }

    String want() {
        return want;
    }

    // ---------------------------------------------------------------- status

    private void onLanded(DataManifest.Pack p) {
        landed.add(p.installAs);
        String wanted = want;
        if (wanted != null && install.missingNeeds(wanted).isEmpty()) {
            // The wanted map is whole: the engine sees its bsp, and the
            // status no longer speaks of a want.
            setWant(null);
        }
        Host h = host;
        if (h != null) {
            h.landed(p.installAs);
        }
        writeStatus();
    }

    private void setState(String s, String text) {
        if (!s.equals(state)) {
            env.log("Data download: " + s + (text != null ? " (" + text + ")" : ""));
        }
        message = text;
        state = s;
        writeStatus();
    }

    String state() {
        return state;
    }

    String message() {
        return message;
    }

    /** {done, total} bytes of the whole set. */
    long[] setProgress() {
        DataManifest m = install.manifest();
        DataFetch f = fetch;
        if (m == null || f == null) {
            return new long[] { 0, 0 };
        }
        long total = m.totalBytes();
        return new long[] { install.legacyCurrent() ? total : f.doneBytes(m.packs), total };
    }

    /**
     * Sets the state for a moment when nothing runs: before the engine
     * starts, so it reads the truth and not what a killed process left.
     * Without a manifest the file is removed (the engine reads it "while it
     * exists").
     */
    void publishIdle(boolean mayFetch) {
        if (backgroundThread == null) {
            if (install.manifest() == null) {
                state = null;
            } else if (install.legacyCurrent()) {
                state = DataStatus.LEGACY;
            } else if (install.complete()) {
                state = DataStatus.COMPLETE;
            } else {
                state = mayFetch ? DataStatus.FETCHING : DataStatus.WAITING;
            }
            message = null;
        }
        writeStatus();
    }

    synchronized void writeStatus() {
        DataManifest m = install.manifest();
        DataFetch f = fetch;
        String s = state;
        if (m == null || f == null || s == null) {
            if (lastStatus != null || statusFile.exists()) {
                statusFile.delete();
                lastStatus = null;
            }
            return;
        }
        long[] progress = setProgress();
        String wanted = want;
        List<DataManifest.Pack> needs = wantNeeds;
        long wantTotal = 0;
        for (DataManifest.Pack p : needs) {
            wantTotal += p.size;
        }
        List<String> arrived;
        synchronized (landed) {
            arrived = new ArrayList<>(landed);
        }
        String text = DataStatus.text(m.set, s, progress[0], progress[1], wanted,
                wanted != null ? f.doneBytes(needs) : 0, wantTotal, message, arrived);
        if (text.equals(lastStatus)) {
            return;
        }
        try {
            DataStatus.writeAtomic(statusFile, text, false);
            lastStatus = text;
        } catch (IOException e) {
            env.log("Could not write " + statusFile + ": " + e.getMessage());
        }
    }
}
