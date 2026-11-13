package io.github.dixonsolutions.xonotictouch;

import android.content.Context;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.util.Log;

/**
 * Wi-Fi locks held only while this device hosts a LAN game (touch_netstate
 * says "hosting", see touch_android.c), released as soon as it stops.
 *
 * Other players find the game with a broadcast (and the engine's replies to it
 * are ordinary unicast). Many phones drop broadcast and multicast packets in
 * the Wi-Fi driver to save power unless an app holds a
 * {@link WifiManager.MulticastLock}, so without it the host never hears the
 * search and the game is not listed. A high-performance (Android 10+:
 * low-latency) {@link WifiManager.WifiLock} keeps the radio out of power save,
 * which otherwise adds 100-300 ms to every packet with the screen off.
 *
 * Joining needs neither: the replies to a search are unicast, and so is the
 * game. Client isolation on a network (guest Wi-Fi, some hotspots) blocks
 * players from each other entirely; no lock helps there (docs/LAN.md).
 */
final class LanLocks {

    private static final String TAG = "Godnotic";

    private static WifiManager.MulticastLock multicast;
    private static WifiManager.WifiLock wifi;
    /** This hosting session's attempt, made once even when it failed. */
    private static boolean held;

    private LanLocks() {
    }

    /** Hold the locks while hosting, release them otherwise. Cheap when unchanged. */
    static synchronized void hosting(Context context, boolean on) {
        if (on == held) {
            return;
        }
        if (!on) {
            release();
            return;
        }
        held = true;
        WifiManager manager = (WifiManager) context.getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (manager == null) {
            return;
        }
        try {
            multicast = manager.createMulticastLock("Godnotic:lan");
            multicast.setReferenceCounted(false);
            multicast.acquire();
            int mode = Build.VERSION.SDK_INT >= 29
                    ? WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                    : WifiManager.WIFI_MODE_FULL_HIGH_PERF;
            wifi = manager.createWifiLock(mode, "Godnotic:lan");
            wifi.setReferenceCounted(false);
            wifi.acquire();
            Log.i(TAG, "Hosting a LAN game: Wi-Fi multicast and performance locks held");
        } catch (RuntimeException e) {
            // A missing permission on a stripped-down ROM: the game still runs,
            // it is only harder to find.
            Log.w(TAG, "Could not take the Wi-Fi locks for hosting", e);
        }
    }

    static synchronized void release() {
        held = false;
        if (multicast != null) {
            try {
                multicast.release();
            } catch (RuntimeException e) {
                Log.w(TAG, "Multicast lock release", e);
            }
            multicast = null;
        }
        if (wifi != null) {
            try {
                wifi.release();
            } catch (RuntimeException e) {
                Log.w(TAG, "Wi-Fi lock release", e);
            }
            wifi = null;
        }
    }
}
