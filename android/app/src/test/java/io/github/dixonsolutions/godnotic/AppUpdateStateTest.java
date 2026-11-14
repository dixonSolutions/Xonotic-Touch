package io.github.dixonsolutions.godnotic;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

/** Settings &gt; Updates without an AndySU host: the app updater's decisions (AppUpdateState). */
public class AppUpdateStateTest {
    private static final String UNMETERED = AppUpdateState.NET_UNMETERED;
    private static final String METERED = AppUpdateState.NET_METERED;
    private static final String OFFLINE = AppUpdateState.NET_OFFLINE;

    @Test
    public void backgroundDownloadsFollowWifiOnly() {
        assertTrue(AppUpdateState.mayDownloadUnasked(true, UNMETERED));
        assertFalse(AppUpdateState.mayDownloadUnasked(true, METERED));
        assertTrue(AppUpdateState.mayDownloadUnasked(false, METERED));
        assertFalse(AppUpdateState.mayDownloadUnasked(false, OFFLINE));
        assertFalse(AppUpdateState.mayDownloadUnasked(true, OFFLINE));
    }

    @Test
    public void holdSaysWhyAFoundReleaseWaits() {
        assertEquals("auto", AppUpdateState.hold("available", false, true, UNMETERED));
        assertEquals("wifi", AppUpdateState.hold("available", true, true, METERED));
        assertEquals("", AppUpdateState.hold("available", true, false, METERED));
        assertEquals("offline", AppUpdateState.hold("paused", true, false, OFFLINE));
        assertEquals("", AppUpdateState.hold("available", true, true, UNMETERED));
        // only a release that waits has a reason
        assertEquals("", AppUpdateState.hold("downloading", false, true, METERED));
        assertEquals("", AppUpdateState.hold("uptodate", false, true, METERED));
        assertEquals("", AppUpdateState.hold("ready", false, true, METERED));
    }

    @Test
    public void checkResults() {
        assertEquals("uptodate", AppUpdateState.checkResult(false, false));
        assertEquals("available", AppUpdateState.checkResult(true, false));
        // a check that could not ask says so, even with a release remembered from before
        assertEquals("error", AppUpdateState.checkResult(false, true));
        assertEquals(0, AppUpdateState.minutesSince(1_000_000, 1_059_999));
        assertEquals(61, AppUpdateState.minutesSince(0, 61 * 60_000 + 5));
        assertEquals(0, AppUpdateState.minutesSince(5_000_000, 1_000));
    }

    @Test
    public void liveAssetAndAbi() {
        assertTrue(AppUpdateState.isLiveAsset("Godnotic-live-1.2.205-arm64.apk"));
        assertFalse(AppUpdateState.isLiveAsset("Godnotic-1.2.205-arm64-v8a.apk"));
        assertFalse(AppUpdateState.isLiveAsset(null));
        assertEquals("arm64-v8a", AppUpdateState.abiOfLibDir("arm64", "x"));
        assertEquals("armeabi-v7a", AppUpdateState.abiOfLibDir("arm", "x"));
        assertEquals("x86_64", AppUpdateState.abiOfLibDir("x86_64", "x"));
        assertEquals("arm64-v8a", AppUpdateState.abiOfLibDir("lib", "arm64-v8a"));
        assertEquals("", AppUpdateState.abiOfLibDir(null, null));
    }

    @Test
    public void extrasAreOneLineEachInOrder() {
        AppUpdateState.Extras x = new AppUpdateState.Extras()
                .put("via", "app").put("channel", "stable").put("abi", "arm64-v8a")
                .flag("wifi", true).put("live", "na").put("hold", "").put("size", 34_552_331L)
                .put("message", "two\nlines");
        assertEquals("via=app\nchannel=stable\nabi=arm64-v8a\nwifi=on\nlive=na\nsize=34552331\nmessage=two lines\n",
                x.encode());
        assertEquals("", x.get("hold"));
        assertEquals("on", x.get("wifi"));
    }

    @Test
    public void rateGivesSpeedAndTimeLeft() {
        AppUpdateState.Rate r = new AppUpdateState.Rate();
        r.sample(0, 0, 10_000_000);
        assertEquals(0, r.speed);
        assertEquals(-1, r.eta);
        r.sample(1000, 1_000_000, 10_000_000);
        r.sample(2000, 2_000_000, 10_000_000);
        assertEquals(1_000_000, r.speed);
        assertEquals(8, r.eta);
        // a download that starts over (the cache was not this asset) starts the rate over
        r.sample(3000, 100, 10_000_000);
        assertEquals(0, r.speed);
        // unknown total: speed, no time left
        AppUpdateState.Rate u = new AppUpdateState.Rate();
        u.sample(0, 0, -1);
        u.sample(500, 500_000, -1);
        assertEquals(1_000_000, u.speed);
        assertEquals(-1, u.eta);
    }
}
