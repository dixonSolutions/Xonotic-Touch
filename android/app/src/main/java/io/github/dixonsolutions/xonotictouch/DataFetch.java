package io.github.dixonsolutions.xonotictouch;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.io.RandomAccessFile;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.atomic.AtomicIntegerArray;

/**
 * Fetches the packs of a {@link DataManifest} out of the release archive, each
 * from its own byte range (docs/DATA_FETCH.md, "Fetching a pack").
 *
 * It works the way {@link PackFetch} does for the single archive, and writes
 * the same files: a pack goes to {@code <install_as>.part}, cut into 8 MiB
 * chunks that several connections fetch at once, each writing at its own
 * offset; how far each chunk got is saved beside the part
 * ({@code .part.state}, the XTPC format), so a stop, a lost connection or a
 * killed process carries on from the bytes on disk. A whole part is checked
 * against the manifest's size, CRC32 and SHA-256 and only then renamed
 * ({@link DataInstall#publish}). PackFetch itself is left as it is: it is the
 * path today's installs run, and the fallback when no manifest is published.
 *
 * What differs: the pack's offset is in the manifest (no zip directory is
 * read), the archive is immutable (no If-Range), and the order is not fixed.
 * Every free connection takes the next chunk of the first pack in
 * {@link DataInstall#order} that has one, so the small packs go several at a
 * time, a big one over several connections, and a map the player asks for
 * mid-run ({@link #want}) is next: chunks of other packs stop where they are
 * and are picked up again afterwards.
 *
 * Ranges are always explicit offsets. GitHub's release storage answers a
 * suffix range ("bytes=-N") with 501.
 */
final class DataFetch {

    static final long CHUNK = 8L * 1024 * 1024;
    /** Room for the filesystem, the app's later updates and the engine's own files (as PackFetch). */
    static final long SLACK_BYTES = 64L * 1024 * 1024;
    /** A part that fails its check is fetched again this many times in all. */
    static final int PACK_ATTEMPTS = 3;

    /** What the fetch needs from where it runs; the JVM tests pass their own. */
    interface Env {
        void log(String line);

        long freeBytes(File dir);
    }

    /** Asked before every chunk: false while the network may not be used (offline, or metered without consent). */
    interface Gate {
        boolean mayFetch();
    }

    interface Listener {
        /** A pack was verified and renamed into place; called on a fetch thread. */
        void landed(DataManifest.Pack pack);
    }

    static final class Options {
        long chunk = CHUNK;
        /** Consecutive failures on one chunk before the run gives up (about two minutes of no network). */
        int chunkAttempts = 10;
        long pauseMs = 2000;
        int connectTimeoutMs = 20_000;
        int readTimeoutMs = 30_000;
        long slackBytes = SLACK_BYTES;
    }

    static final class NoSpace extends IOException {
        final long needed;
        final long available;

        NoSpace(long needed, long available) {
            super("Not enough free storage: needs " + (needed >> 20) + " MB, " + (available >> 20) + " MB free");
            this.needed = needed;
            this.available = available;
        }
    }

    /** The gate closed: not a failure, the run carries on when the network may be used again. */
    static final class Waiting extends IOException {
        Waiting() {
            super("Waiting for a network the download may use");
        }
    }

    private final DataInstall install;
    private final DataManifest manifest;
    private final File dir;
    private final Env env;
    private final Options opt;
    private final Listener listener;

    /** Guards everything below it: the queue, the open packs, the counters. */
    private final Object lock = new Object();
    private final Map<String, Active> active = new HashMap<>();
    /** Bytes on disk of packs that are not open: read from their state files when first asked. */
    private final Map<String, Long> resumable = new HashMap<>();
    private final Map<String, Integer> attempts = new HashMap<>();
    private final Set<String> givenUp = new HashSet<>();
    /**
     * Packs whose part is being checked and renamed. From the moment a
     * thread takes that on until the pack is installed (or its part
     * deleted), no other thread may open the same part: it would find it
     * whole and check and rename it a second time.
     */
    private final Set<String> finishing = new HashSet<>();
    private final List<HttpURLConnection> open = new ArrayList<>();
    private int running;
    private boolean stopped = true;
    private IOException fatal;
    private String wantMap;
    private Set<String> wantPacks = new HashSet<>();
    private int wantGeneration;
    private volatile String verifying;

    DataFetch(DataInstall install, DataManifest manifest, Env env, Options opt, Listener listener) {
        this.install = install;
        this.manifest = manifest;
        this.dir = install.dir();
        this.env = env;
        this.opt = opt;
        this.listener = listener;
    }

    // ------------------------------------------------------------------ want

    /** The map to fetch next (null: none). Chunks of other packs stop where they are. */
    void want(String map) {
        synchronized (lock) {
            wantMap = map;
            wantPacks = new HashSet<>();
            List<DataManifest.Pack> missing = install.missingNeeds(map);
            for (DataManifest.Pack p : missing) {
                wantPacks.add(p.name);
            }
            // Asked for by the player: worth another three tries.
            forgive(missing);
            wantGeneration++;
            lock.notifyAll();
        }
    }

    /**
     * Lets packs that were given up on be tried again. Without it they stay
     * given up for the life of the process: an archive that does not match
     * its manifest must not be downloaded over and over.
     */
    void forgive(List<DataManifest.Pack> packs) {
        synchronized (lock) {
            for (DataManifest.Pack p : packs) {
                givenUp.remove(p.name);
                attempts.remove(p.name);
            }
        }
    }

    // -------------------------------------------------------------- progress

    /** Bytes of {@code packs} that are installed or on disk in a part. */
    long doneBytes(List<DataManifest.Pack> packs) {
        long sum = 0;
        for (DataManifest.Pack p : packs) {
            sum += install.installed(p) ? p.size : partBytes(p);
        }
        return sum;
    }

    private long partBytes(DataManifest.Pack p) {
        synchronized (lock) {
            Active a = active.get(p.name);
            if (a != null) {
                return a.state.total();
            }
            Long known = resumable.get(p.name);
            if (known == null) {
                known = Chunks.load(stateFile(p), partFile(p), p.size, opt.chunk, false).total();
                resumable.put(p.name, known);
            }
            return known;
        }
    }

    /** The install name of the pack being checked right now, or null. */
    String verifying() {
        return verifying;
    }

    /** Free space the packs of {@code scope} still need; throws when the device has less. */
    void checkSpace(List<DataManifest.Pack> scope) throws NoSpace {
        long need = 0;
        for (DataManifest.Pack p : scope) {
            if (!install.installed(p)) {
                need += p.size - partBytes(p);
            }
        }
        long free = env.freeBytes(dir);
        if (need > 0 && free < need + opt.slackBytes) {
            throw new NoSpace(need + opt.slackBytes, free);
        }
    }

    // ------------------------------------------------------------------- run

    /**
     * Fetches what {@link DataInstall#order} lists until nothing of it can be
     * fetched any more. Returns the packs given up on (three bad checks, in
     * this run or an earlier one of this process; see {@link #forgive});
     * throws when the network fails for good ({@link Options#chunkAttempts}),
     * the gate closes ({@link Waiting}), storage runs out ({@link NoSpace}) or
     * the calling thread is interrupted (InterruptedIOException: a stop).
     * One run at a time per data directory; the caller sees to that.
     */
    List<DataManifest.Pack> run(boolean firstPlayOnly, int connections, Gate gate) throws IOException {
        synchronized (lock) {
            stopped = false;
            fatal = null;
            running = 0;
            finishing.clear();
        }
        List<Thread> threads = new ArrayList<>();
        for (int i = 0; i < connections; i++) {
            Thread t = new Thread(() -> work(firstPlayOnly, gate), "godnotic-pack-" + i);
            t.setDaemon(true);
            threads.add(t);
        }
        Thread saver = new Thread(this::saveLoop, "godnotic-pack-state");
        saver.setDaemon(true);
        threads.add(saver);
        boolean interrupted = false;
        try {
            for (Thread t : threads) {
                t.start();
            }
            for (int i = 0; i < connections; i++) {
                threads.get(i).join();
            }
        } catch (InterruptedException e) {
            interrupted = true;
        } finally {
            // A read blocked in a socket sees nothing of this: close the
            // connections and wait for the threads, so the next run never
            // has two writers on one part file.
            synchronized (lock) {
                stopped = true;
                lock.notifyAll();
                for (HttpURLConnection c : open) {
                    c.disconnect();
                }
            }
            long deadline = System.currentTimeMillis() + 15_000;
            for (Thread t : threads) {
                boolean again = true;
                while (again) {
                    again = false;
                    try {
                        t.join(Math.max(1, deadline - System.currentTimeMillis()));
                    } catch (InterruptedException e) {
                        interrupted = true;
                        again = System.currentTimeMillis() < deadline;
                    }
                }
                if (t.isAlive()) {
                    env.log("A pack thread is still running after the stop");
                }
            }
            synchronized (lock) {
                for (Active a : new ArrayList<>(active.values())) {
                    close(a);
                }
            }
        }
        if (interrupted) {
            Thread.currentThread().interrupt();
            throw new InterruptedIOException("Download stopped");
        }
        synchronized (lock) {
            if (fatal != null) {
                throw fatal;
            }
            List<DataManifest.Pack> failed = new ArrayList<>();
            for (DataManifest.Pack p : manifest.packs) {
                if (givenUp.contains(p.name)) {
                    failed.add(p);
                }
            }
            return failed;
        }
    }

    private void work(boolean firstPlayOnly, Gate gate) {
        try {
            while (true) {
                Job job = next(firstPlayOnly, gate);
                if (job == null) {
                    return;
                }
                boolean whole = false;
                try {
                    whole = job.chunk < 0 || fetchChunk(job.pack, job.chunk, job.generation, gate);
                } finally {
                    synchronized (lock) {
                        if (job.chunk >= 0) {
                            job.pack.claimed[job.chunk] = false;
                        } else {
                            job.pack.checkClaimed = false;
                        }
                        // The part this job was handed may have been finished
                        // and closed by another thread since.
                        whole = whole && active.get(job.pack.pack.name) == job.pack;
                        // A map's own pack that is whole before the other packs of
                        // its map waits as a part: next() hands out its check
                        // once they are in.
                        if (whole && job.pack.state.complete() && !finishing.contains(job.pack.pack.name)
                                && install.ready(job.pack.pack)) {
                            finishing.add(job.pack.pack.name);
                        } else {
                            whole = false;
                        }
                        if (!whole) {
                            running--;
                            lock.notifyAll();
                        }
                    }
                }
                if (whole) {
                    try {
                        finish(job.pack);
                    } finally {
                        synchronized (lock) {
                            finishing.remove(job.pack.pack.name);
                            running--;
                            lock.notifyAll();
                        }
                    }
                }
            }
        } catch (IOException | RuntimeException e) {
            synchronized (lock) {
                if (fatal == null && !stopped) {
                    fatal = e instanceof IOException ? (IOException) e : new IOException(String.valueOf(e), e);
                }
                stopped = true;
                lock.notifyAll();
                for (HttpURLConnection c : open) {
                    c.disconnect();
                }
            }
        }
    }

    private static final class Job {
        final Active pack;
        /** The chunk to fetch; -1 when every byte is on disk and only the check is left. */
        final int chunk;
        final int generation;

        Job(Active pack, int chunk, int generation) {
            this.pack = pack;
            this.chunk = chunk;
            this.generation = generation;
        }
    }

    /** The next piece of work, waiting while others' work may free some; null when there is none left. */
    private Job next(boolean firstPlayOnly, Gate gate) throws IOException {
        synchronized (lock) {
            while (true) {
                if (stopped) {
                    return null;
                }
                if (!gate.mayFetch()) {
                    throw new Waiting();
                }
                for (DataManifest.Pack p : install.order(wantMap, firstPlayOnly)) {
                    if (givenUp.contains(p.name) || finishing.contains(p.name)) {
                        continue;
                    }
                    Active a = active.get(p.name);
                    if (a == null) {
                        a = open(p);
                    }
                    int chunk = a.claim();
                    // Its bytes may come at any time; the check and the rename
                    // only when the pack may be installed (a map's own pack last).
                    if (chunk == Active.ONLY_CHECK && (a.checkClaimed || !install.ready(p))) {
                        continue;
                    }
                    if (chunk >= 0 || chunk == Active.ONLY_CHECK) {
                        a.checkClaimed = chunk == Active.ONLY_CHECK;
                        running++;
                        return new Job(a, chunk == Active.ONLY_CHECK ? -1 : chunk, wantGeneration);
                    }
                }
                if (running == 0) {
                    // Nothing in flight that could make a waiting map pack ready.
                    return null;
                }
                try {
                    lock.wait(1000);
                } catch (InterruptedException e) {
                    throw new InterruptedIOException("Download stopped");
                }
            }
        }
    }

    /** Another map was asked for since this chunk started, and this pack is not one it needs. */
    private boolean preempted(Active a, int generation) {
        synchronized (lock) {
            return generation != wantGeneration && !wantPacks.isEmpty() && !wantPacks.contains(a.pack.name);
        }
    }

    private void checkStopped() throws InterruptedIOException {
        synchronized (lock) {
            if (stopped) {
                throw new InterruptedIOException("Download stopped");
            }
        }
    }

    // ----------------------------------------------------------------- packs

    /** One pack being fetched: its part file, its chunk state and which chunks someone is on. */
    private static final class Active {
        static final int NOTHING = -1;
        static final int ONLY_CHECK = -2;

        final DataManifest.Pack pack;
        final File part;
        final Chunks state;
        final boolean[] claimed;
        RandomAccessFile file;
        FileChannel channel;
        /** A thread has taken the check of this (whole) part. */
        boolean checkClaimed;

        Active(DataManifest.Pack pack, File part, Chunks state) {
            this.pack = pack;
            this.part = part;
            this.state = state;
            this.claimed = new boolean[state.chunks()];
        }

        int claim() {
            boolean busy = false;
            for (int i = 0; i < claimed.length; i++) {
                if (claimed[i]) {
                    busy = true;
                } else if (state.pos(i) < state.sizeOf(i)) {
                    claimed[i] = true;
                    return i;
                }
            }
            // Every byte arrived before a stop: only the check is left.
            return !busy && state.complete() ? ONLY_CHECK : NOTHING;
        }
    }

    /** Opens the part of {@code p}, resuming what is there. Called with the lock held. */
    private Active open(DataManifest.Pack p) throws IOException {
        File part = partFile(p);
        Chunks state = Chunks.load(stateFile(p), part, p.size, opt.chunk, true);
        long need = p.size - state.total();
        long committed = 0;
        for (Active other : active.values()) {
            committed += other.pack.size - other.state.total();
        }
        long free = env.freeBytes(dir);
        if (free < committed + need + opt.slackBytes) {
            throw new NoSpace(committed + need + opt.slackBytes, free);
        }
        Active a = new Active(p, part, state);
        a.file = new RandomAccessFile(part, "rw");
        a.channel = a.file.getChannel();
        active.put(p.name, a);
        resumable.remove(p.name);
        return a;
    }

    /** Saves the state and closes the part. Called with the lock held. */
    private void close(Active a) {
        if (a.channel != null) {
            a.state.save(env);
            try {
                a.file.close();
            } catch (IOException ignored) {
                // nothing left to flush that matters: the state says what is there
            }
            a.file = null;
            a.channel = null;
        }
        active.remove(a.pack.name);
    }

    private void saveLoop() {
        synchronized (lock) {
            while (!stopped) {
                try {
                    lock.wait(1000);
                } catch (InterruptedException e) {
                    return;
                }
                for (Active a : active.values()) {
                    if (a.channel != null) {
                        a.state.save(env);
                    }
                }
            }
        }
    }

    /** Fetches what is left of one chunk. False when it stopped early for a wanted map. */
    private boolean fetchChunk(Active a, int i, int generation, Gate gate) throws IOException {
        DataManifest.Pack p = a.pack;
        long chunkStart = i * opt.chunk;
        long size = a.state.sizeOf(i);
        byte[] buf = new byte[128 * 1024];
        int failures = 0;
        while (a.state.pos(i) < size) {
            checkStopped();
            if (preempted(a, generation)) {
                return false;
            }
            long from = p.offset + chunkStart + a.state.pos(i);
            long to = p.offset + chunkStart + size - 1;
            HttpURLConnection c = null;
            try {
                c = connect(from, to);
                track(c, true);
                checkResponse(c, from, to);
                try (InputStream in = c.getInputStream()) {
                    while (a.state.pos(i) < size) {
                        int n = in.read(buf, 0, (int) Math.min(buf.length, size - a.state.pos(i)));
                        if (n < 0) {
                            throw new IOException("Connection closed early");
                        }
                        checkStopped();
                        ByteBuffer bb = ByteBuffer.wrap(buf, 0, n);
                        long at = chunkStart + a.state.pos(i);
                        while (bb.hasRemaining()) {
                            at += a.channel.write(bb, at);
                        }
                        a.state.advance(i, n);
                        failures = 0;
                        if (preempted(a, generation)) {
                            return false;
                        }
                    }
                }
            } catch (IOException ex) {
                // A stop ends here; a timeout (also an InterruptedIOException) retries.
                checkStopped();
                if (!gate.mayFetch()) {
                    throw new Waiting();
                }
                if (++failures >= opt.chunkAttempts) {
                    throw ex;
                }
                env.log(p.installAs + " chunk " + i + ": " + ex + ", retrying");
                pause(failures);
            } finally {
                if (c != null) {
                    track(c, false);
                    c.disconnect();
                }
            }
        }
        return true;
    }

    /** Every chunk is on disk: check the part, then rename it into place or start it over. */
    private void finish(Active a) throws IOException {
        DataManifest.Pack p = a.pack;
        boolean good;
        FileChannel channel;
        synchronized (lock) {
            channel = a.channel;
            if (channel == null) {
                return; // closed by a stop
            }
            a.state.save(env);
        }
        channel.force(false);
        verifying = p.installAs;
        try {
            good = DataInstall.verify(a.part, p);
        } finally {
            verifying = null;
        }
        synchronized (lock) {
            close(a);
            if (stopped) {
                // The bytes and their state stay: the next run checks them again.
                return;
            }
            if (!good) {
                // Which chunk is bad is not known: the pack starts over.
                delete(a.part);
                delete(stateFile(p));
                Integer before = attempts.get(p.name);
                int tries = (before == null ? 0 : before) + 1;
                attempts.put(p.name, tries);
                if (tries >= PACK_ATTEMPTS) {
                    givenUp.add(p.name);
                    env.log(p.installAs + " failed its check " + tries + " times; leaving it for the next start");
                } else {
                    env.log(p.installAs + " failed its check; fetching it again");
                }
                return;
            }
        }
        install.publish(p, a.part);
        delete(stateFile(p));
        delete(new File(stateFile(p).getPath() + ".tmp"));
        listener.landed(p);
    }

    private void pause(int attempt) throws InterruptedIOException {
        long until = System.currentTimeMillis() + Math.min(15_000L, opt.pauseMs * attempt);
        synchronized (lock) {
            while (!stopped) {
                long left = until - System.currentTimeMillis();
                if (left <= 0) {
                    return;
                }
                try {
                    lock.wait(left);
                } catch (InterruptedException e) {
                    throw new InterruptedIOException("Download stopped");
                }
            }
        }
        throw new InterruptedIOException("Download stopped");
    }

    // ------------------------------------------------------------------ HTTP

    private HttpURLConnection connect(long from, long to) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(manifest.archiveUrl).openConnection();
        c.setRequestProperty("Accept-Encoding", "identity");
        c.setRequestProperty("Range", "bytes=" + from + "-" + to);
        c.setConnectTimeout(opt.connectTimeoutMs);
        c.setReadTimeout(opt.readTimeoutMs);
        c.setInstanceFollowRedirects(true);
        return c;
    }

    /**
     * 206 with the range that was asked for, or it is an error for this
     * attempt: a 200 would be the whole archive from its first byte, written
     * where a pack's chunk belongs.
     */
    private void checkResponse(HttpURLConnection c, long from, long to) throws IOException {
        int code = c.getResponseCode();
        if (code != HttpURLConnection.HTTP_PARTIAL) {
            throw new IOException("HTTP " + code + " from the download server, not the range asked for");
        }
        String range = c.getHeaderField("Content-Range");
        if (!rangeMatches(range, from, to, manifest.archiveSize)) {
            throw new IOException("The download server answered with another range: " + range);
        }
    }

    /** "bytes 100-199/1000" is {@code from}-{@code to} of an archive of {@code total} bytes ("*" for the total passes). */
    static boolean rangeMatches(String contentRange, long from, long to, long total) {
        if (contentRange == null) {
            return false;
        }
        String s = contentRange.trim();
        if (!s.regionMatches(true, 0, "bytes", 0, 5)) {
            return false;
        }
        s = s.substring(5).trim();
        int dash = s.indexOf('-');
        int slash = s.indexOf('/');
        if (dash <= 0 || slash < dash) {
            return false;
        }
        try {
            if (Long.parseLong(s.substring(0, dash).trim()) != from
                    || Long.parseLong(s.substring(dash + 1, slash).trim()) != to) {
                return false;
            }
            String size = s.substring(slash + 1).trim();
            return size.equals("*") || Long.parseLong(size) == total;
        } catch (NumberFormatException e) {
            return false;
        }
    }

    private void track(HttpURLConnection c, boolean add) {
        synchronized (lock) {
            if (add) {
                open.add(c);
            } else {
                open.remove(c);
            }
        }
    }

    // ------------------------------------------------------------- the state

    private File partFile(DataManifest.Pack p) {
        return new File(dir, p.installAs + ".part");
    }

    private File stateFile(DataManifest.Pack p) {
        return new File(dir, p.installAs + ".part.state");
    }

    private void delete(File f) {
        if (f.exists() && !f.delete()) {
            env.log("Could not delete " + f);
        }
    }

    /**
     * How many bytes of each fixed-size chunk of a part are on disk, saved
     * beside it: PackFetch.ChunkState's file, byte for byte (magic "XTPC",
     * the size, the chunk count, one int per chunk). No state, or one that
     * does not fit, starts the pack over.
     */
    static final class Chunks {
        private static final int MAGIC = 0x58545043; // "XTPC"
        private final File file;
        private final long size;
        private final long chunk;
        private final AtomicIntegerArray done;

        private Chunks(File file, long size, long chunk, int count) {
            this.file = file;
            this.size = size;
            this.chunk = chunk;
            this.done = new AtomicIntegerArray(count);
        }

        int chunks() {
            return done.length();
        }

        long pos(int i) {
            return done.get(i);
        }

        long sizeOf(int i) {
            return Math.min(chunk, size - i * chunk);
        }

        void advance(int i, int n) {
            done.addAndGet(i, n);
        }

        long total() {
            long sum = 0;
            for (int i = 0; i < chunks(); i++) {
                sum += pos(i);
            }
            return sum;
        }

        boolean complete() {
            return total() == size;
        }

        synchronized void save(Env env) {
            File tmp = new File(file.getPath() + ".tmp");
            try (DataOutputStream out = new DataOutputStream(new FileOutputStream(tmp))) {
                out.writeInt(MAGIC);
                out.writeLong(size);
                out.writeInt(chunks());
                for (int i = 0; i < chunks(); i++) {
                    out.writeInt(done.get(i));
                }
            } catch (IOException e) {
                env.log("Could not save " + file + ": " + e);
                return;
            }
            if (!tmp.renameTo(file)) {
                env.log("Could not save " + file);
            }
        }

        /** @param clean delete a part and state that do not belong together (false: only look) */
        static Chunks load(File file, File part, long size, long chunk, boolean clean) {
            int count = (int) Math.max(1, (size + chunk - 1) / chunk);
            if (part.isFile() && file.isFile()) {
                try (DataInputStream in = new DataInputStream(new FileInputStream(file))) {
                    if (in.readInt() == MAGIC && in.readLong() == size && in.readInt() == count) {
                        Chunks loaded = new Chunks(file, size, chunk, count);
                        boolean ok = true;
                        for (int i = 0; i < count; i++) {
                            int p = in.readInt();
                            if (p < 0 || p > loaded.sizeOf(i)) {
                                ok = false;
                                break;
                            }
                            loaded.done.set(i, p);
                        }
                        if (ok) {
                            return loaded;
                        }
                    }
                } catch (IOException ignored) {
                    // starts over below
                }
            }
            if (clean) {
                if (part.exists() && !part.delete()) {
                    // A part that cannot go would be resumed as if it were empty.
                    part.deleteOnExit();
                }
                if (file.exists()) {
                    file.delete();
                }
            }
            return new Chunks(file, size, chunk, count);
        }
    }
}
