package io.github.dixonsolutions.xonotictouch.fe

import android.util.Log

/**
 * The Rust loader core (loader/crates/android, libgodnotic_android.so) reading
 * the UI bridge: each engine message goes in, the core applies it to its
 * store (the same core the Linux app uses) and answers with the renderer
 * operations it causes and the messages for the engine. This class only
 * plays those operations on the [UiRenderer] seam, on the UI thread, in
 * order. A build without the library, or a core that fails, falls back to
 * the Kotlin [UibClient] (FrontendHost) — never a blank screen.
 */
class RustCore private constructor(private val renderer: UiRenderer, private val send: (String) -> Unit) : UiClient {
    private var handle = nativeStart()
    private var state: Map<String, Any?> = emptyMap()

    override var onChanged: (() -> Unit)? = null
    override var ignoreRows = false
        set(v) { field = v; if (handle != 0L) nativeIgnoreRows(handle, v) }
    override val topKind: String get() = state.str("kind")
    override val canBack: Boolean get() = state.bool("canBack")
    override val modal: List<String> get() = state["modal"].list().map { it.toString() }
    override val hero: String get() = state.str("hero")

    private fun out(msg: String?) { if (!msg.isNullOrEmpty()) send(msg) }

    override fun hello(caps: Map<String, Any?>) = out(nativeHello(handle, Json.write(caps)))
    override fun sync() = out(nativeSync(handle))
    override fun event(ev: String, body: Map<String, Any?>) = out(nativeEvent(handle, ev, Json.write(body)))
    override fun window(source: String, offset: Int, limit: Int, sort: String, desc: Boolean, filter: String) =
        out(nativeWindow(handle, source, offset, limit, sort, desc, filter))
    override fun image(ref: String, w: Int, h: Int) = out(nativeImage(handle, ref, w, h))

    override fun receiveText(json: String) {
        if (handle == 0L) return
        val answer = nativeFeed(handle, json)
        if (answer.isNullOrEmpty()) throw IllegalStateException("the Rust core gave no answer")
        val r = Json.parse(answer).obj()
        var changed = false
        for (o in r["ops"].list()) {
            val op = o.obj()
            when (op.str("op")) {
                "begin" -> renderer.beginBatch()
                "end" -> { renderer.endBatch(); changed = true }
                "create" -> renderer.create(op.str("id"), op["parent"] as? String, op.int("index", -1), op.str("type"), op["props"].obj(), op["hints"].obj())
                "update" -> renderer.update(op.str("id"), op["props"].obj())
                "remove" -> renderer.remove(op.str("id"))
                "navigate" -> renderer.navigate(op.str("screen"), op.str("transition", "replace"))
                "dialog" -> renderer.dialog(op["id"] as? String)
                "toast" -> renderer.toast(op.str("text"), op.str("kind", "info"))
                "rows" -> renderer.listRows(op.str("source"), op.int("offset"), op.int("total"), op["rows"].list().map { it.obj() })
                "image" -> renderer.imageReady(op.str("ref"), op.str("path"))
            }
        }
        state = r["state"].obj()
        for (m in r["send"].list()) out(m as? String)
        if (changed) onChanged?.invoke()
    }

    override fun close() {
        if (handle != 0L) { nativeStop(handle); handle = 0L }
    }

    companion object {
        private val loaded: Boolean by lazy {
            try { System.loadLibrary("godnotic_android"); nativeProtocol() == 1 }
            catch (e: Throwable) { Log.i("Godnotic", "Native UI: no Rust core in this build (${e.javaClass.simpleName})"); false }
        }

        /** The core, or null when this build does not carry it. */
        fun create(renderer: UiRenderer, send: (String) -> Unit): RustCore? = if (loaded) RustCore(renderer, send) else null

        @JvmStatic private external fun nativeProtocol(): Int
        @JvmStatic private external fun nativeStart(): Long
        @JvmStatic private external fun nativeStop(h: Long)
        @JvmStatic private external fun nativeIgnoreRows(h: Long, on: Boolean)
        @JvmStatic private external fun nativeHello(h: Long, caps: String): String?
        @JvmStatic private external fun nativeSync(h: Long): String?
        @JvmStatic private external fun nativeFeed(h: Long, msg: String): String?
        @JvmStatic private external fun nativeEvent(h: Long, ev: String, body: String): String?
        @JvmStatic private external fun nativeWindow(h: Long, source: String, offset: Int, limit: Int, sort: String, desc: Boolean, filter: String): String?
        @JvmStatic private external fun nativeImage(h: Long, ref: String, w: Int, h2: Int): String?
    }
}
