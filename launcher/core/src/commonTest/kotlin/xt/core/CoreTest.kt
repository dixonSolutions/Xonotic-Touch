// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

class ConfigTest {
    @Test fun roundTrip() {
        val c = LauncherConfig.parse("# x\n touch_profile = left \nbad line\nextra_args = +name \"a b\"\n")
        assertEquals("left", c[Keys.TOUCH_PROFILE])
        assertEquals("+name \"a b\"", c[Keys.EXTRA_ARGS])
        c[Keys.HOST_BOTS] = 6
        c[Keys.TOUCH_PROFILE] = ""
        val again = LauncherConfig.parse(c.serialize())
        assertNull(again[Keys.TOUCH_PROFILE])
        assertEquals(6, again.int(Keys.HOST_BOTS, 0))
        assertTrue(LauncherConfig.parse("skip_launcher = yes").bool(Keys.SKIP_LAUNCHER, false))
    }
}

class LaunchTest {
    @Test fun splitArgs() {
        assertEquals(listOf("+name", "a b", "-x", "it's", ""), Launch.splitArgs("+name \"a b\"  -x it\\'s ''"))
        assertEquals(emptyList(), Launch.splitArgs("   "))
    }

    @Test fun gameOnlyPassesWhatWasChosen() {
        val plain = Launch.game(LauncherConfig(), "/app/bin/start.sh")
        assertEquals(emptyList(), plain.args)
        assertEquals(emptyMap(), plain.env)

        val cfg = LauncherConfig.parse(
            """
            touch_profile = minimal
            perf_profile = balanced
            window_mode = windowed
            resolution = 1280x720
            aim_assist = off
            auto_shoot = on
            touch_mode = always
            extra_args = +map dance
            """.trimIndent()
        )
        val p = Launch.game(cfg, "/app/bin/start.sh")
        assertEquals("minimal", p.env["GODNOTIC_PROFILE"])
        assertEquals("balanced", p.env["GODNOTIC_PERF_PROFILE"])
        assertEquals("always", p.env["GODNOTIC_MODE"])
        assertEquals(
            listOf("+vid_fullscreen", "0", "+vid_width", "1280", "+vid_height", "720", "+vid_conheight", "720",
                "+cl_touch_aimassist", "0", "+cl_touch_autoshoot", "1", "+map", "dance"),
            p.args,
        )
    }

    @Test fun serverPutsMaxplayersBeforeTheMap() {
        val paths = GamePaths("/app", "/app/bin/start.sh", "/app/bin/xonotic", "/u", "/h/.xonotic", true)
        val cfg = LauncherConfig.parse("host_map = dance\nhost_gametype = ctf\nhost_bots = 99")
        val p = Launch.server(cfg, "/app/bin/xonotic", paths)
        assertTrue("-dedicated" in p.args)
        assertTrue(p.args.indexOf("+maxplayers") < p.args.indexOf("+map"))
        assertEquals("dance", p.args.last())
        assertEquals("32", p.args[p.args.indexOf("+bot_number") + 1])
        assertEquals("1", p.args[p.args.indexOf("+g_ctf") + 1])
        assertEquals("/u", p.cwd)
        assertEquals("/app/lib", p.env["LD_LIBRARY_PATH"])
        assertEquals(Launch.DEFAULT_MAP, Launch.server(LauncherConfig(), "x", paths).args.last())
    }

    @Test fun resolution() {
        assertEquals(1920 to 1080, Launch.parseResolution("1920x1080"))
        assertEquals(1920 to 1080, Launch.parseResolution("1920 × 1080"))
        assertNull(Launch.parseResolution("huge"))
        assertNull(Launch.parseResolution(""))
    }

    @Test fun colours() {
        assertEquals("red plain ^ caret", Console.stripColors("^1red ^xF80plain ^^ caret"))
        assertEquals("s22: LOAD_ENT x", Console.stripColors("s22: \u001b[1;35mLOAD_ENT x\u001b[m"))
    }
}

class PathsTest {
    @Test fun flatpak() {
        val env = mapOf("HOME" to "/h", "FLATPAK_ID" to "io.x", "XDG_DATA_HOME" to "/h/.var/app/io.x/data")
        val files = setOf("/app/bin/start.sh", "/app/bin/xonotic")
        val p = PathResolver.resolve(env, "/app/bin", { it in files })
        assertEquals("/app", p.appRoot)
        assertEquals("/app/bin/start.sh", p.startScript)
        assertEquals("/h/.var/app/io.x/data/godnotic", p.userBase)
        assertEquals("/h/.xonotic/mods", p.modsDir)
        assertEquals(listOf("/h/.xonotic/data", "/h/.var/app/io.x/data/godnotic/data", "/app/data"), p.dataDirs)
    }

    @Test fun localInstallAndOverride() {
        val files = setOf("/opt/xt/bin/start.sh")
        val p = PathResolver.resolve(mapOf("HOME" to "/h"), "/opt/xt/bin", { it in files }, "/games/xt/")
        assertEquals("/opt/xt", p.appRoot)
        assertNull(p.engine)
        assertEquals("/games/xt", p.userBase)
        assertFalse(p.flatpak)
        assertNull(PathResolver.resolve(mapOf("HOME" to "/h"), "/nowhere/bin", { false }).startScript)
    }
}

class ProfilesTest {
    @Test fun parseKinds() {
        val casual = Profiles.parse("casual", "// touch/profiles/casual.cfg — New players: larger targets, relaxed aim.\nexec x\n")
        assertEquals(Profile.Kind.LAYOUT, casual.kind)
        assertEquals("New players: larger targets, relaxed aim", casual.summary)
        val bal = Profiles.parse("balanced", "// touch/profiles/balanced.cfg — Performance only. The default on Android.\n\ntouch_performance_profile \"balanced\"\n")
        assertEquals(Profile.Kind.PERFORMANCE, bal.kind)
        assertEquals("The default on Android", bal.summary)
        assertEquals(Profile.Kind.PART, Profiles.parse("helpers", "").kind)
        assertEquals("Left-handed", Profiles.parse("left", "").title)
        assertEquals(listOf("standard", "casual", "competitive", "minimal", "left"), Profiles.layouts(Profiles.builtin).map { it.id })
    }
}

class ModsTest {
    @Test fun togglesPreserveUnavailableModsAndRespectFailures() {
        var disk = "missing\nfirst\n"
        fun change(name: String, on: Boolean, fail: Boolean = false) = Mods.setEnabled(name, on, { disk }) {
            if (fail) false else { disk = it; true }
        }
        assertTrue(change("second", true))
        assertTrue(change("first", false))
        assertEquals("missing\nsecond\n", disk)
        assertFalse(change("second", false, fail = true))
        assertEquals("missing\nsecond\n", disk)
        assertFalse(Mods.setEnabled("second", false, { null }) { error("must not write after read failure") })
    }

    @Test fun infoAndEnabled() {
        val info = Mods.parseInfo("title: Kill feed\nversion: 1.2\nauthor: Ana\ndescription: Shows: frags\n")
        assertEquals("Shows: frags", info.description)
        val en = Mods.parseEnabled("killfeed\n\n hud \n")
        assertEquals(setOf("killfeed", "hud"), en)
        val e = Mods.entry("killfeed", info, en, true)
        assertTrue(e.enabled)
        assertEquals("v1.2 · Ana — Shows: frags", e.subtitle)
        assertEquals("hud\nkillfeed\n", Mods.serializeEnabled(en))
        assertFalse(Mods.isModName(".hidden"))
        assertEquals("plain", Mods.entry("plain", null, emptySet(), false).title)
    }
}

class ModPermissionsTest {
    @Test fun requestedGrantedAndMissing() {
        val info = Mods.parseInfo("id: hud\napi: 2\npermissions: events, display hooks\nreason.events: x\n")
        assertEquals(listOf("events", "display", "hooks"), Mods.requested(info))
        // v1 grant file: events (bit 0) and display (bit 6)
        val e = Mods.entry("hud", info, emptySet(), true, "v1 41\n", withPermissions = true)
        assertEquals(setOf("events", "display"), e.granted)
        assertEquals(listOf("hooks"), e.missing)
        assertTrue(e.subtitle.contains("1 of 3 permissions not granted"))
        // no file: nothing for API 1+, the HUD set for legacy; malformed: nothing
        assertEquals(emptySet<String>(), Mods.parseGrants(null, scoped = true))
        assertEquals(Mods.LEGACY_DEFAULT, Mods.parseGrants(null, scoped = false))
        assertEquals(emptySet<String>(), Mods.parseGrants("legacy 7ff", scoped = true))
        assertEquals(11, Mods.requested(Mods.parseInfo("title: old\n")).size)
    }

    @Test fun namesAndRequests() {
        assertTrue(Mods.isModName("hud_clock"))
        assertFalse(Mods.isModName("Hud"))
        assertFalse(Mods.isModName("a".repeat(33)))
        assertEquals("plugin /tmp/x.zip\n", Mods.installRequest(true, "/tmp/x.zip"))
        assertEquals<String?>(null, Mods.installRequest(false, "/tmp/x\";quit.zip"))
    }
}

class ZipTest {
    /** A stored-only zip built by hand: local headers are not read, so only the central directory matters. */
    private fun zip(names: List<String>): ByteArray {
        val cd = mutableListOf<Byte>()
        fun le(v: Long, n: Int) = (0 until n).map { ((v shr (8 * it)) and 0xff).toByte() }
        for (n in names) {
            val nb = n.encodeToByteArray()
            cd += le(0x02014b50, 4); cd += List(24) { 0 }; cd += le(nb.size.toLong(), 2)
            cd += le(0, 2); cd += le(0, 2); cd += List(12) { 0 }; cd += nb.toList()
        }
        val pre = List(10) { 7.toByte() }
        val eocd = le(0x06054b50, 4) + le(0, 4) + le(names.size.toLong(), 2) + le(names.size.toLong(), 2) +
            le(cd.size.toLong(), 4) + le(pre.size.toLong(), 4) + le(0, 2)
        return (pre + cd + eocd).toByteArray()
    }

    @Test fun listsMaps() {
        val bytes = zip(listOf("maps/stormkeep.bsp", "maps/stormkeep.mapinfo", "textures/a.tga", "maps/sub/x.bsp", "MAPS/Dance.BSP"))
        val src = object : Zip.Source {
            override val size = bytes.size.toLong()
            override fun read(offset: Long, length: Int) = bytes.copyOfRange(offset.toInt(), offset.toInt() + length)
        }
        val names = Zip.entryNames(src)
        assertEquals(5, names.size)
        assertEquals(listOf("stormkeep", "Dance"), Zip.mapNames(names))
    }
}
