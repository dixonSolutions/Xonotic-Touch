package io.github.dixonsolutions.godnotic.fe

/**
 * The narrow seam between whatever reads the engine's UI bridge and the
 * Android views that draw it (docs/UI_BRIDGE.md).
 *
 * Operations come in (create/update/remove a node, show a screen or a
 * dialog, a toast, rows of a data source, a picture that is ready); user
 * input goes out through [UiEvents]. It mirrors the renderer trait of the
 * Rust loader core (loader/crates/core, work/linuxui), which on Android will
 * drive it through loader/crates/android; until then [UibClient] (a small
 * Kotlin reader of snapshots and diffs) produces the operations.
 */
interface UiRenderer {
    /** Operations between begin and end are one frame's worth. */
    fun beginBatch() {}
    fun endBatch() {}
    /** A node under [parent] (null: a screen or dialog root) at [index] (-1: last). */
    fun create(id: String, parent: String?, index: Int, type: String, props: Map<String, Any?>, hints: Map<String, Any?>)
    /** The node's props, whole (a diff carries whole nodes). */
    fun update(id: String, props: Map<String, Any?>)
    fun remove(id: String)
    /** Show a created root as the current screen; [transition] "push", "pop" or "replace". */
    fun navigate(screen: String, transition: String)
    /** Show a created root as a modal dialog or sheet; null closes the open one. */
    fun dialog(id: String?)
    fun toast(text: String, kind: String)
    /** Rows [offset, offset + rows.size) of data source [source] of [total] rows. */
    fun listRows(source: String, offset: Int, total: Int, rows: List<Map<String, Any?>>)
    /** A picture asked for with [UiEvents.image] is on disk at [path] ("" if there is none). */
    fun imageReady(ref: String, path: String)
}

/** What the views report: section 6 events, window and image requests. */
interface UiEvents {
    /** An action object ({id, label, kind, confirm ...}) invoked on [node], after any confirm. */
    fun action(node: String, action: Map<String, Any?>)
    fun value(node: String, value: Any?)
    fun submit(node: String, value: Any?)
    fun row(list: String, key: String, kind: String = "activate")
    fun window(list: String, source: String, offset: Int, limit: Int, sort: String, desc: Boolean, filter: String)
    /** The rows on screen; [touching] whether a finger is on the list (its source then holds still). */
    fun visible(list: String, offset: Int, limit: Int, touching: Boolean?) {}
    fun image(ref: String, w: Int, h: Int)
    fun back()
}
