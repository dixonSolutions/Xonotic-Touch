package io.github.dixonsolutions.godnotic.fe

/**
 * The UI bridge's store as the renderer sees it (docs/UI_BRIDGE.md, protocol
 * 1): a flat map of nodes keyed by id, each {id, type, props, children (ids)},
 * the root `app` whose props.stack / props.modal say what is shown. Plain
 * Kotlin with no Android classes, so it runs in JVM unit tests.
 *
 * This is the interim Kotlin reader the coordinator asked for (snapshot,
 * diff, list windows) until the Rust loader core (loader/crates/core) drives
 * the renderer over JNI; [UibClient] turns store changes into [UiRenderer]
 * operations, which is the seam the core will use.
 */
class UibNode(val id: String, val type: String, val props: Map<String, Any?>, val children: List<String>, val origin: String) {
    fun str(key: String, def: String = "") = props.str(key, def)
    fun bool(key: String, def: Boolean = false) = props.bool(key, def)

    companion object {
        fun parse(json: Any?): UibNode? {
            val m = json.obj()
            val id = m.str("id")
            if (id.isEmpty()) return null
            return UibNode(id, m.str("type", "unknown"), m["props"].obj(), m["children"].list().map { it.toString() }, m.str("origin", "core"))
        }
    }
}

class UibStore {
    val nodes = LinkedHashMap<String, UibNode>()
    var rev = -1L
        private set
    var session = ""

    fun node(id: String) = nodes[id]
    val app: UibNode? get() = nodes["app"]

    fun snapshot(msg: Map<String, Any?>) {
        nodes.clear()
        for (n in msg["nodes"].list()) UibNode.parse(n)?.let { nodes[it.id] = it }
        rev = msg.num("rev").toLong()
    }

    /** Section 3.4: false when the diff does not apply (base mismatch): the caller sends `sync`. */
    fun diff(msg: Map<String, Any?>, changed: MutableSet<String>, deleted: MutableSet<String>): Boolean {
        if (msg.num("base").toLong() != rev) return false
        for (d in msg["del"].list()) { val id = d.toString(); if (nodes.remove(id) != null) deleted.add(id) }
        for (n in msg["set"].list()) UibNode.parse(n)?.let { nodes[it.id] = it; changed.add(it.id) }
        rev = msg.num("rev").toLong()
        return true
    }

    /** Every child id resolves (3.4 step 3); false means: ask for a snapshot. */
    fun consistent(): Boolean = nodes.values.all { n -> n.children.all { nodes.containsKey(it) } }

    /** Ids of [root] and everything under it, parents first. */
    fun subtree(root: String, out: MutableList<String> = ArrayList()): List<String> {
        val n = nodes[root] ?: return out
        if (out.size > 8192) return out
        out.add(root)
        for (c in n.children) subtree(c, out)
        return out
    }
}

/**
 * Feeds UI bridge messages into a [UiRenderer]: creates the views of the
 * screens on the stack and the dialogs on `modal`, updates the nodes a diff
 * names (their views are kept, so a value flips and a bar slides instead of
 * the screen being rebuilt), reconciles children whose id list changed, and
 * passes rows and pictures through. Unknown message types are ignored.
 */
/**
 * What reads the UI bridge for the views: the Kotlin [UibClient] or the Rust
 * core ([RustCore], loader/crates/android). FrontendHost talks to this only.
 */
interface UiClient {
    fun hello(caps: Map<String, Any?>)
    fun sync()
    fun event(ev: String, body: Map<String, Any?>)
    fun window(source: String, offset: Int, limit: Int, sort: String = "", desc: Boolean = false, filter: String = "")
    fun image(ref: String, w: Int, h: Int)
    /** One engine message as it came off the bridge (JSON text). */
    fun receiveText(json: String)
    /** Rows come from elsewhere (test builds generate them). */
    var ignoreRows: Boolean
    /** After every snapshot or diff that applied. */
    var onChanged: (() -> Unit)?
    /** The top screen's `kind` (loading, detail ...). */
    val topKind: String
    val canBack: Boolean
    /** Dialog ids shown over the stack. */
    val modal: List<String>
    /** The hero's picture ref (Home's Play map). */
    val hero: String
    /** `app.reserved` (docs/UI_BRIDGE.md 4.12): rects in dp the engine draws its own controls in. */
    val reserved: List<Map<String, Any?>>
    fun close() {}
}

class UibClient(private val renderer: UiRenderer, private val send: (String) -> Unit) : UiClient {
    val store = UibStore()
    private val built = HashSet<String>()   // node ids whose views exist
    private var shownStack: List<String> = emptyList()
    private var shownModal: List<String> = emptyList()
    private var nextId = 1L
    override var onChanged: (() -> Unit)? = null
    override var ignoreRows = false
    override val topKind: String get() = store.app?.props?.get("stack").list().lastOrNull()?.let { store.node(it.toString())?.str("kind") } ?: ""
    override val canBack: Boolean get() = store.app?.bool("canBack") ?: false
    override val modal: List<String> get() = store.app?.props?.get("modal").list().map { it.toString() }
    // the map Home's Play starts: the hero's picture, or the Singleplayer card's (Home since work/screens)
    override val hero: String get() = (store.nodes.values.firstOrNull { it.type == "hero" }
        ?: store.nodes.values.firstOrNull { it.type == "card" && it.props["primary"].obj().str("id") == "sp" })?.str("image") ?: ""
    override val reserved: List<Map<String, Any?>> get() = store.app?.props?.get("reserved").list().map { it.obj() }
    override fun receiveText(json: String) = receive(Json.parseOrNull(json).obj())

    private fun msg(t: String, body: Map<String, Any?>): String =
        Json.write(linkedMapOf<String, Any?>("uib" to 1, "t" to t, "id" to nextId++) + body)

    override fun hello(caps: Map<String, Any?>) = send(msg("hello", mapOf("schema" to "1.0",
        "renderer" to mapOf("name" to "godnotic-android", "version" to "0.1"), "caps" to caps)))
    override fun sync() = send(msg("sync", mapOf("have" to store.rev)))
    override fun event(ev: String, body: Map<String, Any?>) = send(msg("ev", mapOf<String, Any?>("ev" to ev) + body))
    override fun window(source: String, offset: Int, limit: Int, sort: String, desc: Boolean, filter: String) =
        send(msg("window", linkedMapOf<String, Any?>("source" to source, "offset" to offset, "limit" to minOf(limit, 200)).apply {
            if (sort.isNotEmpty()) { put("sort", sort); put("desc", desc) }
            if (filter.isNotEmpty()) put("filter", filter)
        }))
    override fun image(ref: String, w: Int, h: Int) = send(msg("image", mapOf("ref" to ref, "w" to w, "h" to h)))

    /** One engine message (already parsed). */
    fun receive(m: Map<String, Any?>) {
        when (m.str("t")) {
            "snapshot" -> {
                store.snapshot(m)
                if (!store.consistent()) { sync(); return }
                rebuildAll()
                onChanged?.invoke()
            }
            "diff" -> {
                val changed = LinkedHashSet<String>()
                val deleted = LinkedHashSet<String>()
                if (!store.diff(m, changed, deleted) || !store.consistent()) { sync(); return }
                applyChanges(changed, deleted)
                onChanged?.invoke()
            }
            "rows" -> if (!ignoreRows) renderer.listRows(m.str("source"), m.int("offset"), m.int("total"), m["rows"].list().map { it.obj() })
            "source" -> if (!ignoreRows) renderer.listRows(m.str("source"), 0, m.int("total"), emptyList())
            "image" -> renderer.imageReady(m.str("ref"), m.str("path"))
            "error" -> if (m.str("code") == "not_found") m.str("ref").takeIf { it.isNotEmpty() }?.let { renderer.imageReady(it, "") }
            "hello", "ack", "pong", "bye" -> {}
        }
    }

    private fun rebuildAll() {
        renderer.beginBatch()
        for (id in built.toList()) if (store.node(id) == null || isRoot(id)) renderer.remove(id)
        built.clear()
        shownStack = emptyList()
        shownModal = emptyList()
        showApp(force = true)
        renderer.endBatch()
    }

    private fun isRoot(id: String) = id in shownStack || id in shownModal

    private fun build(root: String, parent: String?, index: Int) {
        val n = store.node(root) ?: return
        renderer.create(n.id, parent, index, n.type, n.props, n.props["hints"].obj())
        built.add(n.id)
        for (c in n.children) build(c, n.id, -1)
    }

    private fun showApp(force: Boolean) {
        val app = store.app ?: return
        val stack = app.props["stack"].list().map { it.toString() }.filter { store.node(it) != null }
        val modal = app.props["modal"].list().map { it.toString() }.filter { store.node(it) != null }
        val toasts = app.children.mapNotNull { store.node(it) }.filter { it.type == "toast" }
        if (force || stack != shownStack) {
            val top = stack.lastOrNull()
            if (top != null) {
                if (top !in built) build(top, null, -1)
                val transition = when {
                    shownStack.isEmpty() -> "replace"
                    stack.size > shownStack.size -> "push"
                    stack.size < shownStack.size -> "pop"
                    else -> "replace"
                }
                renderer.navigate(top, transition)
            }
            // screens that left the stack: their views go
            for (old in shownStack) if (old !in stack) dropTree(old)
            shownStack = stack
        }
        if (force || modal != shownModal) {
            for (old in shownModal) if (old !in modal) dropTree(old)
            val top = modal.lastOrNull()
            if (top != null && top !in built) build(top, null, -1)
            renderer.dialog(top)
            shownModal = modal
        }
        for (t in toasts) if (t.id !in built) {
            built.add(t.id)
            renderer.toast(t.str("text"), t.str("kind", "info"))
        }
    }

    private fun dropTree(root: String) {
        renderer.remove(root)
        val gone = ArrayList<String>()
        for (id in built) if (store.node(id) == null || id == root) gone.add(id)
        built.removeAll(gone.toSet())
        built.remove(root)
    }

    private val lastChildren = HashMap<String, List<String>>()

    private fun applyChanges(changed: Set<String>, deleted: Set<String>) {
        renderer.beginBatch()
        for (id in deleted) if (id in built) { renderer.remove(id); built.remove(id) }
        for (id in changed) {
            if (id == "app") continue
            val n = store.node(id) ?: continue
            if (id !in built) continue // built when its parent shows it
            renderer.update(id, n.props)
            // children: views for ids that left go, new ids are built at their place
            val want = n.children
            for ((i, c) in want.withIndex()) if (c !in built) build(c, id, i)
        }
        // removals of children that are no longer referenced by anyone
        val referenced = HashSet<String>()
        for (n in store.nodes.values) referenced.addAll(n.children)
        for (id in built.toList()) if (id !in referenced && !isRoot(id) && store.node(id)?.type != "toast" && store.node(id) != null && id != "app") {
            if (id !in (store.app?.props?.get("stack").list().map { it.toString() } + store.app?.props?.get("modal").list().map { it.toString() })) {
                renderer.remove(id); built.remove(id)
            }
        }
        showApp(force = false)
        renderer.endBatch()
    }
}

/**
 * A virtualised list's rows, fetched a window at a time: the renderer asks
 * [missing] for what is on screen plus a margin, sends one request for it,
 * and [accept] files the reply. Rows already held are never asked for
 * again until [invalidate].
 */
class ListWindow(val listId: String, val pageSize: Int = 40) {
    var total = -1
        private set
    private val rows = HashMap<Int, Map<String, Any?>>()
    private val asked = HashSet<Int>() // page numbers requested and not yet answered

    fun row(i: Int): Map<String, Any?>? = rows[i]
    val loaded: Int get() = rows.size

    /** The first page to request for rows [first]..[last], or null when all are here or asked. */
    fun missing(first: Int, last: Int): IntRange? {
        if (total == 0) return null
        val lo = maxOf(0, first)
        val hi = if (total > 0) minOf(last, total - 1) else last
        var page = lo / pageSize
        while (page * pageSize <= hi) {
            val start = page * pageSize
            val end = if (total > 0) minOf(start + pageSize, total) - 1 else start + pageSize - 1
            val complete = (start..end).all { rows.containsKey(it) }
            if (!complete && page !in asked) {
                asked.add(page)
                return start..end
            }
            page++
        }
        return null
    }

    /** {"list", "offset", "total", "rows":[...]} */
    fun accept(msg: Map<String, Any?>) {
        val offset = msg.int("offset")
        total = msg.int("total", total)
        msg["rows"].list().forEachIndexed { i, r -> rows[offset + i] = r.obj() }
        asked.remove(offset / pageSize)
        if (total >= 0) rows.keys.removeAll { it >= total }
    }

    fun invalidate() {
        rows.clear()
        asked.clear()
        total = -1
    }
}

/** Percent-encoding of `fe` arguments (touch_frontend.c fe_decode): one console token each. */
object FeArgs {
    private const val SAFE = "-_.:@+,="

    /** Letters, digits and -_.:@+,= stay; every other byte is %XX ("//" would start a console comment, ';' a new command, '$' a cvar). */
    fun encode(s: String): String {
        if (s.isEmpty()) return "~"
        val b = StringBuilder()
        for (byte in s.toByteArray(Charsets.UTF_8)) {
            val c = byte.toInt() and 0xff
            val ch = c.toChar()
            if (c < 0x80 && (ch.isLetterOrDigit() || SAFE.indexOf(ch) >= 0)) b.append(ch)
            else b.append('%').append(String.format("%02X", c))
        }
        return b.toString()
    }

    fun command(verb: String, vararg args: String) =
        "fe $verb" + args.joinToString("") { " " + encode(it) }
}
