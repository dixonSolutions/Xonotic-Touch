package io.github.dixonsolutions.xonotictouch

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
