package io.github.dixonsolutions.xonotictouch;

import android.os.StatFs;
import android.util.Log;

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
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicIntegerArray;
import java.util.concurrent.atomic.AtomicLong;
import java.util.zip.CRC32;
import java.util.zip.DataFormatException;
import java.util.zip.Inflater;

/**
 * The game's pk3s, taken straight out of the autobuild's release zip over HTTP
 * Range requests. The zip itself is never stored: its central directory comes
 * from its last bytes, and each pk3 is fetched from its own byte range into its
 * final file (under {@code .part} until whole and checked against the CRC32 the
 * directory gives). The peak storage is the pk3s themselves, about 1 GB, where
 * staging the archive and then unzipping it needed twice that.
 *
 * A stored entry (all the big pk3s) is cut into 8 MB chunks that {@link
 * #CONNECTIONS} connections fetch at once, each writing at its own offset. How
 * far each chunk got is saved beside the part file ({@code .part.state}), so a
 * Cancel, a dropped connection or a killed process resumes from the bytes on
 * disk. A deflated entry (the two small font pk3s) is inflated as one stream
 * and starts over after a break.
 *
 * {@link #META} pins the archive (its ETag and size) while a setup runs and
 * lists the packs that setup finished. A new build on the server discards the
 * parts and the packs this setup already finished rather than mixing two
 * builds, and every range request carries {@code If-Range} so a build that
 * changes mid-download is caught on the next request.
 *
 * The idea follows Insanotic's PackDownloader; this is our own implementation.
 */
final class PackFetch {

    private static final String TAG = "Godnotic";

    /** Parallel connections: 8 keep a fast link full; on a slow one they cost nothing over 4. */
    static final int CONNECTIONS = 8;
    static final long CHUNK = 8L * 1024 * 1024;
    /** Room for the filesystem, the slim bundle's later updates and the engine's own files. */
    static final long SLACK_BYTES = 64L * 1024 * 1024;
    /** Consecutive failures on one chunk before the run gives up (about two minutes of no network). */
    private static final int CHUNK_ATTEMPTS = 10;
    /** The setup in progress: the archive it is pinned to, what it wants, what it finished. */
    static final String META = ".packs-setup";

    /** One pk3 in the archive's central directory. */
    static final class Entry {
        final String fileName;
        final int method;
        final long crc;
        final long csize;
        final long usize;
        final long offset;
        /** Where the data starts (after the local header); -1 until asked. */
        long dataStart = -1;

        Entry(String fileName, int method, long crc, long csize, long usize, long offset) {
            this.fileName = fileName;
            this.method = method;
            this.crc = crc;
            this.csize = csize;
            this.usize = usize;
            this.offset = offset;
        }

        boolean isMusic() {
            return fileName.contains("-music");
        }

        /** The name with its build date as a wildcard: which installed file plays this part. */
        String role() {
            return PackFetch.role(fileName);
        }
    }

    /** The space a run needs and what it would fetch. */
    static final class Need {
        long fetch;
        long disk;
        long resumed;
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

    /** The archive changed on the server while this run was using it. */
    static final class ArchiveChanged extends IOException {
        ArchiveChanged() {
            super("The download server published a new build. Retry starts over with it.");
        }
    }

    /** The archive as a plan: every pk3 under its data/ folder, and its identity. */
    static final class Plan {
        final String url;
        final String auth;
        final String identity;
        final String etag;
        final List<Entry> entries;

        Plan(String url, String auth, String identity, String etag, List<Entry> entries) {
            this.url = url;
            this.auth = auth;
            this.identity = identity;
            this.etag = etag;
            this.entries = entries;
        }

        Entry music() {
            for (Entry e : entries) {
                if (e.isMusic()) {
                    return e;
                }
            }
            return null;
        }
    }

    interface Listener {
        void progress(long done, long total, String file, boolean verifying);
    }

    // ------------------------------------------------------------------ plan

    /**
     * Reads the archive's central directory. {@code folder} is the zip folder
     * whose pk3s are wanted ("Xonotic/data/"); deeper entries are ignored.
     */
    static Plan plan(String url, String auth, String folder) throws IOException {
        // The size first, then the tail by explicit offsets. A suffix range
        // ("bytes=-65557") would do both in one request, but GitHub's release
        // storage answers it with 501, and that is where the data lives.
        HttpURLConnection c = open(url, auth, "bytes=0-0", null);
        long length;
        String etag;
        try {
            int code = c.getResponseCode();
            if (code != HttpURLConnection.HTTP_PARTIAL) {
                throw new IOException("The download server does not support partial downloads (HTTP " + code + ")");
            }
            length = totalOf(c.getHeaderField("Content-Range"));
            etag = c.getHeaderField("ETag");
        } finally {
            c.disconnect();
        }
        if (length <= 0) {
            throw new IOException("No size from " + url);
        }
        // End-of-central-directory record: 22 bytes plus a comment of at most 65535.
        long tailStart = Math.max(0, length - 65557);
        byte[] tail = fetchRange(url, auth, etag, tailStart, length - 1);
        int eocd = -1;
        for (int p = tail.length - 22; p >= 0; p--) {
            if (le32(tail, p) == 0x06054b50L) {
                eocd = p;
                break;
            }
        }
        if (eocd < 0) {
            throw new IOException("Not a zip file: " + url);
        }
        long cdSize = le32(tail, eocd + 12);
        long cdOffset = le32(tail, eocd + 16);
        if (cdSize == 0xFFFFFFFFL || cdOffset == 0xFFFFFFFFL) {
            throw new IOException("zip64 archives are not supported");
        }
        byte[] cd;
        if (cdOffset >= tailStart) {
            int from = (int) (cdOffset - tailStart);
            cd = new byte[(int) cdSize];
            System.arraycopy(tail, from, cd, 0, (int) cdSize);
        } else {
            cd = fetchRange(url, auth, etag, cdOffset, cdOffset + cdSize - 1);
        }

        List<Entry> entries = new ArrayList<>();
        int p = 0;
        while (p + 46 <= cd.length && le32(cd, p) == 0x02014b50L) {
            int n = le16(cd, p + 28);
            int x = le16(cd, p + 30);
            int k = le16(cd, p + 32);
            String name = new String(cd, p + 46, n, StandardCharsets.UTF_8);
            if (name.startsWith(folder) && name.endsWith(".pk3")) {
                String file = name.substring(folder.length());
                // One level only, and never a name that could leave the folder.
                if (!file.isEmpty() && file.indexOf('/') < 0 && file.indexOf('\\') < 0
                        && !file.startsWith(".")) {
                    entries.add(new Entry(file, le16(cd, p + 10), le32(cd, p + 16),
                            le32(cd, p + 20), le32(cd, p + 24), le32(cd, p + 42)));
                }
            }
            p += 46 + n + x + k;
        }
        if (entries.isEmpty()) {
            throw new IOException("No pk3 in " + url);
        }
        return new Plan(url, auth, etag + "\n" + length, etag, entries);
    }

    // ------------------------------------------------------------------- run

    private final File dir;
    private final Plan plan;
    private final Listener listener;
    private final AtomicLong done = new AtomicLong();
    private long total;
    private volatile boolean stopped;
    private final List<HttpURLConnection> open = new ArrayList<>();

    PackFetch(File dir, Plan plan, Listener listener) {
        this.dir = dir;
        this.plan = plan;
        this.listener = listener;
    }

    /** The wanted entries that no installed file plays yet. */
    static List<Entry> todo(File dir, Plan plan, boolean music) {
        List<Entry> out = new ArrayList<>();
        for (Entry e : plan.entries) {
            if ((music || !e.isMusic()) && !installed(dir, e.role())) {
                out.add(e);
            }
        }
        return out;
    }

    /** What fetching {@code todo} costs in download and in storage, counting resumable parts. */
    static Need need(File dir, List<Entry> todo, Plan plan) {
        Need need = new Need();
        boolean same = plan.identity.equals(Meta.read(dir).identity);
        for (Entry e : todo) {
            long resumable = same && e.method == 0
                    ? ChunkState.load(stateFile(dir, e), partFile(dir, e), e.csize, false).total() : 0;
            need.fetch += e.csize - resumable;
            need.disk += e.usize - resumable;
            need.resumed += resumable;
        }
        return need;
    }

    static long freeBytes(File dir) {
        try {
            return new StatFs(dir.getPath()).getAvailableBytes();
        } catch (IllegalArgumentException e) {
            return Long.MAX_VALUE;
        }
    }

    /** Fetches the wanted packs; returns when all are installed. Interrupt the thread to stop. */
    void run(boolean music) throws IOException {
        Meta meta = Meta.read(dir);
        if (!plan.identity.equals(meta.identity)) {
            // A different build than the one the parts and finished packs came from.
            discard(meta);
            meta = new Meta(plan.identity);
        }
        List<Entry> todo = todo(dir, plan, music);
        for (Entry e : plan.entries) {
            if (music || !e.isMusic()) {
                meta.want.add(e.role());
            }
        }
        if (todo.isEmpty()) {
            Meta.clear(dir);
            return;
        }
        Need need = need(dir, todo, plan);
        long free = freeBytes(dir);
        if (free < need.disk + SLACK_BYTES) {
            throw new NoSpace(need.disk + SLACK_BYTES, free);
        }
        meta.write(dir);

        total = need.fetch + need.resumed;
        done.set(need.resumed);
        List<Job> jobs = new ArrayList<>();
        List<Pack> packs = new ArrayList<>();
        try {
            for (Entry e : todo) {
                Pack pack = new Pack(e, meta);
                packs.add(pack);
                jobs.addAll(pack.jobs());
            }
            runJobs(jobs, packs);
        } finally {
            for (Pack pack : packs) {
                pack.close();
            }
        }
        // Everything wanted is there now.
        Meta.clear(dir);
    }

    private void runJobs(List<Job> jobs, List<Pack> packs) throws IOException {
        ExecutorService pool = Executors.newFixedThreadPool(CONNECTIONS, runnable -> {
            Thread thread = new Thread(runnable, "xonotic-pack");
            thread.setDaemon(true);
            return thread;
        });
        Thread saver = new Thread(() -> {
            while (!stopped) {
                try {
                    Thread.sleep(1000);
                } catch (InterruptedException e) {
                    return;
                }
                for (Pack pack : packs) {
                    pack.save();
                }
                report(null, false);
            }
        }, "xonotic-pack-state");
        saver.setDaemon(true);
        saver.start();
        List<Future<?>> futures = new ArrayList<>();
        try {
            for (Job job : jobs) {
                futures.add(pool.submit(() -> {
                    job.run();
                    return null;
                }));
            }
            for (Future<?> f : futures) {
                f.get();
            }
        } catch (InterruptedException e) {
            stopped = true;
            Thread.currentThread().interrupt();
            throw new InterruptedIOException("Download cancelled");
        } catch (ExecutionException e) {
            stopped = true;
            Throwable cause = e.getCause();
            if (cause instanceof IOException) {
                throw (IOException) cause;
            }
            throw new IOException(String.valueOf(cause), cause);
        } finally {
            stopped = true;
            pool.shutdownNow();
            saver.interrupt();
            // A read blocked in a socket does not see the interrupt: close the
            // connections, and wait for the threads, so a Resume never has two
            // writers on one part file.
            synchronized (open) {
                for (HttpURLConnection c : open) {
                    c.disconnect();
                }
            }
            try {
                if (!pool.awaitTermination(15, TimeUnit.SECONDS)) {
                    Log.w(TAG, "Pack threads still running after stop");
                }
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            for (Pack pack : packs) {
                pack.save();
            }
        }
    }

    private long lastReport;

    private void report(String file, boolean verifying) {
        listener.progress(done.get(), total, file, verifying);
    }

    private void advance(int n) {
        long now = done.addAndGet(n);
        // One report per megabyte at most; the saver reports once a second too.
        synchronized (this) {
            if (now - lastReport >= 1024 * 1024) {
                lastReport = now;
                report(null, false);
            }
        }
    }

    private void checkStopped() throws InterruptedIOException {
        if (stopped || Thread.currentThread().isInterrupted()) {
            throw new InterruptedIOException("Download cancelled");
        }
    }

    private interface Job {
        void run() throws IOException;
    }

    /** One pk3 being fetched: its part file, chunk state and the jobs that fill it. */
    private final class Pack {
        final Entry e;
        final Meta meta;
        final File part;
        ChunkState state;
        RandomAccessFile file;
        FileChannel channel;
        final AtomicInteger left = new AtomicInteger();

        Pack(Entry e, Meta meta) throws IOException {
            this.e = e;
            this.meta = meta;
            this.part = partFile(dir, e);
            if (e.method == 0) {
                state = ChunkState.load(stateFile(dir, e), part, e.csize, true);
                file = new RandomAccessFile(part, "rw");
                channel = file.getChannel();
            } else if (e.method != 8) {
                throw new IOException("Unsupported compression in " + e.fileName);
            }
        }

        List<Job> jobs() {
            List<Job> out = new ArrayList<>();
            if (e.method == 8) {
                left.set(1);
                out.add(() -> {
                    retrying(this::fetchDeflated);
                    finish();
                });
                return out;
            }
            List<Integer> chunks = new ArrayList<>();
            for (int i = 0; i < state.chunks(); i++) {
                if (state.pos(i) < state.sizeOf(i)) {
                    chunks.add(i);
                }
            }
            if (chunks.isEmpty()) {
                // Every byte arrived before the stop: only the check is left.
                left.set(1);
                out.add(this::finish);
                return out;
            }
            left.set(chunks.size());
            for (int i : chunks) {
                out.add(() -> {
                    fetchChunk(i);
                    if (left.decrementAndGet() == 0) {
                        verifyAndPublish();
                    }
                });
            }
            return out;
        }

        private void finish() throws IOException {
            if (e.method == 0) {
                verifyAndPublish();
            }
        }

        synchronized long dataStart() throws IOException {
            if (e.dataStart < 0) {
                byte[] lh = retryingFetch(e.offset, e.offset + 29);
                if (le32(lh, 0) != 0x04034b50L) {
                    throw new IOException("Bad local header for " + e.fileName);
                }
                e.dataStart = e.offset + 30 + le16(lh, 26) + le16(lh, 28);
            }
            return e.dataStart;
        }

        private byte[] retryingFetch(long from, long to) throws IOException {
            for (int attempt = 1; ; attempt++) {
                checkStopped();
                try {
                    return fetchRange(plan.url, plan.auth, plan.etag, from, to);
                } catch (ArchiveChanged changed) {
                    throw changed;
                } catch (IOException ex) {
                    // A cancel ends here; a timeout (also an InterruptedIOException) retries.
                    checkStopped();
                    if (attempt >= CHUNK_ATTEMPTS) {
                        throw ex;
                    }
                    pause(attempt);
                }
            }
        }

        private void fetchChunk(int i) throws IOException {
            long start = dataStart();
            long chunkStart = i * CHUNK;
            long size = state.sizeOf(i);
            byte[] buf = new byte[256 * 1024];
            int failures = 0;
            while (state.pos(i) < size) {
                checkStopped();
                long from = chunkStart + state.pos(i);
                HttpURLConnection c = null;
                try {
                    c = open(plan.url, plan.auth,
                            "bytes=" + (start + from) + "-" + (start + chunkStart + size - 1), plan.etag);
                    track(c, true);
                    checkResponse(c);
                    try (InputStream in = c.getInputStream()) {
                        while (state.pos(i) < size) {
                            int n = in.read(buf, 0, (int) Math.min(buf.length, size - state.pos(i)));
                            if (n < 0) {
                                throw new IOException("Connection closed early");
                            }
                            checkStopped();
                            ByteBuffer bb = ByteBuffer.wrap(buf, 0, n);
                            long at = chunkStart + state.pos(i);
                            while (bb.hasRemaining()) {
                                at += channel.write(bb, at);
                            }
                            state.advance(i, n);
                            advance(n);
                            failures = 0;
                        }
                    }
                } catch (ArchiveChanged ex) {
                    throw ex;
                } catch (IOException ex) {
                    checkStopped();
                    if (++failures >= CHUNK_ATTEMPTS) {
                        throw ex;
                    }
                    Log.w(TAG, e.fileName + " chunk " + i + ": " + ex + ", retrying");
                    pause(failures);
                } finally {
                    if (c != null) {
                        track(c, false);
                        c.disconnect();
                    }
                }
            }
        }

        private void fetchDeflated() throws IOException {
            long start = dataStart();
            long[] got = new long[1];
            CRC32 crc = new CRC32();
            HttpURLConnection c = open(plan.url, plan.auth,
                    "bytes=" + start + "-" + (start + e.csize - 1), plan.etag);
            track(c, true);
            Inflater inflater = new Inflater(true);
            try {
                checkResponse(c);
                byte[] in = new byte[64 * 1024];
                byte[] out = new byte[128 * 1024];
                try (InputStream src = c.getInputStream();
                     FileOutputStream sink = new FileOutputStream(part)) {
                    while (!inflater.finished()) {
                        if (inflater.needsInput()) {
                            int n = src.read(in, 0, (int) Math.min(in.length, e.csize - got[0]));
                            if (n <= 0) {
                                throw new IOException("Connection closed early");
                            }
                            checkStopped();
                            got[0] += n;
                            advance(n);
                            inflater.setInput(in, 0, n);
                        }
                        int m;
                        try {
                            m = inflater.inflate(out);
                        } catch (DataFormatException ex) {
                            throw new IOException("Damaged download of " + e.fileName, ex);
                        }
                        if (m > 0) {
                            sink.write(out, 0, m);
                            crc.update(out, 0, m);
                        } else if (inflater.needsDictionary()) {
                            throw new IOException("Damaged download of " + e.fileName);
                        }
                    }
                }
            } catch (IOException ex) {
                // A deflated pack starts over: take back what it counted.
                done.addAndGet(-got[0]);
                throw ex;
            } finally {
                inflater.end();
                track(c, false);
                c.disconnect();
            }
            if (part.length() != e.usize || crc.getValue() != e.crc) {
                delete(part);
                throw new IOException("Damaged download of " + e.fileName);
            }
            publish();
        }

        private void retrying(Job job) throws IOException {
            for (int attempt = 1; ; attempt++) {
                checkStopped();
                try {
                    job.run();
                    return;
                } catch (ArchiveChanged ex) {
                    throw ex;
                } catch (IOException ex) {
                    checkStopped();
                    if (attempt >= CHUNK_ATTEMPTS) {
                        throw ex;
                    }
                    Log.w(TAG, e.fileName + ": " + ex + ", retrying");
                    pause(attempt);
                }
            }
        }

        private void verifyAndPublish() throws IOException {
            state.save();
            channel.force(false);
            report(e.fileName, true);
            CRC32 crc = new CRC32();
            byte[] buf = new byte[256 * 1024];
            try (InputStream in = new FileInputStream(part)) {
                int n;
                while ((n = in.read(buf)) > 0) {
                    checkStopped();
                    crc.update(buf, 0, n);
                }
            }
            if (part.length() != e.usize || crc.getValue() != e.crc) {
                // Which chunk is bad is not known: the pack starts over.
                close();
                delete(part);
                delete(stateFile(dir, e));
                done.addAndGet(-e.csize);
                throw new IOException("Damaged download of " + e.fileName + "; Retry fetches it again");
            }
            close();
            publish();
            delete(stateFile(dir, e));
        }

        private void publish() throws IOException {
            File dest = new File(dir, e.fileName);
            if (dest.exists() && !dest.delete()) {
                throw new IOException("Cannot replace " + dest);
            }
            if (!part.renameTo(dest)) {
                throw new IOException("Cannot move " + part + " to " + dest);
            }
            synchronized (meta) {
                meta.finished.add(e.fileName);
                meta.write(dir);
            }
            Log.i(TAG, "Installed " + e.fileName + " (" + (e.usize >> 20) + " MB)");
        }

        synchronized void save() {
            if (state != null && channel != null) {
                state.save();
            }
        }

        synchronized void close() {
            if (state != null && channel != null) {
                state.save();
            }
            if (file != null) {
                try {
                    file.close();
                } catch (IOException ignored) {
                    // nothing left to flush that matters: the state says what is there
                }
                file = null;
                channel = null;
            }
        }
    }

    private void pause(int attempt) throws InterruptedIOException {
        try {
            Thread.sleep(Math.min(15_000L, 2000L * attempt));
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new InterruptedIOException("Download cancelled");
        }
    }

    private void track(HttpURLConnection c, boolean add) {
        synchronized (open) {
            if (add) {
                open.add(c);
            } else {
                open.remove(c);
            }
        }
    }

    private void checkResponse(HttpURLConnection c) throws IOException {
        int code = c.getResponseCode();
        if (code == HttpURLConnection.HTTP_OK) {
            // If-Range did not match: the archive is a different build now.
            throw new ArchiveChanged();
        }
        if (code != HttpURLConnection.HTTP_PARTIAL) {
            throw new IOException("HTTP " + code + " from the download server");
        }
        String etag = c.getHeaderField("ETag");
        if (plan.etag != null && etag != null && !plan.etag.equals(etag)) {
            throw new ArchiveChanged();
        }
    }

    /** Removes what a setup pinned to another build left: its parts and the packs it finished. */
    private void discard(Meta old) {
        File[] files = dir.listFiles();
        if (files != null) {
            for (File f : files) {
                String n = f.getName();
                if (n.endsWith(".pk3.part") || n.endsWith(".pk3.part.state")) {
                    delete(f);
                }
            }
        }
        if (old.identity == null) {
            return;
        }
        Set<String> current = new HashSet<>();
        for (Entry e : plan.entries) {
            current.add(e.fileName + "\n" + e.usize);
        }
        for (String name : old.finished) {
            File f = new File(dir, name);
            if (f.isFile() && !current.contains(name + "\n" + f.length())) {
                Log.i(TAG, "Removing " + name + ": the server has another build now");
                delete(f);
            }
        }
    }

    // ------------------------------------------------------------- the state

    /** {@link #META}: the identity line, then "want <role>" and "done <file>" lines. */
    static final class Meta {
        final String identity;
        final Set<String> want = new LinkedHashSet<>();
        final Set<String> finished = new LinkedHashSet<>();

        Meta(String identity) {
            this.identity = identity;
        }

        static Meta read(File dir) {
            File f = new File(dir, META);
            if (!f.isFile()) {
                return new Meta(null);
            }
            try {
                String[] lines = new String(readAll(new FileInputStream(f), 1 << 20), StandardCharsets.UTF_8)
                        .split("\n");
                if (lines.length < 2) {
                    return new Meta(null);
                }
                Meta meta = new Meta(lines[0] + "\n" + lines[1]);
                for (int i = 2; i < lines.length; i++) {
                    if (lines[i].startsWith("want ")) {
                        meta.want.add(lines[i].substring(5));
                    } else if (lines[i].startsWith("done ")) {
                        meta.finished.add(lines[i].substring(5));
                    }
                }
                return meta;
            } catch (IOException e) {
                return new Meta(null);
            }
        }

        void write(File dir) throws IOException {
            StringBuilder text = new StringBuilder(identity).append('\n');
            for (String w : want) {
                text.append("want ").append(w).append('\n');
            }
            for (String d : finished) {
                text.append("done ").append(d).append('\n');
            }
            File tmp = new File(dir, META + ".tmp");
            try (FileOutputStream out = new FileOutputStream(tmp)) {
                out.write(text.toString().getBytes(StandardCharsets.UTF_8));
            }
            if (!tmp.renameTo(new File(dir, META))) {
                throw new IOException("Cannot write " + META);
            }
        }

        static void clear(File dir) {
            delete(new File(dir, META));
        }

        /** Roles a setup wants that are not installed yet. */
        List<String> missing(File dir) {
            List<String> out = new ArrayList<>();
            for (String role : want) {
                if (!installed(dir, role)) {
                    out.add(role);
                }
            }
            return out;
        }
    }

    /**
     * How many bytes of each fixed-size chunk of a stored entry are on disk,
     * saved beside the part file. No state, or one that does not fit, starts
     * the entry over.
     */
    static final class ChunkState {
        private static final int MAGIC = 0x58545043; // "XTPC"
        private final File file;
        private final long csize;
        private final AtomicIntegerArray done;

        private ChunkState(File file, long csize, int count) {
            this.file = file;
            this.csize = csize;
            this.done = new AtomicIntegerArray(count);
        }

        int chunks() {
            return done.length();
        }

        long pos(int i) {
            return done.get(i);
        }

        long sizeOf(int i) {
            return Math.min(CHUNK, csize - i * CHUNK);
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

        synchronized void save() {
            File tmp = new File(file.getPath() + ".tmp");
            try (DataOutputStream out = new DataOutputStream(new FileOutputStream(tmp))) {
                out.writeInt(MAGIC);
                out.writeLong(csize);
                out.writeInt(chunks());
                for (int i = 0; i < chunks(); i++) {
                    out.writeInt(done.get(i));
                }
            } catch (IOException e) {
                Log.w(TAG, "Could not save " + file, e);
                return;
            }
            if (!tmp.renameTo(file)) {
                Log.w(TAG, "Could not save " + file);
            }
        }

        /** @param clean delete a part and state that do not belong together (false: only look) */
        static ChunkState load(File file, File part, long csize, boolean clean) {
            int count = (int) Math.max(1, (csize + CHUNK - 1) / CHUNK);
            ChunkState fresh = new ChunkState(file, csize, count);
            if (part.isFile() && file.isFile()) {
                try (DataInputStream in = new DataInputStream(new FileInputStream(file))) {
                    if (in.readInt() == MAGIC && in.readLong() == csize && in.readInt() == count) {
                        ChunkState loaded = new ChunkState(file, csize, count);
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
                delete(part);
                delete(file);
            }
            return fresh;
        }
    }

    // ------------------------------------------------------------- utilities

    static File partFile(File dir, Entry e) {
        return new File(dir, e.fileName + ".part");
    }

    static File stateFile(File dir, Entry e) {
        return new File(dir, e.fileName + ".part.state");
    }

    /** "xonotic-20261107-maps.pk3" and "xonotic-20261029_2-maps.pk3" -> "xonotic-*-maps.pk3". */
    static String role(String fileName) {
        return fileName.replaceAll("\\d{8}(_\\d+)?", "*");
    }

    /** Whether a pk3 that plays this role is in {@code dir}, whatever its build date. */
    static boolean installed(File dir, String role) {
        String[] files = dir.list();
        if (files == null) {
            return false;
        }
        for (String name : files) {
            if (name.endsWith(".pk3") && role(name).equals(role)) {
                return true;
            }
        }
        return false;
    }

    private static HttpURLConnection open(String url, String auth, String range, String ifRange)
            throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        if (auth != null) {
            c.setRequestProperty("Authorization", auth);
        }
        c.setRequestProperty("Accept-Encoding", "identity");
        c.setRequestProperty("Range", range);
        if (ifRange != null) {
            c.setRequestProperty("If-Range", ifRange);
        }
        c.setConnectTimeout(20_000);
        c.setReadTimeout(30_000);
        c.setInstanceFollowRedirects(true);
        return c;
    }

    private static byte[] fetchRange(String url, String auth, String etag, long from, long to)
            throws IOException {
        HttpURLConnection c = open(url, auth, "bytes=" + from + "-" + to, etag);
        try {
            int code = c.getResponseCode();
            if (code == HttpURLConnection.HTTP_OK) {
                throw new ArchiveChanged();
            }
            if (code != HttpURLConnection.HTTP_PARTIAL) {
                throw new IOException("HTTP " + code + " for a range request");
            }
            byte[] b = readAll(c.getInputStream(), (int) (to - from + 1));
            if (b.length != to - from + 1) {
                throw new IOException("Short range response");
            }
            return b;
        } finally {
            c.disconnect();
        }
    }

    private static byte[] readAll(InputStream in, int limit) throws IOException {
        try (InputStream src = in) {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream(Math.min(limit, 1 << 16));
            byte[] buf = new byte[16 * 1024];
            int n;
            while ((n = src.read(buf)) > 0) {
                out.write(buf, 0, n);
                if (out.size() > limit) {
                    throw new IOException("Response larger than expected");
                }
            }
            return out.toByteArray();
        }
    }

    private static long totalOf(String contentRange) {
        if (contentRange == null) {
            return -1;
        }
        int slash = contentRange.lastIndexOf('/');
        try {
            return slash < 0 ? -1 : Long.parseLong(contentRange.substring(slash + 1).trim());
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    private static void delete(File f) {
        if (f.exists() && !f.delete()) {
            Log.w(TAG, "Could not delete " + f);
        }
    }

    private static int le16(byte[] b, int o) {
        return (b[o] & 0xff) | ((b[o + 1] & 0xff) << 8);
    }

    private static long le32(byte[] b, int o) {
        return (le16(b, o) | ((long) le16(b, o + 2) << 16)) & 0xFFFFFFFFL;
    }
}
