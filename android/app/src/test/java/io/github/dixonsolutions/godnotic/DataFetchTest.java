package io.github.dixonsolutions.godnotic;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;

import java.io.DataInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Random;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.zip.CRC32;

/**
 * The progressive data download (docs/DATA_FETCH.md) against a local HTTP
 * range server and small synthetic archives: the manifest's checks, the two
 * phases, resume, a damaged pack, the old layout, a new set, wanted maps and
 * the files the engine reads and writes. No Android class is involved.
 */
public class DataFetchTest {

    private static final int CHUNK = 64 * 1024;

    private File root;
    private File data;
    private File touch;
    private Server server;
    private final List<String> log = Collections.synchronizedList(new ArrayList<String>());
    private volatile long free = Long.MAX_VALUE;

    private final DataFetch.Env env = new DataFetch.Env() {
        @Override
        public void log(String line) {
            log.add(line);
        }

        @Override
        public long freeBytes(File dir) {
            return free;
        }
    };

    @Before
    public void setUp() throws IOException {
        root = Files.createTempDirectory("datafetch").toFile();
        data = new File(root, "data");
        touch = new File(root, "userdata/data/data/touch");
        assertTrue(data.mkdirs());
        server = new Server();
    }

    @After
    public void tearDown() {
        server.stop();
        // Whatever a test did, it never asked for a suffix range.
        assertEquals("suffix ranges sent", 0, server.suffixRanges.get());
        delete(root);
    }

    // ------------------------------------------------------------- manifest

    @Test
    public void manifestIsAcceptedOnlyWhenEverythingHolds() throws Exception {
        Fixture f = standard("A");
        DataManifest m = DataManifest.parse(f.json);
        assertEquals("A", m.set);
        assertEquals(7, m.packs.size());
        assertEquals(Arrays.asList("core", "shared-1", "map-alpha"), names(m.firstPlay));
        assertEquals(f.archive.length, m.archiveSize);
        assertEquals("map-beta", m.ownPack.get("beta").name);

        assertInvalid(f.json.replace("\"format\": 1", "\"format\": 2"));
        assertInvalid(f.json.replace("\"format\": 1", "\"format\": \"1\""));
        assertInvalid(f.json.replace("\"set\": \"A\"", "\"set\": \"\""));
        assertInvalid(f.json.replace("\"set\": \"A\",", ""));
        assertInvalid(f.json.replace("http://", "ftp://"));
        assertInvalid(f.json.replace("\"size\": " + f.archive.length, "\"size\": 0"));
        assertInvalid(f.json.replace("godnotic-T-core.pk3", "../godnotic-T-core.pk3"));
        assertInvalid(f.json.replace("godnotic-T-core.pk3", "xonotic-T-core.pk3"));
        assertInvalid(f.json.replace("godnotic-T-core.pk3", "godnotic-T-core.pk3.part"));
        assertInvalid(f.json.replace("godnotic-T-core.pk3", "godnotic-T-music.pk3")); // two packs, one file
        assertInvalid(f.json.replace("\"crc32\": \"" + hex8(f.crc("core")), "\"crc32\": \"123"));
        assertInvalid(f.json.replace(f.sha("core"), f.sha("core").substring(2)));
        assertInvalid(f.json.replace("\"size\": " + f.content.get("core").length + ",", "\"size\": 0,"));
        assertInvalid(f.json.replace("\"size\": " + f.content.get("extras").length + ",",
                "\"size\": " + (f.content.get("extras").length + 201) + ",")); // past the archive's end
        assertInvalid(f.json.replace("\"first_play\": [\"core\"", "\"first_play\": [\"nope\""));
        assertInvalid(f.json.replace("\"needs\": [\"core\", \"shared-1\", \"map-alpha\"]",
                "\"needs\": [\"core\", \"shared-9\", \"map-alpha\"]"));
        assertInvalid(f.json.replace("\"depends\": [\"shared-1\"]", "\"depends\": [\"shared-7\"]"));
        assertInvalid(f.json.replace("\"priority\": 0,", ""));
        assertInvalid("[]");
        assertInvalid("{\"format\": 1");
        assertInvalid("<html>Not Found</html>");
    }

    @Test
    public void thePublishedManifestParses() throws Exception {
        File sample = new File(System.getProperty("user.home"),
                ".cache/godnotic-work/data-out/split-G2/godnotic-data.json");
        org.junit.Assume.assumeTrue(sample.isFile());
        String text = new String(Files.readAllBytes(sample.toPath()), StandardCharsets.UTF_8);
        // As built it has no archive.url (set when published): not usable yet.
        assertInvalid(text);
        String published = text.replace("\"folder\": \"Xonotic/data/\"",
                "\"folder\": \"Xonotic/data/\", \"url\": \"https://example.org/Godnotic-data-astc.zip\"");
        DataManifest m = DataManifest.parse(published);
        assertEquals(72, m.packs.size());
        assertEquals(14, m.firstPlay.size());
        assertEquals("map-stormkeep", m.firstPlay.get(13).name);
        assertEquals(64225800L, m.pack("core").size);
        assertEquals(0xc49b9836L, m.pack("core").crc32);
    }

    // -------------------------------------------------- first play, the rest

    @Test
    public void freshInstallFetchesFirstPlayThenTheRest() throws Exception {
        Fixture f = standard("A");
        server.publish(f);
        DataSession s = session();
        assertNull("no manifest before one is fetched", s.manifest());
        assertEquals(DataSession.Refresh.CURRENT, s.refresh(server.manifestUrl()));
        assertFalse(s.playable());
        assertEquals(f.bytes("core", "shared-1", "map-alpha"), s.firstPlayNeed()[0]);
        assertEquals(f.bytes("shared-2", "map-beta", "music", "extras"), s.restBytes());

        List<long[]> progress = Collections.synchronizedList(new ArrayList<long[]>());
        s.firstPlay((done, total, verifying) -> progress.add(new long[] { done, total }));

        assertTrue(s.playable());
        f.assertInstalled(data, "core", "shared-1", "map-alpha");
        f.assertAbsent(data, "shared-2", "map-beta", "music", "extras");
        // first_play's bytes and not one more: no directory read, no retries.
        assertEquals(f.bytes("core", "shared-1", "map-alpha"), server.archiveBytes.get());
        assertEquals(f.bytes("core", "shared-1", "map-alpha"), progress.get(progress.size() - 1)[1]);
        assertEquals(progress.get(progress.size() - 1)[0], progress.get(progress.size() - 1)[1]);
        assertNoParts();

        // The game starts here; the rest follows without asking.
        s.publishIdle(true);
        assertTrue(status().contains("state fetching\n"));
        TestHost host = new TestHost();
        s.background(server.manifestUrl(), host);

        assertEquals(DataStatus.COMPLETE, s.state());
        f.assertInstalled(data, "core", "shared-1", "map-alpha", "shared-2", "map-beta", "music", "extras");
        assertEquals(f.archiveBytesOfPacks(), server.archiveBytes.get());
        assertEquals(Arrays.asList("godnotic-T-shared-2.pk3", "godnotic-T-map-beta.pk3",
                "godnotic-T-music.pk3", "godnotic-T-extras.pk3"), sorted(host.landed, f));
        assertNoParts();
        String status = status();
        assertTrue(status, status.startsWith("format 1\nset A\nstate complete\n"));
        assertTrue(status, status.contains("done_kb " + f.archiveBytesOfPacks() / 1024 + "\n"));
        assertTrue(status, status.contains("total_kb " + f.archiveBytesOfPacks() / 1024 + "\n"));
        for (String name : f.content.keySet()) {
            assertTrue(status, status.contains("landed godnotic-T-" + name + ".pk3\n"));
        }
        String state = read(new File(data, DataInstall.STATE));
        assertTrue(state, state.startsWith("set A\n"));
        assertTrue(state, state.contains("pack godnotic-T-core.pk3 " + f.content.get("core").length
                + " " + hex8(f.crc("core")) + "\n"));
        assertEquals(f.json, read(new File(data, DataSession.MANIFEST_FILE)));

        // The next start: nothing but the manifest is asked for.
        long before = server.archiveRequests.get();
        DataSession again = session();
        assertTrue(again.playable());
        again.background(server.manifestUrl(), new TestHost());
        assertEquals(DataStatus.COMPLETE, again.state());
        assertEquals(before, server.archiveRequests.get());
    }

    @Test
    public void aStartWithoutNetworkUsesTheStoredManifest() throws Exception {
        Fixture f = standard("A");
        server.publish(f);
        DataSession s = session();
        s.refresh(server.manifestUrl());
        s.firstPlay((d, t, v) -> { });
        server.stop();

        DataSession offline = session();
        assertNotNull(offline.manifest());
        assertTrue(offline.playable());
        assertEquals(DataSession.Refresh.UNREACHABLE, offline.refresh(server.manifestUrl()));
        assertNotNull("a failed fetch keeps the stored one", offline.manifest());
        assertEquals(f.bytes("shared-2", "map-beta", "music", "extras"), offline.restBytes());
    }

    // ---------------------------------------------------------------- resume

    @Test
    public void aStopMidPackResumesWithoutFetchingFinishedBytesAgain() throws Exception {
        Fixture f = standard("A");
        server.publish(f);
        server.delayMs = 3;
        DataSession s = session();
        s.refresh(server.manifestUrl());
        long first = f.bytes("core", "shared-1", "map-alpha");

        Throwable[] thrown = new Throwable[1];
        Thread run = new Thread(() -> {
            try {
                s.firstPlay((d, t, v) -> { });
            } catch (Throwable e) {
                thrown[0] = e;
            }
        });
        run.start();
        // Core is five chunks: stop when about half of first_play has left the server.
        waitUntil(() -> server.archiveBytes.get() >= first / 2);
        run.interrupt();
        run.join(30_000);
        assertFalse(run.isAlive());
        assertTrue(String.valueOf(thrown[0]), thrown[0] instanceof InterruptedIOException);

        File part = new File(data, "godnotic-T-core.pk3.part");
        File stateFile = new File(data, "godnotic-T-core.pk3.part.state");
        assertTrue("the part is kept", part.isFile());
        assertTrue("its chunk state is kept", stateFile.isFile());
        assertFalse(new File(data, "godnotic-T-core.pk3").exists());
        int[] saved = chunkState(stateFile, f.content.get("core").length);
        long onDisk = 0;
        for (int p : saved) {
            onDisk += p;
        }
        assertTrue("something was fetched before the stop", onDisk > 0);
        assertTrue("and not everything", onDisk < f.content.get("core").length);

        // A new process: a new session over the same directory.
        server.delayMs = 0;
        server.ranges.clear();
        long othersOnDisk = doneBytesOf(f, "shared-1", "map-alpha");
        DataSession next = session();
        assertEquals(first - onDisk - othersOnDisk, next.firstPlayNeed()[0]);
        next.firstPlay((d, t, v) -> { });
        f.assertInstalled(data, "core", "shared-1", "map-alpha");
        assertNoParts();

        // Every request for core starts exactly where a chunk had stopped.
        long coreOffset = f.offset("core");
        int coreRequests = 0;
        for (long[] r : server.ranges) {
            if (r[0] < coreOffset || r[0] >= coreOffset + f.content.get("core").length) {
                continue;
            }
            coreRequests++;
            int chunk = (int) ((r[0] - coreOffset) / CHUNK);
            assertEquals("chunk " + chunk + " resumes at its saved byte",
                    coreOffset + (long) chunk * CHUNK + saved[chunk], r[0]);
            assertTrue("a finished chunk is not asked for again",
                    saved[chunk] < Math.min(CHUNK, f.content.get("core").length - (long) chunk * CHUNK));
        }
        assertTrue(coreRequests > 0);
        // (Counted from the requests: the server may still have been
        // writing into the stopped run's closed connections.)
        long asked = 0;
        for (long[] r : server.ranges) {
            asked += r[1] - r[0] + 1;
        }
        assertEquals("the second run asked only for what was missing", first - onDisk - othersOnDisk, asked);
    }

    // --------------------------------------------------------------- damage

    @Test
    public void aDamagedPackIsDeletedAndFetchedAgain() throws Exception {
        Fixture f = standard("A");
        server.publish(f);
        server.corrupt(f, "shared-1", 1);
        DataSession s = session();
        s.refresh(server.manifestUrl());
        s.firstPlay((d, t, v) -> { });
        f.assertInstalled(data, "core", "shared-1", "map-alpha");
        assertEquals(2, server.requestsFor(f, "shared-1"));
        assertTrue(log.toString(), log.toString().contains("godnotic-T-shared-1.pk3 failed its check"));
        assertNoParts();
    }

    @Test
    public void aPackDamagedThreeTimesIsGivenUpAndTheOthersCarryOn() throws Exception {
        Fixture f = standard("A");
        server.publish(f);
        server.corrupt(f, "shared-1", 1000);
        DataSession s = session();
        s.refresh(server.manifestUrl());
        try {
            s.firstPlay((d, t, v) -> { });
            fail("first play cannot be complete without shared-1");
        } catch (IOException e) {
            assertTrue(e.getMessage(), e.getMessage().contains("godnotic-T-shared-1.pk3"));
        }
        assertEquals(DataFetch.PACK_ATTEMPTS, server.requestsFor(f, "shared-1"));
        // Never a damaged pk3 in place, nor a part of it left behind.
        f.assertAbsent(data, "shared-1");
        assertFalse(new File(data, "godnotic-T-shared-1.pk3.part").exists());
        // Core does not need it and is in; map-alpha does, and waits as a part.
        f.assertInstalled(data, "core");
        f.assertAbsent(data, "map-alpha");
        assertFalse(s.playable());

        // The background run reports the error and installs what it can.
        TestHost host = new TestHost();
        Thread run = new Thread(() -> s.background(null, host));
        run.start();
        waitUntil(() -> DataStatus.ERROR.equals(s.state()));
        f.assertInstalled(data, "core", "shared-2", "map-beta", "music", "extras");
        f.assertAbsent(data, "shared-1", "map-alpha");
        assertTrue(status(), status().contains("state error\nmessage Damaged download of godnotic-T-shared-1.pk3\n"));
        // Given up on means left alone: an archive that does not match its
        // manifest is not downloaded again and again.
        assertEquals(DataFetch.PACK_ATTEMPTS, server.requestsFor(f, "shared-1"));

        // The server is mended; a want tries again without waiting.
        server.corrupt.clear();
        request("want alpha 1");
        waitUntil(() -> s.install().complete());
        host.stop = true;
        run.join(30_000);
        f.assertInstalled(data, "shared-1", "map-alpha");
    }

    // -------------------------------------------------- no manifest: fallback

    @Test
    public void aMissingOrUnknownManifestMeansTheSingleArchive() throws Exception {
        Fixture f = standard("A");
        server.publish(f);
        server.manifestStatus = 404;
        DataSession s = session();
        assertEquals(DataSession.Refresh.ABSENT, s.refresh(server.manifestUrl()));
        assertNull(s.manifest());
        assertFalse(s.playable());

        server.manifestStatus = 200;
        server.manifest = f.json.replace("\"format\": 1", "\"format\": 2");
        assertEquals(DataSession.Refresh.ABSENT, s.refresh(server.manifestUrl()));
        assertNull(s.manifest());

        server.manifest = "<html>";
        assertEquals(DataSession.Refresh.ABSENT, s.refresh(server.manifestUrl()));
        assertNull(s.manifest());

        server.manifestStatus = 503;
        assertEquals(DataSession.Refresh.UNREACHABLE, s.refresh(server.manifestUrl()));
        assertNull(s.manifest());

        // Nothing was written, nothing was fetched: the old path owns the directory.
        assertFalse(new File(data, DataSession.MANIFEST_FILE).exists());
        assertFalse(new File(data, DataInstall.STATE).exists());
        assertEquals(0, server.archiveRequests.get());
        s.publishIdle(true);
        assertFalse("no status file without a manifest", new File(touch, DataStatus.STATUS_FILE).exists());

        // A background run with nothing published ends by itself.
        server.manifestStatus = 404;
        s.background(server.manifestUrl(), new TestHost());
        assertNull(s.state());

        // And a manifest that vanishes later does not undo an install made from one.
        server.manifestStatus = 200;
        server.manifest = f.json;
        assertEquals(DataSession.Refresh.CURRENT, s.refresh(server.manifestUrl()));
        server.manifestStatus = 404;
        assertEquals(DataSession.Refresh.ABSENT, session().refresh(server.manifestUrl()));
        assertNotNull(session().manifest());
    }

    // --------------------------------------------------------- the old layout

    @Test
    public void aCompleteOldInstallFetchesNothing() throws Exception {
        Fixture f = standard("A");
        server.publish(f);
        oldLayout("data", "maps", "music", "xoncompat");
        write(new File(data, "font-unifont-20261107.pk3"), "font");

        DataSession s = session();
        s.background(server.manifestUrl(), new TestHost());
        assertEquals(DataStatus.LEGACY, s.state());
        assertTrue(s.playable());
        assertEquals("only the manifest was asked for", 1, server.requests.get());
        assertEquals(0, server.archiveRequests.get());
        assertEquals("legacy A\n", read(new File(data, DataInstall.STATE)));
        assertTrue(status(), status().startsWith("format 1\nset A\nstate legacy\n"));
        assertOldLayout(true, "data", "maps", "music", "xoncompat");
        f.assertAbsent(data, "core");

        // Start after start, as long as the published set is that one.
        DataSession again = session();
        assertTrue(again.playable());
        again.background(server.manifestUrl(), new TestHost());
        assertEquals(DataStatus.LEGACY, again.state());
        assertEquals(2, server.requests.get());
        assertEquals(0, server.archiveRequests.get());
        assertOldLayout(true, "data", "maps", "music", "xoncompat");
    }

    @Test
    public void aCompleteOldInstallMovesToANewSetAndIsDeletedLast() throws Exception {
        Fixture a = standard("A");
        server.publish(a);
        oldLayout("data", "maps", "music", "xoncompat");
        session().background(server.manifestUrl(), new TestHost());
        assertEquals("legacy A\n", read(new File(data, DataInstall.STATE)));

        Fixture b = standard("B");
        server.publish(b);
        DataSession s = session();
        assertTrue("the old packs keep the game whole meanwhile", s.playable());
        List<String> oldSeen = Collections.synchronizedList(new ArrayList<String>());
        TestHost host = new TestHost() {
            @Override
            public void landed(String installAs) {
                super.landed(installAs);
                // Whenever a new pack lands the old ones are still all there,
                // or the new set is complete.
                oldSeen.add(installAs + ":" + new File(data, "xonotic-20261107-maps.pk3").exists()
                        + ":" + s.install().complete());
            }
        };
        s.background(server.manifestUrl(), host);
        assertEquals(DataStatus.COMPLETE, s.state());
        assertEquals(7, oldSeen.size());
        int stillThere = 0;
        for (String seen : oldSeen) {
            // Never "old packs gone, new set not complete". (Both there and
            // complete can be seen: the two are read one after the other
            // while another thread installs the last pack.)
            assertFalse(seen, seen.endsWith(":false:false"));
            stillThere += seen.endsWith(":true:false") ? 1 : 0;
        }
        assertTrue("the old packs were there while the new set arrived: " + oldSeen, stillThere >= 4);
        assertOldLayout(false, "data", "maps", "music", "xoncompat");
        b.assertInstalled(data, "core", "shared-1", "map-alpha", "shared-2", "map-beta", "music", "extras");
        String state = read(new File(data, DataInstall.STATE));
        assertTrue(state, state.startsWith("set B\npack "));
        assertFalse(state, state.contains("legacy"));
    }

    @Test
    public void anUnfinishedOldInstallLosesItsPartsAtOnceAndItsPacksAfterFirstPlay() throws Exception {
        Fixture f = standard("A");
        server.publish(f);
        oldLayout("data", "maps"); // no music: a first download that never finished
        write(new File(data, "xonotic-20261107-music.pk3.part"), "half");
        write(new File(data, "xonotic-20261107-music.pk3.part.state"), "state");
        write(new File(data, ".packs-setup"), "etag\n123\nwant xonotic-*-music.pk3\n");
        write(new File(data, "my-own-map.pk3"), "not ours to touch");
        new File(data, "xonotic-data.pk3dir").mkdirs();

        DataSession s = session();
        assertEquals(DataSession.Refresh.CURRENT, s.refresh(server.manifestUrl()));
        assertFalse(new File(data, "xonotic-20261107-music.pk3.part").exists());
        assertFalse(new File(data, "xonotic-20261107-music.pk3.part.state").exists());
        assertFalse(new File(data, ".packs-setup").exists());
        assertOldLayout(true, "data", "maps");
        assertFalse("an old install without music does not make the split set playable", s.playable());
        assertFalse(read(new File(data, DataInstall.STATE)).contains("legacy"));

        s.firstPlay((d, t, v) -> { });
        assertTrue(s.playable());
        assertOldLayout(false, "data", "maps");
        assertTrue(new File(data, "my-own-map.pk3").isFile());
        assertTrue(new File(data, "xonotic-data.pk3dir").isDirectory());
    }

    // ------------------------------------------------------------- a new set

    @Test
    public void aNewSetKeepsUnchangedPacksRefetchesChangedOnesAndDropsUnlistedOnesAfterFirstPlay()
            throws Exception {
        Fixture a = standard("A");
        server.publish(a);
        DataSession s = session();
        s.refresh(server.manifestUrl());
        s.firstPlay((d, t, v) -> { });
        s.background(null, new TestHost());
        assertTrue(s.install().complete());
        write(new File(data, "godnotic-T-extras.pk3.part"), "a part of a pack the next set drops");

        // Set B: core and music as they were, shared-1 with new bytes under
        // its old name, extras gone, a new pack, and map-alpha changed.
        Fixture b = new Builder("B")
                .pack("core", 300_000, 0, "A").pack("shared-1", 30_000, 1, "B").pack("map-alpha", 20_000, 2, "B")
                .pack("shared-2", 40_000, 3, "A").pack("map-beta", 20_000, 4, "A").pack("music", 150_000, 5, "A")
                .pack("shared-3", 25_000, 6, "B")
                .firstPlay("core", "shared-1", "map-alpha")
                .map("alpha", "map-alpha", "core", "shared-1", "map-alpha")
                .map("beta", "map-beta", "core", "shared-2", "map-beta")
                .build(server);
        server.publish(b);
        server.ranges.clear();
        long before = server.archiveBytes.get();

        DataSession next = session();
        assertEquals("the stored manifest is still A", "A", next.manifest().set);
        assertEquals(DataSession.Refresh.CURRENT, next.refresh(server.manifestUrl()));
        assertEquals("B", next.manifest().set);
        assertFalse("first_play of the new set is not all there", next.playable());
        // Nothing whole has gone yet: the unlisted pack and the changed ones
        // are still what the old set installed.
        a.assertInstalled(data, "core", "shared-1", "map-alpha", "music", "extras");
        assertTrue(new File(data, "godnotic-T-extras.pk3.part").exists());
        assertEquals(b.bytes("shared-1", "map-alpha"), next.firstPlayNeed()[0]);

        next.firstPlay((d, t, v) -> { });
        b.assertInstalled(data, "core", "shared-1", "map-alpha");
        assertEquals("only the changed first_play packs were fetched",
                b.bytes("shared-1", "map-alpha"), server.archiveBytes.get() - before);
        assertEquals(0, server.requestsFor(b, "core"));
        assertFalse("the unlisted pack goes once first_play is in",
                new File(data, "godnotic-T-extras.pk3").exists());
        assertFalse(new File(data, "godnotic-T-extras.pk3.part").exists());

        next.background(null, new TestHost());
        assertEquals(DataStatus.COMPLETE, next.state());
        b.assertInstalled(data, "core", "shared-1", "map-alpha", "shared-2", "map-beta", "music", "shared-3");
        assertEquals(b.bytes("shared-1", "map-alpha", "shared-3"), server.archiveBytes.get() - before);
        assertEquals(0, server.requestsFor(b, "music"));
        assertEquals(0, server.requestsFor(b, "shared-2"));
        assertTrue(read(new File(data, DataInstall.STATE)).startsWith("set B\n"));
    }

    @Test
    public void aWholeFileWithoutItsLineIsHashedAndAdoptedOrDeleted() throws Exception {
        Fixture f = standard("A");
        server.publish(f);
        // A kill between the rename and the save: the file, no line.
        write(new File(data, "godnotic-T-core.pk3"), f.content.get("core"));
        byte[] wrong = f.content.get("shared-1").clone();
        wrong[10] ^= 1;
        write(new File(data, "godnotic-T-shared-1.pk3"), wrong);

        DataSession s = session();
        s.refresh(server.manifestUrl());
        s.reconcile();
        assertTrue(s.install().installed(s.manifest().pack("core")));
        assertFalse(s.install().installed(s.manifest().pack("shared-1")));
        assertFalse("right size, wrong bytes: deleted", new File(data, "godnotic-T-shared-1.pk3").exists());
        s.firstPlay((d, t, v) -> { });
        assertEquals(0, server.requestsFor(f, "core"));
        f.assertInstalled(data, "core", "shared-1", "map-alpha");
    }

    // ------------------------------------------------------- order and wants

    @Test
    public void theOrderIsFirstPlayThenPriorityAndAWantedMapJumpsIt() throws Exception {
        // Priorities put the maps' own packs before the packs they need.
        Fixture f = new Builder("A")
                .pack("map-alpha", 10_000, 0, "A").pack("map-beta", 10_000, 1, "A").pack("map-gamma", 10_000, 2, "A")
                .pack("core", 100_000, 3, "A").pack("music", 50_000, 4, "A")
                .pack("shared-1", 10_000, 5, "A").pack("shared-2", 10_000, 6, "A").pack("extras", 10_000, 7, "A")
                .firstPlay("core", "shared-1", "map-alpha")
                .map("alpha", "map-alpha", "core", "shared-1", "map-alpha")
                .map("beta", "map-beta", "core", "map-beta", "shared-1", "shared-2")
                .map("gamma", "map-gamma", "core", "shared-2", "map-gamma")
                .build(server);
        DataInstall install = new DataInstall(data, env);
        install.adopt(DataManifest.parse(f.json));

        assertEquals(Arrays.asList("core", "shared-1", "map-alpha"), names(install.order(null, true)));
        // By priority map-beta would be first of the rest; its needs go before it.
        assertEquals(Arrays.asList("core", "shared-1", "map-alpha", "shared-2", "map-beta", "map-gamma",
                "music", "extras"), names(install.order(null, false)));
        // A want: that map's needs in front, its own pack after them.
        assertEquals(Arrays.asList("core", "shared-2", "map-gamma", "shared-1", "map-alpha", "map-beta",
                "music", "extras"), names(install.order("gamma", false)));
        assertEquals(Arrays.asList("core", "shared-1", "shared-2", "map-beta"), names(install.missingNeeds("beta")));
        assertEquals(install.order(null, false), install.order("no-such-map", false));
        // The boot phase is first_play whatever is wanted.
        assertEquals(Arrays.asList("core", "shared-1", "map-alpha"), names(install.order("gamma", true)));
        assertFalse(install.ready(install.manifest().pack("map-gamma")));
        assertTrue(install.ready(install.manifest().pack("core")));
    }

    @Test
    public void aWantedMapIsFetchedBeforeWhatWasAheadOfIt() throws Exception {
        Builder b = new Builder("A").pack("core", 20_000, 0, "A").firstPlay("core");
        for (int i = 1; i <= 24; i++) {
            b.pack("filler-" + i, 60_000, i, "A");
        }
        Fixture f = b.pack("shared-z", 30_000, 98, "A").pack("map-zulu", 20_000, 99, "A")
                .map("zulu", "map-zulu", "core", "shared-z", "map-zulu").build(server);
        server.publish(f);
        DataSession s = session();
        s.refresh(server.manifestUrl());
        s.firstPlay((d, t, v) -> { });

        // A request an earlier session left is not this one's.
        request("want zulu 7");
        assertTrue(new File(touch, DataStatus.REQUEST_FILE).setLastModified(System.currentTimeMillis() - 3_600_000));
        s.pollRequest();
        assertNull(s.want());

        server.delayMs = 30;
        TestHost host = new TestHost();
        Thread run = new Thread(() -> s.background(null, host));
        run.start();
        waitUntil(() -> host.landed.size() >= 2);
        request("want zulu 8");
        waitUntil(() -> status().contains("want zulu\n") || new File(data, "godnotic-T-map-zulu.pk3").exists());
        waitUntil(() -> new File(data, "godnotic-T-map-zulu.pk3").exists());
        int landedWhenPlayable = host.landed.size();
        server.delayMs = 0;
        run.join(60_000);
        assertFalse(run.isAlive());
        assertEquals(DataStatus.COMPLETE, s.state());

        int own = host.landed.indexOf("godnotic-T-map-zulu.pk3");
        int need = host.landed.indexOf("godnotic-T-shared-z.pk3");
        assertTrue("the map's other pack lands before its own", need >= 0 && need < own);
        // Last by priority, yet in long before the fillers were done.
        assertTrue("landed " + landedWhenPlayable + " of 26 when zulu was playable: " + host.landed,
                landedWhenPlayable <= 20);
        assertFalse("the want is over once the map is in", status().contains("want "));
    }

    @Test
    public void aMapsOwnPackIsRenamedLast() throws Exception {
        // The own pack is tiny and first by priority; its need is big and slow.
        Fixture f = new Builder("A").pack("map-alpha", 2_000, 0, "A").pack("core", 10_000, 1, "A")
                .pack("shared-1", 1_500_000, 2, "A")
                .firstPlay("map-alpha", "core", "shared-1")
                .map("alpha", "map-alpha", "core", "shared-1", "map-alpha").build(server);
        server.publish(f);
        server.delayMs = 1;
        server.watchFrom = f.offset("map-alpha");
        server.watchFile = new File(data, "godnotic-T-shared-1.pk3");
        DataSession s = session();
        s.refresh(server.manifestUrl());
        List<String> seen = Collections.synchronizedList(new ArrayList<String>());
        Thread watch = new Thread(() -> {
            while (!Thread.currentThread().isInterrupted()) {
                boolean own = new File(data, "godnotic-T-map-alpha.pk3").exists();
                boolean others = new File(data, "godnotic-T-core.pk3").exists()
                        && new File(data, "godnotic-T-shared-1.pk3").exists();
                if (own && !others) {
                    seen.add("the bsp was there before the map was playable");
                }
            }
        });
        watch.start();
        try {
            s.firstPlay((d, t, v) -> { });
        } finally {
            watch.interrupt();
            watch.join();
        }
        assertEquals(Collections.emptyList(), seen);
        f.assertInstalled(data, "map-alpha", "core", "shared-1");
        // Its bytes were not held back, only its rename.
        assertEquals("map-alpha was asked for while shared-1 was still on its way",
                Boolean.FALSE, server.watchExisted);
    }

    // ------------------------------------------------------------ the server

    @Test
    public void a200InsteadOfA206IsAnError() throws Exception {
        Fixture f = standard("A");
        server.publish(f);
        server.ignoreRange = true;
        DataSession s = session();
        s.refresh(server.manifestUrl());
        try {
            s.firstPlay((d, t, v) -> { });
            fail("a server without ranges cannot serve packs");
        } catch (IOException e) {
            assertTrue(e.getMessage(), e.getMessage().contains("HTTP 200"));
        }
        f.assertAbsent(data, "core", "shared-1", "map-alpha");
        File part = new File(data, "godnotic-T-core.pk3.part");
        assertTrue("not a byte of the answer was written", !part.exists() || chunkTotal(f, "core") == 0);
    }

    @Test
    public void aContentRangeThatDoesNotMatchIsAnError() throws Exception {
        assertTrue(DataFetch.rangeMatches("bytes 100-199/1000", 100, 199, 1000));
        assertTrue(DataFetch.rangeMatches("bytes 100-199/*", 100, 199, 1000));
        assertFalse(DataFetch.rangeMatches("bytes 0-99/1000", 100, 199, 1000));
        assertFalse(DataFetch.rangeMatches("bytes 100-198/1000", 100, 199, 1000));
        assertFalse(DataFetch.rangeMatches("bytes 100-199/2000", 100, 199, 1000));
        assertFalse(DataFetch.rangeMatches(null, 100, 199, 1000));
        assertFalse(DataFetch.rangeMatches("100-199/1000", 100, 199, 1000));

        Fixture f = standard("A");
        server.publish(f);
        server.shiftContentRange = 1;
        DataSession s = session();
        s.refresh(server.manifestUrl());
        try {
            s.firstPlay((d, t, v) -> { });
            fail();
        } catch (IOException e) {
            assertTrue(e.getMessage(), e.getMessage().contains("another range"));
        }
        f.assertAbsent(data, "core", "shared-1", "map-alpha");
    }

    @Test
    public void suffixRangesGetA501FromTheTestServer() throws Exception {
        // The check in tearDown means something only if the server would say so.
        java.net.HttpURLConnection c = (java.net.HttpURLConnection)
                new java.net.URL(server.archiveUrl()).openConnection();
        c.setRequestProperty("Range", "bytes=-100");
        assertEquals(501, c.getResponseCode());
        c.disconnect();
        assertEquals(1, server.suffixRanges.getAndSet(0));
    }

    // ----------------------------------------------------- network, storage

    @Test
    public void aMeteredNetworkWithoutConsentWaits() throws Exception {
        Fixture f = standard("A");
        server.publish(f);
        DataSession s = session();
        s.refresh(server.manifestUrl());
        s.firstPlay((d, t, v) -> { });
        long before = server.archiveRequests.get();

        TestHost host = new TestHost();
        host.mayFetch = false;
        Thread run = new Thread(() -> s.background(null, host));
        run.start();
        waitUntil(() -> DataStatus.WAITING.equals(s.state()));
        Thread.sleep(700);
        assertEquals("nothing is fetched while waiting", before, server.archiveRequests.get());
        assertTrue(status(), status().contains("state waiting\n"));

        host.mayFetch = true; // Wi-Fi, or "Use mobile data"
        run.join(30_000);
        assertFalse(run.isAlive());
        assertEquals(DataStatus.COMPLETE, s.state());
    }

    @Test
    public void stoppingTheBackgroundRunKeepsWhatItHasAndSaysSo() throws Exception {
        Fixture f = standard("A");
        server.publish(f);
        DataSession s = session();
        s.refresh(server.manifestUrl());
        s.firstPlay((d, t, v) -> { });
        server.delayMs = 5;
        TestHost host = new TestHost();
        Thread run = new Thread(() -> s.background(null, host));
        run.start();
        waitUntil(() -> DataStatus.FETCHING.equals(s.state()) && server.archiveBytes.get()
                > f.bytes("core", "shared-1", "map-alpha") + 20_000);
        host.stop = true; // the service's time is up, or the game went away
        run.join(30_000);
        assertFalse(run.isAlive());
        assertFalse(s.backgroundRunning());
        assertTrue(status(), status().contains("state error\nmessage The download stopped"));

        server.delayMs = 0;
        DataSession next = session();
        next.background(null, new TestHost());
        assertEquals(DataStatus.COMPLETE, next.state());
        f.assertInstalled(data, "shared-2", "map-beta", "music", "extras");
    }

    @Test
    public void tooLittleStorageIsSaidBeforeAnythingIsFetched() throws Exception {
        Fixture f = standard("A");
        server.publish(f);
        DataSession s = new DataSession(data, touch, env, options(DataFetch.SLACK_BYTES));
        s.refresh(server.manifestUrl());
        free = DataFetch.SLACK_BYTES + 1000;
        try {
            s.firstPlay((d, t, v) -> { });
            fail();
        } catch (DataFetch.NoSpace e) {
            assertEquals(f.bytes("core", "shared-1", "map-alpha") + DataFetch.SLACK_BYTES, e.needed);
        }
        assertEquals(0, server.archiveRequests.get());
    }

    // ---------------------------------------------------- the engine's files

    @Test
    public void theStatusFileIsWhatTheContractShows() {
        assertEquals("format 1\nset 20261107-4c53098165d2\nstate fetching\ndone_kb 184320\ntotal_kb 363868\n"
                        + "want boil\nwant_done_kb 2048\nwant_total_kb 9216\n"
                        + "landed godnotic-20261107-map-boil.pk3\nlanded godnotic-20261107-music.pk3\n",
                DataStatus.text("20261107-4c53098165d2", DataStatus.FETCHING, 184320L * 1024, 363868L * 1024 + 5,
                        "boil", 2048L * 1024, 9216L * 1024, null,
                        Arrays.asList("godnotic-20261107-map-boil.pk3", "godnotic-20261107-music.pk3")));
        assertEquals("format 1\nset S\nstate error\nmessage two lines\ndone_kb 0\ntotal_kb 1\n",
                DataStatus.text("S", DataStatus.ERROR, 100, 1024, null, 0, 0, "two\nlines",
                        Collections.<String>emptyList()));
        assertEquals("format 1\nset S\nstate waiting\ndone_kb 0\ntotal_kb 1\n",
                DataStatus.text("S", DataStatus.WAITING, 100, 1024, null, 0, 0, "ignored",
                        Collections.<String>emptyList()));
    }

    @Test
    public void theRequestFileIsParsed() throws Exception {
        DataStatus.Request r = DataStatus.parseRequest("want boil 3\n");
        assertEquals("boil", r.map);
        assertEquals(3, r.serial);
        assertNull(DataStatus.parseRequest(""));
        assertNull(DataStatus.parseRequest("want boil\n"));
        assertNull(DataStatus.parseRequest("want boil x\n"));
        assertNull(DataStatus.parseRequest("fetch boil 3\n"));
        assertNull(DataStatus.parseRequest(null));
        assertEquals("https://h/r/data-slim/godnotic-data-astc.json",
                DataSession.manifestUrl("https://h/r/data-slim/Godnotic-data.zip", "astc"));
        assertEquals("https://h/r/data-slim/godnotic-data-dxt.json",
                DataSession.manifestUrl("https://h/r/data-slim/Godnotic-data.zip", "dxt"));
        assertNull(DataSession.manifestUrl(null, "astc"));
    }

    // =============================================================== fixtures

    private DataSession session() {
        return new DataSession(data, touch, env, options(0));
    }

    private static DataFetch.Options options(long slack) {
        DataFetch.Options o = new DataFetch.Options();
        o.chunk = CHUNK;
        o.chunkAttempts = 2;
        o.pauseMs = 5;
        o.connectTimeoutMs = 3000;
        o.readTimeoutMs = 3000;
        o.slackBytes = slack;
        return o;
    }

    /** Core in five chunks, two maps with their shared packs, music, extras. */
    private Fixture standard(String set) {
        return new Builder(set)
                .pack("core", 300_000, 0, set).pack("shared-1", 30_000, 1, set).pack("map-alpha", 20_000, 2, set)
                .pack("shared-2", 40_000, 3, set).pack("map-beta", 20_000, 4, set)
                .pack("music", 150_000, 5, set).pack("extras", 200_000, 6, set)
                .firstPlay("core", "shared-1", "map-alpha")
                .map("alpha", "map-alpha", "core", "shared-1", "map-alpha")
                .map("beta", "map-beta", "core", "shared-2", "map-beta")
                .build(server);
    }

    private static class TestHost implements DataSession.Host {
        final List<String> landed = Collections.synchronizedList(new ArrayList<String>());
        volatile boolean mayFetch = true;
        volatile boolean stop;

        @Override
        public boolean mayFetch() {
            return mayFetch;
        }

        @Override
        public boolean keepGoing() {
            return !stop;
        }

        @Override
        public boolean online() {
            return true;
        }

        @Override
        public void landed(String installAs) {
            landed.add(installAs);
        }

        @Override
        public void changed() {
        }
    }

    private static final class Fixture {
        String json;
        byte[] archive;
        final Map<String, byte[]> content = new LinkedHashMap<>();
        final Map<String, Long> offsets = new LinkedHashMap<>();

        long offset(String name) {
            return offsets.get(name);
        }

        long bytes(String... names) {
            long sum = 0;
            for (String n : names) {
                sum += content.get(n).length;
            }
            return sum;
        }

        long archiveBytesOfPacks() {
            long sum = 0;
            for (byte[] b : content.values()) {
                sum += b.length;
            }
            return sum;
        }

        long crc(String name) {
            CRC32 c = new CRC32();
            c.update(content.get(name));
            return c.getValue();
        }

        String sha(String name) {
            return sha256(content.get(name));
        }

        void assertInstalled(File dir, String... names) throws IOException {
            for (String n : names) {
                File file = new File(dir, "godnotic-T-" + n + ".pk3");
                assertTrue(n + " is installed", file.isFile());
                assertArrayEquals(n + " has the set's bytes", content.get(n), Files.readAllBytes(file.toPath()));
            }
        }

        void assertAbsent(File dir, String... names) {
            for (String n : names) {
                assertFalse(n + " is not installed", new File(dir, "godnotic-T-" + n + ".pk3").exists());
            }
        }
    }

    /** Builds an archive (packs behind stand-ins for zip local headers) and its manifest. */
    private static final class Builder {
        private final String set;
        private final List<String[]> packs = new ArrayList<>();
        private final List<String> firstPlay = new ArrayList<>();
        private final Map<String, String[]> maps = new LinkedHashMap<>();

        Builder(String set) {
            this.set = set;
        }

        /** @param version packs of the same name, size and version have the same bytes in every set */
        Builder pack(String name, int size, int priority, String version) {
            packs.add(new String[] { name, String.valueOf(size), String.valueOf(priority), version });
            return this;
        }

        Builder firstPlay(String... names) {
            firstPlay.addAll(Arrays.asList(names));
            return this;
        }

        /** @param needs the map's packs, own pack included */
        Builder map(String map, String own, String... needs) {
            String[] all = new String[needs.length + 1];
            all[0] = own;
            System.arraycopy(needs, 0, all, 1, needs.length);
            maps.put(map, all);
            return this;
        }

        Fixture build(Server server) {
            Fixture f = new Fixture();
            java.io.ByteArrayOutputStream archive = new java.io.ByteArrayOutputStream();
            byte[] header = new byte[69];
            Arrays.fill(header, (byte) 'H');
            StringBuilder list = new StringBuilder();
            for (String[] p : packs) {
                byte[] bytes = new byte[Integer.parseInt(p[1])];
                new Random((p[0] + "/" + p[3]).hashCode()).nextBytes(bytes);
                archive.write(header, 0, header.length);
                f.offsets.put(p[0], (long) archive.size());
                archive.write(bytes, 0, bytes.length);
                f.content.put(p[0], bytes);
                String depends = "";
                for (String[] m : maps.values()) {
                    if (m[0].equals(p[0])) {
                        for (int i = 1; i < m.length; i++) {
                            if (m[i].startsWith("shared-")) {
                                depends += (depends.isEmpty() ? "" : ", ") + "\"" + m[i] + "\"";
                            }
                        }
                    }
                }
                if (list.length() > 0) {
                    list.append(",\n");
                }
                list.append("  {\"name\": \"").append(p[0]).append("\", \"file\": \"godnotic-T-").append(p[0])
                        .append(".pk3\", \"install_as\": \"godnotic-T-").append(p[0]).append(".pk3\", \"kind\": \"x\", ")
                        .append("\"size\": ").append(bytes.length).append(", \"sha256\": \"").append(f.sha(p[0]))
                        .append("\", \"crc32\": \"").append(hex8(f.crc(p[0]))).append("\", \"priority\": ")
                        .append(p[2]).append(", \"entries\": 3, \"depends\": [").append(depends)
                        .append("], \"offset\": ").append(f.offsets.get(p[0])).append("}");
            }
            byte[] tail = new byte[200]; // the zip directory's stand-in
            archive.write(tail, 0, tail.length);
            f.archive = archive.toByteArray();
            StringBuilder mapText = new StringBuilder();
            for (Map.Entry<String, String[]> m : maps.entrySet()) {
                if (mapText.length() > 0) {
                    mapText.append(", ");
                }
                mapText.append("\"").append(m.getKey()).append("\": {\"pack\": \"").append(m.getValue()[0])
                        .append("\", \"needs\": [").append(quoted(Arrays.asList(m.getValue()).subList(1, m.getValue().length)))
                        .append("]}");
            }
            f.json = "{\n \"format\": 1, \"set\": \"" + set + "\", \"meta\": {\"x\": [1.5, true, null, \"a\\\"b\"]},\n"
                    + " \"default_map\": \"alpha\",\n"
                    + " \"first_play\": [" + quoted(firstPlay) + "],\n"
                    + " \"maps\": {" + mapText + "},\n"
                    + " \"packs\": [\n" + list + "\n ],\n"
                    + " \"archive\": {\"file\": \"a.zip\", \"size\": " + f.archive.length
                    + ", \"folder\": \"Xonotic/data/\", \"url\": \"" + server.archiveUrl() + "\"}\n}\n";
            return f;
        }

        private static String quoted(List<String> names) {
            StringBuilder s = new StringBuilder();
            for (String n : names) {
                s.append(s.length() > 0 ? ", " : "").append('"').append(n).append('"');
            }
            return s.toString();
        }
    }

    /**
     * A range server as GitHub's release storage is: explicit ranges only,
     * suffix ranges get 501. On a ServerSocket: the JDK's own HTTP server is
     * not on the classpath of an Android unit test.
     */
    private static final class Server {
        private final java.net.ServerSocket socket;
        private final java.util.concurrent.ExecutorService pool = Executors.newCachedThreadPool(r -> {
            Thread t = new Thread(r, "test-server");
            t.setDaemon(true);
            return t;
        });
        private volatile boolean stopped;
        volatile String manifest = "";
        volatile int manifestStatus = 200;
        volatile byte[] archive = new byte[0];
        volatile boolean ignoreRange;
        volatile int shiftContentRange;
        volatile int delayMs;
        /** Whether {@code watchFile} existed when the range starting at {@code watchFrom} was asked for. */
        volatile long watchFrom = -1;
        volatile File watchFile;
        volatile Boolean watchExisted;
        final AtomicInteger requests = new AtomicInteger();
        final AtomicInteger archiveRequests = new AtomicInteger();
        final AtomicInteger suffixRanges = new AtomicInteger();
        /** Body bytes of archive answers that went out. */
        final AtomicLong archiveBytes = new AtomicLong();
        /** {from, to} of every archive range request, in order. */
        final List<long[]> ranges = Collections.synchronizedList(new ArrayList<long[]>());
        /** {from, to, times}: answers covering these bytes go out with one byte flipped. */
        final List<long[]> corrupt = Collections.synchronizedList(new ArrayList<long[]>());

        Server() throws IOException {
            socket = new java.net.ServerSocket(0, 64, java.net.InetAddress.getByName("127.0.0.1"));
            Thread accept = new Thread(() -> {
                while (!stopped) {
                    try {
                        java.net.Socket client = socket.accept();
                        pool.execute(() -> serve(client));
                    } catch (IOException e) {
                        return;
                    }
                }
            }, "test-server-accept");
            accept.setDaemon(true);
            accept.start();
        }

        String manifestUrl() {
            return "http://127.0.0.1:" + socket.getLocalPort() + "/manifest.json";
        }

        String archiveUrl() {
            return "http://127.0.0.1:" + socket.getLocalPort() + "/archive.zip";
        }

        void publish(Fixture f) {
            manifest = f.json;
            archive = f.archive;
        }

        void corrupt(Fixture f, String pack, int times) {
            corrupt.add(new long[] { f.offset(pack), f.offset(pack) + f.content.get(pack).length - 1, times });
        }

        int requestsFor(Fixture f, String pack) {
            int n = 0;
            synchronized (ranges) {
                for (long[] r : ranges) {
                    if (r[0] >= f.offset(pack) && r[0] < f.offset(pack) + f.content.get(pack).length) {
                        n++;
                    }
                }
            }
            return n;
        }

        void stop() {
            stopped = true;
            try {
                socket.close();
            } catch (IOException ignored) {
                // closed already
            }
            pool.shutdownNow();
        }

        /** One request per connection. */
        private void serve(java.net.Socket client) {
            try (java.net.Socket c = client) {
                java.io.BufferedReader in = new java.io.BufferedReader(
                        new java.io.InputStreamReader(c.getInputStream(), StandardCharsets.ISO_8859_1));
                String request = in.readLine();
                if (request == null) {
                    return;
                }
                String range = null;
                String line;
                while ((line = in.readLine()) != null && !line.isEmpty()) {
                    if (line.regionMatches(true, 0, "Range:", 0, 6)) {
                        range = line.substring(6).trim();
                    }
                }
                String path = request.split(" ")[1];
                OutputStream out = c.getOutputStream();
                requests.incrementAndGet();
                if (path.equals("/manifest.json")) {
                    if (manifestStatus != 200) {
                        head(out, manifestStatus, 0, null);
                        return;
                    }
                    byte[] body = manifest.getBytes(StandardCharsets.UTF_8);
                    head(out, 200, body.length, null);
                    out.write(body);
                    out.flush();
                } else if (path.equals("/archive.zip")) {
                    archive(out, range);
                } else {
                    head(out, 404, 0, null);
                }
            } catch (IOException e) {
                // the client went away mid-answer: a stop
            }
        }

        private static void head(OutputStream out, int code, long length, String contentRange) throws IOException {
            String text = "HTTP/1.1 " + code + " X\r\nContent-Length: " + length + "\r\nConnection: close\r\n"
                    + (contentRange != null ? "Content-Range: " + contentRange + "\r\n" : "") + "\r\n";
            out.write(text.getBytes(StandardCharsets.ISO_8859_1));
            out.flush();
        }

        private void archive(OutputStream out, String range) throws IOException {
            archiveRequests.incrementAndGet();
            byte[] all = archive;
            long from = 0;
            long to = all.length - 1;
            int code = 200;
            String contentRange = null;
            if (range != null && !ignoreRange) {
                String spec = range.substring("bytes=".length());
                if (spec.startsWith("-")) {
                    suffixRanges.incrementAndGet();
                    head(out, 501, 0, null);
                    return;
                }
                int dash = spec.indexOf('-');
                from = Long.parseLong(spec.substring(0, dash));
                to = Long.parseLong(spec.substring(dash + 1));
                if (from > to || to >= all.length) {
                    head(out, 416, 0, null);
                    return;
                }
                code = 206;
                contentRange = "bytes " + (from + shiftContentRange) + "-" + (to + shiftContentRange)
                        + "/" + all.length;
                ranges.add(new long[] { from, to });
                if (from == watchFrom && watchExisted == null) {
                    watchExisted = watchFile.exists();
                }
            }
            byte[] body = Arrays.copyOfRange(all, (int) from, (int) to + 1);
            synchronized (corrupt) {
                for (long[] c : corrupt) {
                    if (c[2] > 0 && from <= c[1] && to >= c[0]) {
                        c[2]--;
                        body[(int) (Math.max(from, c[0]) - from)] ^= 0x40;
                        break;
                    }
                }
            }
            head(out, code, body.length, contentRange);
            int at = 0;
            while (at < body.length) {
                int n = Math.min(4096, body.length - at);
                out.write(body, at, n);
                out.flush();
                archiveBytes.addAndGet(n);
                at += n;
                if (delayMs > 0) {
                    try {
                        Thread.sleep(delayMs);
                    } catch (InterruptedException e) {
                        return;
                    }
                }
            }
        }
    }

    // ================================================================ helpers

    private interface Condition {
        boolean holds() throws Exception;
    }

    private static void waitUntil(Condition c) throws Exception {
        long deadline = System.currentTimeMillis() + 60_000;
        while (!c.holds()) {
            if (System.currentTimeMillis() > deadline) {
                fail("timed out waiting");
            }
            Thread.sleep(5);
        }
    }

    private void assertInvalid(String json) {
        try {
            DataManifest.parse(json);
            fail("accepted: " + (json.length() > 200 ? json.substring(0, 200) : json));
        } catch (DataManifest.Invalid expected) {
            // as it should be
        }
    }

    private void assertNoParts() {
        for (String name : data.list()) {
            assertFalse(name + " left behind", name.contains(".part") || name.endsWith(".tmp"));
        }
    }

    private void oldLayout(String... roles) throws IOException {
        for (String role : roles) {
            write(new File(data, "xonotic-20261107-" + role + ".pk3"), "old " + role);
        }
    }

    private void assertOldLayout(boolean there, String... roles) {
        for (String role : roles) {
            assertEquals("xonotic-20261107-" + role + ".pk3", there,
                    new File(data, "xonotic-20261107-" + role + ".pk3").exists());
        }
    }

    /** Bytes of the named packs that are installed. */
    private long doneBytesOf(Fixture f, String... names) {
        long sum = 0;
        for (String n : names) {
            File file = new File(data, "godnotic-T-" + n + ".pk3");
            if (file.isFile()) {
                sum += file.length();
            } else {
                File state = new File(data, "godnotic-T-" + n + ".pk3.part.state");
                if (state.isFile()) {
                    try {
                        for (int p : chunkState(state, f.content.get(n).length)) {
                            sum += p;
                        }
                    } catch (IOException e) {
                        throw new AssertionError(e);
                    }
                }
            }
        }
        return sum;
    }

    private long chunkTotal(Fixture f, String name) throws IOException {
        File state = new File(data, "godnotic-T-" + name + ".pk3.part.state");
        long sum = 0;
        if (state.isFile()) {
            for (int p : chunkState(state, f.content.get(name).length)) {
                sum += p;
            }
        }
        return sum;
    }

    /** Reads a .part.state as PackFetch.ChunkState writes it: "XTPC", the size, the count, one int per chunk. */
    private static int[] chunkState(File file, long size) throws IOException {
        try (DataInputStream in = new DataInputStream(new FileInputStream(file))) {
            assertEquals(0x58545043, in.readInt());
            assertEquals(size, in.readLong());
            int[] done = new int[in.readInt()];
            assertEquals((size + CHUNK - 1) / CHUNK, done.length);
            for (int i = 0; i < done.length; i++) {
                done[i] = in.readInt();
            }
            return done;
        }
    }

    private String status() throws IOException {
        File f = new File(touch, DataStatus.STATUS_FILE);
        return f.isFile() ? read(f) : "";
    }

    private void request(String line) throws IOException {
        File f = new File(touch, DataStatus.REQUEST_FILE);
        f.getParentFile().mkdirs();
        write(f, line + "\n");
    }

    private static List<String> names(List<DataManifest.Pack> packs) {
        List<String> out = new ArrayList<>();
        for (DataManifest.Pack p : packs) {
            out.add(p.name);
        }
        return out;
    }

    /** The landed names in the fixture's (priority) order: what landed, whatever the threads' order. */
    private static List<String> sorted(List<String> landed, Fixture f) {
        List<String> out = new ArrayList<>();
        for (String name : f.content.keySet()) {
            if (landed.contains("godnotic-T-" + name + ".pk3")) {
                out.add("godnotic-T-" + name + ".pk3");
            }
        }
        assertEquals(out.size(), landed.size());
        return out;
    }

    private static String hex8(long crc) {
        return String.format("%08x", crc);
    }

    private static String sha256(byte[] bytes) {
        try {
            StringBuilder hex = new StringBuilder();
            for (byte b : MessageDigest.getInstance("SHA-256").digest(bytes)) {
                hex.append(String.format("%02x", b));
            }
            return hex.toString();
        } catch (Exception e) {
            throw new AssertionError(e);
        }
    }

    private static String read(File f) throws IOException {
        return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
    }

    private static void write(File f, String text) throws IOException {
        write(f, text.getBytes(StandardCharsets.UTF_8));
    }

    private static void write(File f, byte[] bytes) throws IOException {
        try (FileOutputStream out = new FileOutputStream(f)) {
            out.write(bytes);
        }
    }

    private static void delete(File f) {
        File[] children = f.listFiles();
        if (children != null) {
            for (File c : children) {
                delete(c);
            }
        }
        f.delete();
    }
}
