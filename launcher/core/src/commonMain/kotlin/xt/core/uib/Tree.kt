// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core.uib

/**
 * The UI bridge's screen description, as the renderer sees it: a tree of
 * typed nodes with string ids and free-form properties ("hints, not pixels").
 * The engine's QuakeC menu builds it (docs/UI_BRIDGE.md on work/uibridge);
 * the loader renders it with native widgets.
 *
 * Wire shape this accepts (adapted in one place, [Node.from], so the final
 * schema is a small change): an object with "type", "id", optional
 * "children" (array of nodes) and any other keys as properties; a nested
 * "props" object is merged into them.
 */
class Node(
    val type: String,
    val id: String,
    val props: MutableMap<String, Any?>,
    val children: MutableList<Node>,
) {
    var parent: Node? = null
        internal set

    fun str(k: String) = props.str(k)
    fun int(k: String) = props.int(k)
    fun num(k: String) = props.num(k)
    fun bool(k: String) = props.bool(k)
    fun list(k: String) = props.list(k)
    fun obj(k: String) = props.obj(k)

    /** The text a node shows: title, then label, then text. */
    val title: String get() = str("title") ?: str("label") ?: str("text") ?: ""

    fun walk(f: (Node) -> Unit) {
        f(this)
        children.forEach { it.walk(f) }
    }

    override fun toString() = "Node($type#$id)"

    companion object {
        private val reserved = setOf("type", "id", "children", "props")

        /** Parses a node; ids missing on the wire get stable path ids ("home/2/0"). */
        fun from(m: Map<String, Any?>, path: String = "root"): Node {
            val props = LinkedHashMap<String, Any?>()
            for ((k, v) in m) if (k !in reserved) props[k] = v
            m.obj("props")?.let { props.putAll(it) }
            val id = m.str("id")?.takeIf { it.isNotEmpty() } ?: path
            val kids = ArrayList<Node>()
            m.list("children")?.forEachIndexed { i, c ->
                @Suppress("UNCHECKED_CAST")
                (c as? Map<String, Any?>)?.let { kids += from(it, "$id/$i") }
            }
            val n = Node(m.str("type")?.lowercase() ?: "unknown", id, props, kids)
            kids.forEach { it.parent = n }
            return n
        }
    }
}

/**
 * Holds the screens the engine described and applies its updates by id, so a
 * change touches only the widgets it concerns. The renderer listens.
 */
class Store {
    interface Listener {
        /** A whole screen arrived (new or replacing one with the same id). */
        fun screen(root: Node) {}
        /** Properties changed in place. */
        fun patched(node: Node, keys: Set<String>) {}
        /** A subtree was replaced, inserted or removed under [parent]. */
        fun structure(parent: Node) {}
        fun navigate(screenId: String, push: Boolean) {}
        fun back() {}
        fun toast(text: String, timeoutS: Int) {}
        fun dialog(node: Node) {}
        fun closeDialog(id: String) {}
        fun rows(listId: String, window: ListWindow) {}
        /** The engine says the game is starting (hide the loader) or the match ended (show it). */
        fun session(state: String, info: Map<String, Any?>) {}
    }

    val screens = LinkedHashMap<String, Node>()
    private val index = HashMap<String, Node>()
    val lists = HashMap<String, ListWindow>()
    var listener: Listener = object : Listener {}

    fun node(id: String): Node? = index[id]

    private fun indexTree(n: Node) = n.walk { index[it.id] = it }
    private fun unindex(n: Node) = n.walk { index.remove(it.id) }

    /** Applies one message; false when it is not understood (ignored, never fatal). */
    fun apply(msg: Map<String, Any?>): Boolean {
        when (msg.str("op") ?: msg.str("type")) {
            "screen", "snapshot" -> {
                @Suppress("UNCHECKED_CAST")
                val m = (msg["screen"] ?: msg["node"] ?: msg["root"]) as? Map<String, Any?> ?: return false
                val root = Node.from(m)
                screens[root.id]?.let { unindex(it) }
                screens[root.id] = root
                indexTree(root)
                listener.screen(root)
            }
            "patch", "set" -> {
                val n = msg.str("id")?.let { index[it] } ?: return false
                val set = msg.obj("set") ?: msg.obj("props") ?: return false
                n.props.putAll(set)
                listener.patched(n, set.keys)
            }
            "replace" -> {
                val old = msg.str("id")?.let { index[it] } ?: return false
                @Suppress("UNCHECKED_CAST")
                val m = msg["node"] as? Map<String, Any?> ?: return false
                val fresh = Node.from(m, old.id)
                val p = old.parent
                unindex(old)
                if (p == null) {
                    screens[fresh.id] = fresh
                    indexTree(fresh)
                    listener.screen(fresh)
                } else {
                    val i = p.children.indexOf(old)
                    p.children[i] = fresh
                    fresh.parent = p
                    indexTree(fresh)
                    listener.structure(p)
                }
            }
            "insert" -> {
                val p = msg.str("parent")?.let { index[it] } ?: return false
                @Suppress("UNCHECKED_CAST")
                val m = msg["node"] as? Map<String, Any?> ?: return false
                val at = (msg.int("index") ?: p.children.size).coerceIn(0, p.children.size)
                val n = Node.from(m, "${p.id}/$at")
                n.parent = p
                p.children.add(at, n)
                indexTree(n)
                listener.structure(p)
            }
            "remove" -> {
                val n = msg.str("id")?.let { index[it] } ?: return false
                val p = n.parent ?: return false
                p.children.remove(n)
                unindex(n)
                listener.structure(p)
            }
            "push", "navigate" -> listener.navigate(msg.str("screen") ?: msg.str("id") ?: return false, msg.bool("push") ?: true)
            "pop", "back" -> listener.back()
            "toast" -> listener.toast(msg.str("text") ?: return false, msg.int("timeout") ?: 3)
            "dialog" -> {
                @Suppress("UNCHECKED_CAST")
                val m = (msg["dialog"] ?: msg["node"]) as? Map<String, Any?> ?: return false
                val d = Node.from(m, "dialog")
                indexTree(d)
                listener.dialog(d)
            }
            "close_dialog", "close" -> listener.closeDialog(msg.str("id") ?: "dialog")
            "rows" -> {
                val id = msg.str("list") ?: msg.str("id") ?: return false
                val w = lists.getOrPut(id) { ListWindow() }
                @Suppress("UNCHECKED_CAST")
                val rows = msg.list("rows")?.mapNotNull { it as? Map<String, Any?> } ?: emptyList()
                w.put(msg.int("offset") ?: 0, rows, msg.int("total"))
                listener.rows(id, w)
            }
            "session" -> listener.session(msg.str("state") ?: return false, msg)
            else -> return false
        }
        return true
    }
}

/**
 * A long list (the server browser) the engine sends in windows: the renderer
 * asks for the rows around what is visible, keeps a bounded number of blocks
 * and drops the farthest when it has too many.
 */
class ListWindow(val block: Int = 50, val maxBlocks: Int = 40) {
    var total = 0
        private set
    private val blocks = LinkedHashMap<Int, Array<Map<String, Any?>?>>() // access order kept by re-insert
    private val inFlight = HashSet<Int>()
    /** Bumped on every change; the renderer compares it to refresh. */
    var version = 0
        private set

    fun row(i: Int): Map<String, Any?>? {
        if (i < 0 || i >= total) return null
        val b = blocks[i / block] ?: return null
        return b[i % block]
    }

    val loadedBlocks: Set<Int> get() = blocks.keys

    /** Stores rows from [offset]; a [total] resizes the list (rows past it are dropped). */
    fun put(offset: Int, rows: List<Map<String, Any?>>, total: Int?) {
        if (total != null && total != this.total) {
            this.total = total.coerceAtLeast(0)
            val last = (this.total - 1) / block
            blocks.keys.filter { it > last }.forEach { blocks.remove(it) }
        }
        rows.forEachIndexed { k, r ->
            val i = offset + k
            if (i < 0) return@forEachIndexed
            if (i >= this.total) this.total = i + 1
            val bi = i / block
            val b = blocks.remove(bi) ?: arrayOfNulls(block)
            b[i % block] = r
            blocks[bi] = b
            inFlight.remove(bi)
        }
        while (blocks.size > maxBlocks) blocks.remove(blocks.keys.first())
        version++
    }

    /**
     * The (offset, count) requests needed to show rows [first]..[last], with a
     * block of margin either side; each block is asked once until it arrives.
     */
    fun need(first: Int, last: Int): List<Pair<Int, Int>> {
        if (total == 0) {
            if (0 in inFlight || blocks.containsKey(0)) return emptyList()
            inFlight += 0
            return listOf(0 to block)
        }
        val lo = ((first / block) - 1).coerceAtLeast(0)
        val hi = ((last.coerceAtMost(total - 1)) / block + 1).coerceAtMost(((total - 1) / block).coerceAtLeast(0))
        val out = mutableListOf<Pair<Int, Int>>()
        for (b in lo..hi) {
            if (b in blocks || b in inFlight) {
                blocks[b]?.let { blocks.remove(b); blocks[b] = it } // touch: most recently used
                continue
            }
            inFlight += b
            out += (b * block) to block
        }
        return out
    }

    /** Forget everything (the engine started a new listing). */
    fun reset() {
        blocks.clear()
        inFlight.clear()
        total = 0
        version++
    }
}

/** Messages the loader sends: the handshake, events, list requests. */
object Outgoing {
    const val PROTOCOL = 1

    /** The node types this renderer draws natively; anything else becomes a generic row. */
    val NODE_TYPES = listOf(
        "screen", "section", "group", "grid", "cards", "card", "hero", "list", "table", "preview",
        "row", "link", "switch", "slider", "spin", "choice", "text", "entry", "password", "button",
        "label", "image", "badge", "status", "progress", "toolbar", "tabs", "tab", "dialog", "spacer",
    )

    fun hello(app: String, version: String, platform: String) = Json.write(linkedMapOf(
        "op" to "hello", "protocol" to PROTOCOL, "app" to app, "version" to version, "platform" to platform,
        "nodes" to NODE_TYPES, "features" to listOf("diff", "rows", "images", "dialogs", "toasts", "motion"),
    ))

    fun event(id: String, kind: String, value: Any? = null) = Json.write(linkedMapOf("op" to "event", "id" to id, "kind" to kind, "value" to value))

    fun rows(list: String, offset: Int, count: Int) = Json.write(linkedMapOf("op" to "rows?", "list" to list, "offset" to offset, "count" to count))

    fun image(ref: String, width: Int) = Json.write(linkedMapOf("op" to "image?", "ref" to ref, "width" to width))
}

/** Splits a byte stream into newline-delimited JSON messages (the Linux transport's framing). */
class LineFramer(private val maxLine: Int = 4 shl 20) {
    private val buf = StringBuilder()
    var dropped = 0
        private set

    /** Feeds decoded text; returns the complete messages it ended. */
    fun feed(text: String): List<Map<String, Any?>> {
        val out = mutableListOf<Map<String, Any?>>()
        for (c in text) {
            if (c == '\n') {
                val line = buf.toString().trim()
                buf.clear()
                if (line.isEmpty()) continue
                val m = Json.obj(line)
                if (m != null) out += m else dropped++
            } else if (buf.length < maxLine) {
                buf.append(c)
            }
        }
        return out
    }
}
