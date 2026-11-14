package io.github.dixonsolutions.godnotic;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.util.HashMap;
import java.util.Map;

/** The Updates screen's decisions on an AndySU install (AndySUState): plain JVM, no Android. */
public class AndySUStateTest {

    @Test
    public void hostStatesAreSpelledTheMenusWay() {
        assertEquals("uptodate", AndySUState.normalize("up_to_date"));
        assertEquals("shell-update", AndySUState.normalize("shell_update"));
        assertEquals("staged", AndySUState.normalize("staged"));
        assertEquals("idle", AndySUState.normalize(""));
        assertEquals("idle", AndySUState.normalize(null));
        assertTrue(AndySUState.busy("downloading"));
        assertFalse(AndySUState.busy("staged"));
        assertTrue(AndySUState.endsCheck("uptodate"));
        assertTrue(AndySUState.endsCheck("deferred"));
        assertFalse(AndySUState.endsCheck("checking"));
        assertFalse(AndySUState.endsCheck("idle"));
    }

    @Test
    public void automaticOffOnlyChecks() {
        for (boolean wifi : new boolean[] { false, true })
            for (boolean metered : new boolean[] { false, true })
                for (boolean staged : new boolean[] { false, true })
                    for (boolean fg : new boolean[] { false, true })
                        assertEquals("check_only", AndySUState.targetMode(false, wifi, metered, staged, fg));
    }

    @Test
    public void automaticOnAnyNetworkIsAuto() {
        assertEquals("auto", AndySUState.targetMode(true, false, true, false, true));
        assertEquals("auto", AndySUState.targetMode(true, false, true, false, false));
    }

    @Test
    public void wifiOnlyFollowsTheConnectionInTheForeground() {
        assertEquals("auto", AndySUState.targetMode(true, true, false, false, true));
        assertEquals("check_only", AndySUState.targetMode(true, true, true, false, true));
        // even with a release staged: a newer one must not download on mobile data
        assertEquals("check_only", AndySUState.targetMode(true, true, true, true, true));
    }

    @Test
    public void wifiOnlyInTheBackgroundKeepsAStagedReleaseApplying() {
        // the host promotes a staged release at the next start only in auto mode
        assertEquals("auto", AndySUState.targetMode(true, true, true, true, false));
        assertEquals("auto", AndySUState.targetMode(true, true, false, true, false));
        // nothing staged: the host's periodic job runs on any network, so only check
        assertEquals("check_only", AndySUState.targetMode(true, true, false, false, false));
    }

    @Test
    public void checkNowAndDownloadNow() {
        assertNull("auto already downloads", AndySUState.manualMode(true, false, true, false));
        assertNull("Wi-Fi only on mobile data: check, then offer Download now",
                AndySUState.manualMode(true, true, true, false));
        assertNull(AndySUState.manualMode(false, true, true, false));
        assertEquals("fetch it, never apply it unasked", "download_only",
                AndySUState.manualMode(false, false, true, false));
        assertEquals("download_only", AndySUState.manualMode(false, true, false, false));
        assertEquals("auto", AndySUState.manualMode(true, true, true, true));
        assertEquals("download_only", AndySUState.manualMode(false, true, true, true));
    }

    @Test
    public void holdSaysWhyAnAvailableReleaseWaits() {
        assertEquals("wifi", AndySUState.hold("available", true, true, true));
        assertEquals("auto", AndySUState.hold("available", false, true, true));
        assertEquals("", AndySUState.hold("available", true, false, true));
        assertEquals("", AndySUState.hold("staged", false, true, true));
    }

    @Test
    public void recordKeepsTheFirstEightLinesAndAddsExtras() {
        AndySUState.Record r = new AndySUState.Record();
        r.state = "staged";
        r.installed = "1.2.360";
        r.latest = "1.2.361";
        r.percent = 140;
        r.message = "two\nlines";
        r.auto = true;
        r.mode = "auto";
        r.put("staged", "1.2.361").flag("wifi", true).flag("live", null).put("eta", 13).put("empty", "");
        String[] lines = r.encode().split("\n", -1);
        assertEquals("1", lines[0]);
        assertEquals("staged", lines[1]);
        assertEquals("1.2.360", lines[2]);
        assertEquals("1.2.361", lines[3]);
        assertEquals("100", lines[4]);
        assertEquals("two lines", lines[5]);
        assertEquals("on", lines[6]);
        assertEquals("auto", lines[7]);
        assertEquals("staged=1.2.361", lines[8]);
        assertEquals("wifi=on", lines[9]);
        assertEquals("live=na", lines[10]);
        assertEquals("eta=13", lines[11]);
        assertEquals("", lines[12]); // empty values are left out
        assertEquals(13, lines.length);
    }

    private static Map<String, Long> parts(Object... kv) {
        Map<String, Long> m = new HashMap<>();
        for (int i = 0; i < kv.length; i += 2) m.put((String) kv[i], ((Number) kv[i + 1]).longValue());
        return m;
    }

    @Test
    public void progressCountsWhatTheDownloadAdded() {
        AndySUState.Progress p = new AndySUState.Progress();
        // a resumed blob already had 1 MB on disk; the host said 9 MB were left
        p.start(9_000_000, parts("a", 1_000_000));
        p.sample(0, parts("a", 1_000_000), k -> -1);
        assertEquals(0, p.done);
        assertEquals(-1, p.eta);
        p.sample(1000, parts("a", 2_000_000), k -> -1);
        assertEquals(1_000_000, p.done);
        assertEquals(1_000_000, p.speed);
        assertEquals(8, p.eta);
        // "a" finished (renamed to its 4 MB blob), "b" started
        p.sample(2000, parts("b", 500_000), k -> "a".equals(k) ? 4_000_000 : -1);
        assertEquals(3_000_000 + 500_000, p.done);
        assertEquals(38, p.percent());
        assertEquals(1_750_000, p.speed);
        // a delta whose result has another key: its last size counts
        p.sample(3000, parts(), k -> -1);
        assertEquals(3_500_000, p.done);
        p.sample(4000, parts("c", 99_000_000), k -> -1);
        assertEquals("never past the total", 9_000_000, p.done);
        assertEquals(100, p.percent());
        assertEquals(0, p.eta);
        p.reset();
        assertFalse(p.started());
        assertEquals(0, p.percent());
    }

    @Test
    public void progressWithoutATotalStillCounts() {
        AndySUState.Progress p = new AndySUState.Progress();
        p.start(0, parts());
        p.sample(0, parts("x", 10), k -> -1);
        assertEquals(10, p.done);
        assertEquals(0, p.percent());
        assertEquals(-1, p.eta);
    }

    @Test
    public void progressText() {
        assertEquals("999 B", AndySUState.bytes(999));
        assertEquals("850 KB", AndySUState.bytes(850_000));
        assertEquals("12.3 MB", AndySUState.bytes(12_340_000));
        assertEquals("1.50 GB", AndySUState.bytes(1_500_000_000L));
        assertEquals("45 s", AndySUState.duration(45));
        assertEquals("2 min", AndySUState.duration(61));
        assertEquals("1 h 5 min", AndySUState.duration(3900));
        assertEquals("", AndySUState.duration(-1));
        AndySUState.Progress p = new AndySUState.Progress();
        p.start(40_000_000, parts());
        p.sample(0, parts("k", 0), k -> -1);
        p.sample(1000, parts("k", 2_100_000), k -> -1);
        assertEquals("2.1 MB of 40.0 MB · 2.1 MB/s · 19 s left", AndySUState.progressLine(p));
    }
}
