// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core

import xt.core.net.JoinLink
import xt.core.net.ServerAddress
import xt.core.uib.*
import kotlin.test.*

class UibTest {
    @Test
    fun jsonRoundTrip() {
        val text = """{"a":1,"b":[true,false,null,"x\n\"y\"\u00e9"],"c":{"d":-2.5e1,"e":""},"big":12345678901}"""
        @Suppress("UNCHECKED_CAST")
        val v = Json.parse(text) as Map<String, Any?>
        assertEquals(1L, v["a"])
        assertEquals(listOf(true, false, null, "x\n\"y\"é"), v["b"])
        assertEquals(-25.0, v.obj("c")!!.num("d"))
        assertEquals(12345678901L, v["big"])
        assertEquals(Json.write(v), Json.write(Json.parse(Json.write(v))))
        assertNull(Json.obj("[1]"))
        assertNull(Json.obj("{\"a\":}"))
        assertFailsWith<Json.Error> { Json.parse("{} x") }
        assertFailsWith<Json.Error> { Json.parse("[".repeat(100) + "]".repeat(100)) }
        assertEquals("{\"k\":\"\\u0001\",\"n\":3}", Json.write(mapOf("k" to "\u0001", "n" to 3.0)))
    }

    private fun msg(s: String) = Json.obj(s)!!

    private val home = """{"op":"screen","screen":{"type":"screen","id":"home","title":"Godnotic","children":[
        {"type":"hero","id":"play","title":"Play","image":"img:map/stormkeep","props":{"subtitle":"Quick match"}},
        {"type":"section","title":"More","children":[{"type":"row","title":"Settings"},{"type":"switch","id":"vsync","value":true}]}
    ]}}"""

    private class Rec : Store.Listener {
        val log = mutableListOf<String>()
        override fun screen(root: Node) { log += "screen ${root.id}" }
        override fun patched(node: Node, keys: Set<String>) { log += "patch ${node.id} ${keys.sorted()}" }
        override fun structure(parent: Node) { log += "structure ${parent.id}" }
        override fun toast(text: String, timeoutS: Int) { log += "toast $text" }
        override fun rows(listId: String, window: ListWindow) { log += "rows $listId ${window.total}" }
        override fun navigate(screenId: String, push: Boolean) { log += "nav $screenId" }
        override fun session(state: String, info: Map<String, Any?>) { log += "session $state" }
    }

    @Test
    fun storeAppliesSnapshotsAndDiffsById() {
        val s = Store()
        val r = Rec()
        s.listener = r
        assertTrue(s.apply(msg(home)))
        val root = s.screens["home"]!!
        assertEquals(2, root.children.size)
        assertEquals("Quick match", s.node("play")!!.str("subtitle"), "nested props are merged")
        assertEquals("home/1", root.children[1].id, "missing ids become stable paths")
        assertEquals("home/1/0", root.children[1].children[0].id)
        assertSame(root.children[1], s.node("vsync")!!.parent)

        assertTrue(s.apply(msg("""{"op":"patch","id":"vsync","set":{"value":false}}""")))
        assertEquals(false, s.node("vsync")!!.bool("value"))
        assertTrue(s.apply(msg("""{"op":"insert","parent":"home/1","index":0,"node":{"type":"button","id":"b","title":"Go"}}""")))
        assertEquals("b", s.node("home/1")!!.children[0].id)
        assertTrue(s.apply(msg("""{"op":"replace","id":"play","node":{"type":"hero","id":"play","title":"Continue"}}""")))
        assertEquals("Continue", s.node("play")!!.title)
        assertSame(root, s.node("play")!!.parent)
        assertTrue(s.apply(msg("""{"op":"remove","id":"b"}""")))
        assertNull(s.node("b"))
        assertTrue(s.apply(msg("""{"op":"toast","text":"Saved"}""")))
        assertTrue(s.apply(msg("""{"op":"session","state":"game"}""")))
        assertFalse(s.apply(msg("""{"op":"patch","id":"nope","set":{}}""")), "unknown ids are ignored")
        assertFalse(s.apply(msg("""{"op":"future-thing"}""")), "unknown ops are ignored")
        assertEquals(listOf("screen home", "patch vsync [value]", "structure home/1", "structure home", "structure home/1", "toast Saved", "session game"), r.log)
    }

    @Test
    fun listWindowsAskOncePerBlockAndStayBounded() {
        val w = ListWindow(block = 10, maxBlocks = 3)
        assertEquals(listOf(0 to 10), w.need(0, 5))
        assertEquals(emptyList(), w.need(0, 5), "in flight: not asked twice")
        w.put(0, List(10) { mapOf("name" to "s$it") }, total = 95)
        assertEquals(95, w.total)
        assertEquals("s3", w.row(3)!!.str("name"))
        assertNull(w.row(50))
        // Rows 40-49 visible: blocks 3, 4, 5 (one of margin each side)
        assertEquals(listOf(30 to 10, 40 to 10, 50 to 10), w.need(40, 49))
        w.put(30, List(10) { mapOf("i" to 30 + it) }, null)
        w.put(40, List(10) { mapOf("i" to 40 + it) }, null)
        w.put(50, List(10) { mapOf("i" to 50 + it) }, null)
        assertEquals(3, w.loadedBlocks.size, "bounded: block 0 was dropped")
        assertNull(w.row(3))
        assertEquals(45, w.row(45)!!.int("i"))
        // The last block never asks past the end.
        assertEquals(listOf(80 to 10, 90 to 10), w.need(90, 200))
        w.put(0, emptyList(), total = 20)
        assertTrue(w.loadedBlocks.all { it <= 1 })
        w.reset()
        assertEquals(0, w.total)
    }

    @Test
    fun framerSplitsLinesAndSkipsJunk() {
        val f = LineFramer()
        assertTrue(f.feed("{\"op\":\"toast\",").isEmpty())
        val a = f.feed("\"text\":\"a\"}\n\nnot json\n{\"op\":\"pop\"}\n{\"op\"")
        assertEquals(listOf("toast", "pop"), a.map { it.str("op") })
        assertEquals(1, f.dropped)
        assertEquals(listOf("x"), f.feed(":\"x\"}\n").map { it.str("op") })
        val hello = Json.obj(Outgoing.hello("xt-launcher", "1", "linux"))!!
        assertEquals("hello", hello.str("op"))
        assertTrue("card" in hello.list("nodes")!!)
        assertEquals("""{"op":"event","id":"vsync","kind":"change","value":true}""", Outgoing.event("vsync", "change", true))
        assertEquals("""{"op":"rows?","list":"servers","offset":50,"count":50}""", Outgoing.rows("servers", 50, 50))
    }

    @Test
    fun joinLinksAndAddresses() {
        assertEquals(ServerAddress("192.168.1.23", 26000), ServerAddress.parse("192.168.1.23"))
        assertEquals(ServerAddress("fe80::1", 26000), ServerAddress.parse("[fe80::1]:26000"))
        assertNull(ServerAddress.parse("1.2.3.4; quit"))
        assertNull(ServerAddress.parse("1.2.3.4:70000"))
        assertEquals("godnotic://join/100.101.1.2:26000", JoinLink.of(ServerAddress("100.101.1.2")))
        assertEquals(ServerAddress("100.101.1.2", 26001), JoinLink.parse("godnotic://join/100.101.1.2:26001/"))
        assertNull(JoinLink.parse("godnotic://other/1.2.3.4"))
        assertNull(JoinLink.parse("godnotic://join/1.2.3.4\$x"))
    }

    @Test
    fun sessionPlanKeepsThePasswordOutOfArgvAndEnv() {
        val a = GameAction.Connect(ServerAddress("192.168.1.23", 26001), "hunter2 x")
        val p = Session.plan(a, LauncherConfig(), "/app/bin/start.sh", "/tmp/r")
        assertEquals("native", p.env[Session.ENV_MODE])
        assertEquals("3", p.env[Session.ENV_SECRET_FD])
        assertEquals(listOf("+connect", "192.168.1.23:26001"), p.args.takeLast(2))
        assertEquals("join\t192.168.1.23:26001\thunter2 x\n", p.secret)
        for (x in p.args + p.env.values) assertFalse("hunter2" in x, x)
        val play = Session.plan(GameAction.Play, LauncherConfig(), "/s", "/r")
        assertNull(play.env[Session.ENV_MODE])
        assertNull(play.secret)
    }

    @Test
    fun sessionResults() {
        val k = SessionResult.parse("kind=kicked\nreason=Disconnect: You were kicked: AFK\nserver=1.2.3.4:26000\n", 0, false)
        assertEquals("Disconnected: You were kicked: AFK", k.message())
        assertTrue(SessionResult.parse("kind=password_wrong\n", 0, false).wantsPassword)
        assertNull(SessionResult.parse("kind=left\n", 0, false).message())
        assertEquals("crashed", SessionResult.parse(null, 139, true, "/x/log").kind)
        assertEquals("quit", SessionResult.parse(null, 0, false).kind)
        assertEquals("error", SessionResult.parse("kind=quit\n", 1, false).kind)
        assertEquals("Could not join: This game needs a password.",
            SessionResult.parse("kind=connect_failed\nreason=Connect: rejected, [pw-required] This game needs a password.\n", 0, false).message())
    }
}
