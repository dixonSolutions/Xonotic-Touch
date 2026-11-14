package io.github.dixonsolutions.godnotic

import android.app.Application
import android.util.Log

/**
 * The resident JNI bridge (cpp/nativeui/bridge.c) between Android code and
 * whichever engine is loaded. Commands go into a bounded queue that only the
 * engine thread drains (NativeUI_Frame), so nothing here touches engine state
 * from the UI thread.
 *
 * Loaded on first use. An APK without libnativeui.so (a live update must never
 * crash on one) leaves [available] false and every call a no-op.
 */
object NativeBridge {
    @JvmStatic
    val available: Boolean = try {
        System.loadLibrary("nativeui")
        true
    } catch (e: UnsatisfiedLinkError) {
        Log.w("Godnotic", "Native UI bridge unavailable: ${e.message}")
        false
    }

    private external fun enqueueBytes(command: ByteArray): Boolean
    private external fun presented(): Boolean

    /** Queue a console command for the engine thread; false when the queue is full or the bridge is missing. */
    @JvmStatic
    fun enqueue(command: String): Boolean = available && enqueueBytes(command.toByteArray(Charsets.UTF_8))

    /** True once the engine has put its first frame on screen. */
    @JvmStatic
    fun framePresented(): Boolean = available && presented()

    // ---- Front-end bridge (bridge.c, docs/FRONTEND_BRIDGE.md) -------------
    // Natives an older libnativeui.so lacks: every call is guarded, so a new
    // dex over an old resident bridge (an AndySU engine swap mixes them)
    // only means "no native front end".
    private external fun feVersion(): Int
    private external fun feTakeBytes(all: Boolean): ByteArray?
    private external fun feBeat()
    private external fun feCancel()

    /** Protocol version of the resident bridge; 0 when it predates the front-end bridge. */
    @JvmStatic
    val frontendVersion: Int by lazy {
        if (!available) 0 else try { feVersion() } catch (e: UnsatisfiedLinkError) { 0 }
    }

    /** Called by bridge.c on the engine thread when a snapshot or event is waiting. */
    @Volatile @JvmStatic var onDirty: Runnable? = null

    @JvmStatic
    fun onFrontendDirty() {
        onDirty?.run()
    }

    /** Topics that changed (all of them with [all]) and pending events: "topic\u001fjson" records. */
    @JvmStatic
    fun frontendTake(all: Boolean): List<Pair<String, String>> {
        if (frontendVersion == 0) return emptyList()
        val bytes = try { feTakeBytes(all) } catch (e: UnsatisfiedLinkError) { null } ?: return emptyList()
        val out = ArrayList<Pair<String, String>>()
        for (rec in String(bytes, Charsets.UTF_8).split('\u001e')) {
            val sep = rec.indexOf('\u001f')
            if (sep > 0) out.add(rec.substring(0, sep) to rec.substring(sep + 1))
        }
        return out
    }

    @JvmStatic
    fun frontendBeat() {
        if (frontendVersion > 0) try { feBeat() } catch (e: UnsatisfiedLinkError) {}
    }

    /** Cancel a join, read by the engine between the items of a map load. */
    @JvmStatic
    fun frontendCancel() {
        if (frontendVersion > 0) try { feCancel() } catch (e: UnsatisfiedLinkError) {}
    }
}

/**
 * Named by the manifest of every host since the Kotlin shell, so it has to
 * stay. The console used to be an Android button and dialog added here; it is
 * the engine's own now (engine/darkplaces/touch_shellbtn.c), drawn the same on
 * every platform, and plugins are managed in the engine console (lua_mods,
 * lua_mod_enable ...) and the in-game Mods dialog. Old hosts whose manifest
 * does not name this class lose nothing: the game starts from the activities.
 */
class NativeUIApplication : Application()
