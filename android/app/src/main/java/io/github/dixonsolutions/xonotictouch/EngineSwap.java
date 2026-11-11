package io.github.dixonsolutions.xonotictouch;

import android.content.Context;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Live engine swap under an AndySU host (Host API level 2, D29), by reflection so the same sources
 * build and run without AndySU (like {@link AndySUCompat}; no compile dependency on com.andysu.api).
 *
 * <p>{@link #register} hands the host a {@code LiveSwapHandler}. When the host has a staged release
 * that only changes native code and content, it calls {@code onLiveSwap(slotDir, version, cb)}: this
 * class finds the release's {@code libmain*.so} and asks the resident loader
 * ({@code libandysu_xonotic_loader.so}, {@link #nativeRequestSwap}) to swap the engine in this
 * process. The loader reports back through {@link #onEngineEvent} (started, failed(reason),
 * healthy) and {@link #beforeEngineStart} (unpack the new release's data between the old engine's
 * teardown and the new engine's start), both called from native threads.
 */
final class EngineSwap {
    private static final String TAG = "XonoticTouch";
    private static final int EV_STARTED = 1;
    private static final int EV_FAILED = 2;
    private static final int EV_HEALTHY = 3;

    private static volatile Context appContext;
    private static volatile Object callback;       // com.andysu.api.LiveSwapCallback
    private static volatile Object host;           // com.andysu.api.HostApi
    private static volatile String fromVersion;    // the release running before the swap: where a rollback goes
    private static volatile String toVersion;      // the release being swapped in

    private EngineSwap() {}

    /** The loader answers for the engine library it loads; null means the swap was accepted. */
    static native String nativeRequestSwap(String enginePath);

    static void register(Context context) {
        appContext = context.getApplicationContext();
        try {
            Class<?> hostClass = Class.forName("com.andysu.api.Host");
            Object current = hostClass.getMethod("current").invoke(null);
            if (current == null) {
                return; // not under an AndySU host
            }
            Class<?> api = Class.forName("com.andysu.api.HostApi");
            int level = (Integer) api.getMethod("apiLevel").invoke(current);
            if (level < 2) {
                return; // an older host: no live native swaps, updates apply on the next start
            }
            Class<?> handlerClass = Class.forName("com.andysu.api.LiveSwapHandler");
            InvocationHandler ih = (proxy, method, args) -> {
                if (method.getName().equals("onLiveSwap")) {
                    return onLiveSwap((File) args[0], (String) args[1], args[2]);
                }
                return method.getName().equals("hashCode") ? System.identityHashCode(proxy)
                        : method.getName().equals("equals") ? proxy == args[0] : "XonoticLiveSwapHandler";
            };
            Object handler = Proxy.newProxyInstance(handlerClass.getClassLoader(), new Class<?>[] { handlerClass }, ih);
            api.getMethod("setLiveSwapHandler", handlerClass).invoke(current, handler);
            host = current;
            Log.i(TAG, "Live engine swap handler registered with the AndySU host (API level " + level + ")");
        } catch (Throwable t) {
            Log.w(TAG, "AndySU live swap not available", t);
        }
    }

    static void unregister() {
        Object h = host;
        host = null;
        if (h == null) {
            return;
        }
        try {
            Class<?> api = Class.forName("com.andysu.api.HostApi");
            api.getMethod("setLiveSwapHandler", Class.forName("com.andysu.api.LiveSwapHandler")).invoke(h, (Object) null);
        } catch (Throwable t) {
            // the host is gone or older: nothing to undo
        }
    }

    /** Host thread: main. Returns true to accept (the loader then swaps and reports through callbacks). */
    private static boolean onLiveSwap(File slotDir, String version, Object cb) {
        File lib = findEngine(slotDir);
        if (lib == null) {
            Log.w(TAG, "Live swap to " + version + " declined: no libmain in " + slotDir);
            return false;
        }
        // Before the loader may start the swap: the host has not switched its slot yet, so this is
        // still the running release, and a rollback goes back to it.
        fromVersion = AndySUCompat.payloadVersion();
        toVersion = version;
        try {
            String refused = nativeRequestSwap(lib.getAbsolutePath());
            if (refused != null) {
                Log.w(TAG, "Live swap to " + version + " declined by the loader: " + refused);
                fromVersion = toVersion = null;
                return false;
            }
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "Live swap to " + version + " declined: no loader in this build", e);
            fromVersion = toVersion = null;
            return false;
        }
        callback = cb;
        Log.i(TAG, "Live swap to " + version + " accepted: " + lib);
        return true;
    }

    private static File findEngine(File slotDir) {
        for (String abi : Build.SUPPORTED_ABIS) {
            File dir = new File(slotDir, "lib/" + abi);
            File exact = new File(dir, "libmain.so");
            if (exact.isFile()) {
                return exact;
            }
            File[] hashed = dir.listFiles((d, name) -> name.startsWith("libmain.") && name.endsWith(".so"));
            if (hashed != null && hashed.length > 0) {
                return hashed[0];
            }
        }
        return null;
    }

    private static void callback(String method, String reason) {
        Object cb = callback;
        if (cb == null) {
            return;
        }
        try {
            Class<?> cls = Class.forName("com.andysu.api.LiveSwapCallback");
            if (reason == null) {
                cls.getMethod(method).invoke(cb);
            } else {
                cls.getMethod(method, String.class).invoke(cb, reason);
            }
        } catch (Throwable t) {
            Log.w(TAG, "Could not report " + method + " to the AndySU host", t);
        }
    }

    /** Native thread (SDL thread or the loader's): the loader's progress, forwarded to the host. */
    static void onEngineEvent(int kind, String detail) {
        switch (kind) {
            case EV_STARTED:
                callback("started", null);
                break;
            case EV_FAILED:
                callback("failed", detail.isEmpty() ? "the new engine failed to start" : detail);
                callback = null;
                break;
            case EV_HEALTHY:
                try {
                    Object h = host;
                    if (h != null) {
                        Class.forName("com.andysu.api.HostApi").getMethod("markHealthy").invoke(h);
                    }
                } catch (Throwable t) {
                    Log.w(TAG, "Could not mark the release healthy", t);
                }
                callback = null;
                break;
            default:
                break;
        }
    }

    /**
     * SDL thread, after the old engine is gone and before the next one starts: the game data
     * (QuakeC progs, menu skin) has to belong to the release that is about to run, unpacked from
     * that release's assets. {@code swapped} 1 = the new release (the host already switched the
     * app's asset loader), 2 = the in-process rollback to the previous one. 0 = go on, anything
     * else abandons the start.
     */
    static int beforeEngineStart(int swapped) {
        Context c = appContext;
        if (swapped < 1 || c == null) {
            return 0;
        }
        GameData data = new GameData(c);
        long t0 = System.currentTimeMillis();
        try {
            if (fromVersion != null) { // driven by an AndySU host: its slot switch has to be done first
                // The host switches its slot (version, asset loader) on the main thread: right after
                // onLiveSwap returns for the swap (same main-thread task), and in onLiveFailed, posted
                // by the loader's failed(reason) just before this call, for the rollback. A task queued
                // now runs after either, so once it has run the assets are the right release's.
                awaitMainLooper();
                String want = swapped == 1 ? toVersion : fromVersion;
                if (!want.equals(AndySUCompat.payloadVersion())) {
                    throw new IOException("the host is on " + AndySUCompat.payloadVersion() + ", not " + want);
                }
            }
            if (swapped == 1 || fromVersion == null) { // fromVersion null: no host, nothing changes between releases
                data.prepare((text, note, percent) -> { });
            } else {
                data.prepareAfterRollback((text, note, percent) -> { }, toVersion);
            }
            Log.i("AndySU-Event", "{\"event\":\"game_data_prepared\",\"pid\":" + Process.myPid() + ",\"swapped\":" + swapped
                    + ",\"release\":\"" + AndySUCompat.payloadVersion() + "\",\"data_version\":\"" + data.bundleVersion()
                    + "\",\"ms\":" + (System.currentTimeMillis() - t0) + "}");
            return 0;
        } catch (IOException | RuntimeException | InterruptedException e) {
            Log.e(TAG, "Preparing the game data failed (swapped=" + swapped + ")", e);
            Log.i("AndySU-Event", "{\"event\":\"game_data_failed\",\"pid\":" + Process.myPid() + ",\"swapped\":" + swapped + "}");
            if (swapped == 2) {
                // The previous engine must not run on a mix of two releases' data. Make the next
                // start unpack everything and let the host relaunch the app now; if that is not
                // possible the loader stops (return 1) and the next start does it.
                data.invalidateBundle();
                restartThroughHost();
            }
            return 1;
        }
    }

    /** Returns once the main thread has run everything queued before this call. */
    private static void awaitMainLooper() throws InterruptedException, IOException {
        CountDownLatch done = new CountDownLatch(1);
        new Handler(Looper.getMainLooper()).post(done::countDown);
        if (!done.await(10, TimeUnit.SECONDS)) {
            throw new IOException("the main thread did not process the host's rollback in time");
        }
    }

    /** {@code HostApi.updates().restartNow()}: the host relaunches the app (does not return). */
    private static void restartThroughHost() {
        try {
            Object h = host;
            if (h == null) {
                return;
            }
            Object updates = Class.forName("com.andysu.api.HostApi").getMethod("updates").invoke(h);
            Class.forName("com.andysu.api.UpdateClient").getMethod("restartNow").invoke(updates);
        } catch (Throwable t) {
            Log.w(TAG, "Could not ask the AndySU host to relaunch", t);
        }
    }
}
