// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core

import xt.core.net.ServerAddress
import kotlin.test.*

class SettingsSessionTest {
    // What Cvar_WriteVariables writes: engine cvars as "name" "value", QC/console ones as seta.
    private val engineConfig = """
        bind w "+forward"
        "vid_fullscreen" "0"
        "volume" "0.5"
        seta "_cl_name" "^1Rat \"the\" \${'$'}node"
        seta "_cl_color" "52"
        "cl_touch_esp_enabled" "off"
        seta cl_maxfps 120 // comment
        alias foo "echo hi"
    """.trimIndent()

    private fun store(cfg: String = engineConfig, over: String = "") =
        SettingsStore(SettingsSchema.all, CvarFile.parse(cfg), Overrides.parse(over), LauncherConfig())

    @Test
    fun parsesWhatTheEngineWrites() {
        val c = CvarFile.parse(engineConfig)
        assertEquals("0", c["vid_fullscreen"])
        assertEquals("0.5", c["VOLUME"])
        assertEquals("^1Rat \"the\" \$node", c["_cl_name"])
        assertEquals("120", c["cl_maxfps"])
        assertNull(c["w"])
        assertNull(c["foo"])
        assertEquals(listOf("seta", "a b", "c"), CvarFile.tokenize("seta \"a b\" c; echo x"))
        assertEquals("\"a\\\"b\\\\c\\\$d\"", CvarFile.quote("a\"b\\c\$d"))
    }

    @Test
    fun valuesComeFromConfigThenOverridesThenDefaults() {
        val s = store()
        assertEquals("0", s.value("vid_fullscreen"))
        assertEquals("0", s.value("vid_vsync"), "not in config.cfg: the default")
        assertFalse(s.bool("cl_touch_esp_enabled"))
        assertTrue(s.bool("cl_touch_aimassist"), "default on")
        assertEquals(3 to 4, SettingsSchema.colorOf(s.value("_cl_color")))
        assertEquals(52, SettingsSchema.clColor(3, 4))
    }

    @Test
    fun changesBecomeOverridesAndSameValuesDropThem() {
        val s = store()
        assertTrue(s.set("vid_fullscreen", "1"))
        assertTrue(s.pending("vid_fullscreen"))
        assertEquals("1", s.value("vid_fullscreen"))
        assertTrue(s.set("vid_fullscreen", "0"))
        assertFalse(s.pending("vid_fullscreen"), "back to config.cfg's value: nothing pending")
        assertTrue(s.set("volume", "0.75"))
        assertTrue(s.set("cl_maxfps", "9999"))
        assertEquals("360", s.value("cl_maxfps"), "clamped to the schema range")
        assertTrue(s.set("cl_touch_esp_enabled", "1"))
        assertEquals("local", s.value("cl_touch_esp_enabled"), "string-valued switch")
        assertTrue(s.set("_cl_name", "New \"name\""))
        assertFalse(s.set("_cl_name", "two\nlines"))
        val text = s.overrides.serialize()
        assertTrue("seta \"volume\" \"0.75\"" in text, text)
        assertTrue("seta \"_cl_name\" \"New \\\"name\\\"\"" in text, text)
        // round trip
        val back = Overrides.parse(text)
        assertEquals(s.overrides.cvars, back.cvars)
    }

    @Test
    fun afterAGameAbsorbedOverridesAreDroppedButLaterOnesStay() {
        val s = store()
        s.set("volume", "0.9")
        s.command("exec effects-high.cfg", "exec effects-")
        val atLaunch = s.snapshot()
        // While the game runs the player changes another setting in the app.
        s.set("fov", "110")
        // The game quit: config.cfg was rewritten with the override absorbed, and the
        // player changed vid_fullscreen in the game's own menu.
        val newCfg = CvarFile.parse("\"volume\" \"0.9\"\n\"vid_fullscreen\" \"1\"\n")
        assertTrue(s.afterGame(newCfg, configChanged = true, atLaunch = atLaunch))
        assertFalse(s.pending("volume"))
        assertTrue(s.overrides.commands.isEmpty())
        assertTrue(s.pending("fov"), "changed after the launch: still pending")
        assertEquals("1", s.value("vid_fullscreen"), "the in-game change is what the app shows")

        // A crash: config.cfg untouched, the overrides stay for the next start.
        val t = store()
        t.set("volume", "0.2")
        assertFalse(t.afterGame(CvarFile.parse(engineConfig), configChanged = false))
        assertTrue(t.pending("volume"))
    }

    @Test
    fun launcherSettingsGoToLauncherConf() {
        val s = store()
        assertTrue(s.set(Keys.RESOLUTION, "1920x1080"))
        assertEquals("1920x1080", s.launcher[Keys.RESOLUTION])
        assertTrue(s.overrides.isEmpty)
    }

    private val start = "/app/bin/start.sh"

    @Test
    fun connectWithPasswordKeepsItOutOfArgvAndEnv() {
        val a = GameAction.Connect(ServerAddress("192.168.1.23", 26001), password = "hunter2 secret")
        val p = Session.plan(a, LauncherConfig(), start, "/tmp/r", withOverrides = true)
        assertEquals(start, p.program)
        assertEquals("native", p.env[Session.ENV_MODE])
        assertEquals("/tmp/r", p.env[Session.ENV_RESULT])
        assertEquals("3", p.env[Session.ENV_SECRET_FD])
        assertTrue(p.args.containsAll(listOf("+connect", "192.168.1.23:26001")))
        assertEquals(listOf("+exec", "godnotic-frontend.cfg"), p.args.takeLast(2), "overrides last")
        assertEquals("join\t192.168.1.23:26001\thunter2 secret\n", p.secret)
        for (x in p.args + p.env.values + p.env.keys) assertFalse("hunter2" in x, "password leaked into $x")
    }

    @Test
    fun hostUsesTheHostScreensCommands() {
        val h = HostSettings(name = "My \"game\"; quit", gametype = "ctf", map = "afterslime", bots = 7, maxPlayers = 4, port = 26001, password = "pw", assist = 5)
        val p = Session.plan(GameAction.Host(h), LauncherConfig(), start, "/r", withOverrides = false)
        val a = p.args
        fun after(k: String) = a[a.indexOf(k) + 1]
        assertEquals("9", after("+maxplayers"), "bots + 2")
        assertEquals("26001", after("+port"))
        assertEquals("0", after("+sv_public"))
        assertEquals("1", after("+sv_status_privacy"))
        assertEquals("My 'game'; quit", after("+hostname"), "no quote can end the string (args are not re-parsed as commands)")
        assertEquals("7", after("+bot_number"))
        assertEquals("1", after("+g_ctf"))
        assertEquals("0", after("+g_dm"))
        assertTrue(a.indexOf("+touch_lan_host_password_apply") < a.indexOf("+map"))
        assertEquals("afterslime", a.last())
        assertTrue(a.containsAll(listOf("+set", "sv_touch_assist", "5")))
        assertEquals("host\tpw\n", p.secret)
        assertFalse(a.any { "pw" == it })
        assertFalse("+exec" in a)
    }

    @Test
    fun botMatchCampaignAndMenu() {
        val m = MatchSettings("stormkeep", "tdm", bots = 5, skill = 8, timelimit = 10, fraglimit = 30, mutators = setOf("g_instagib"))
        val a = Session.actionArgs(GameAction.BotMatch(m))
        fun after(k: String) = a[a.indexOf(k) + 1]
        assertEquals("6", after("+maxplayers"))
        assertEquals("8", after("+skill"))
        assertEquals("10", after("+timelimit_override"))
        assertEquals("1", a[a.indexOf("g_instagib") + 1])
        assertEquals("0", a[a.indexOf("g_nix") + 1], "unchosen mutators are switched off explicitly")
        assertEquals("1", after("+g_tdm"))
        assertEquals(listOf("+map", "stormkeep"), a.takeLast(2))
        assertNull(Session.secret(GameAction.BotMatch(m)))
        assertEquals(m, MatchSettings.parse(m.serialize()))
        assertNull(MatchSettings.parse("bad"))

        val c = Session.actionArgs(GameAction.Campaign("xonoticbeta", 3, "stormkeep"))
        assertEquals(listOf("+set", "g_campaign", "1", "+set", "_campaign_name", "xonoticbeta", "+set", "_campaign_index", "3", "+maxplayers", "16", "+map", "stormkeep"), c)

        val menu = Session.plan(GameAction.Menu, LauncherConfig(), start, "/r", withOverrides = true)
        assertNull(menu.env[Session.ENV_MODE], "the game's own menu is the normal (not native) mode")
    }

    @Test
    fun launcherChoicesStillApply() {
        val cfg = LauncherConfig()
        cfg[Keys.WINDOW_MODE] = "windowed"
        cfg[Keys.TOUCH_PROFILE] = "casual"
        val p = Session.plan(GameAction.Connect(ServerAddress("1.2.3.4")), cfg, start, "/r", false)
        assertEquals("casual", p.env["XONOTIC_TOUCH_PROFILE"])
        assertTrue(p.args.indexOf("+vid_fullscreen") < p.args.indexOf("+connect"))
        assertNull(p.secret)
        assertNull(p.env[Session.ENV_SECRET_FD])
    }

    @Test
    fun sessionResults() {
        val k = SessionResult.parse("kind=kicked\nreason=Disconnect: You were kicked: AFK\nserver=1.2.3.4:26000\n", 0, false)
        assertEquals("kicked", k.kind)
        assertEquals("Disconnected: You were kicked: AFK", k.message())
        assertEquals("1.2.3.4:26000", k.server)
        assertTrue(SessionResult.parse("kind=password_wrong\n", 0, false).wantsPassword)
        assertEquals("Wrong password. Try again", SessionResult.parse("kind=password_wrong\n", 0, false).message())
        assertNull(SessionResult.parse("kind=left\n", 0, false).message())
        val crash = SessionResult.parse(null, 139, true, "/x/log.txt")
        assertEquals("crashed", crash.kind)
        assertTrue(crash.message()!!.contains("/x/log.txt"))
        assertEquals("quit", SessionResult.parse(null, 0, false).kind)
        assertEquals("crashed", SessionResult.parse("kind=quit\n", 134, true).kind)
        val cf = SessionResult.parse("kind=connect_failed\nreason=Connect: [pw-required] This game needs a password.\n", 0, false)
        assertEquals("Could not join: This game needs a password.", cf.message())
    }

    @Test
    fun campaignAndMapinfoFiles() {
        val camp = """
            // comment
            dm,stormkeep,3,1,20,10,"","Welcome","Learn the ropes, then win"
            ctf,afterslime,5,2,10,15,"g_instagib","Flags",Capture them
        """.trimIndent()
        val lv = CampaignFile.parse(camp)
        assertEquals(2, lv.size)
        assertEquals("stormkeep", lv[0].map)
        assertEquals("Welcome", lv[0].shortDesc)
        assertEquals("Learn the ropes, then win", lv[0].longDesc)
        assertEquals("ctf", lv[1].gametype)

        val mi = MapInfoFile.parse("stormkeep", "title Storm Keep\nauthor Someone\ndescription x\ngametype dm // def\ngametype tdm\ngametype ctf 30\ncdtrack 5\n")
        assertEquals("Storm Keep", mi.title)
        assertEquals("Someone", mi.author)
        assertEquals(setOf("dm", "tdm", "ctf"), mi.gametypes)
        val e = MapEntry("stormkeep", "p.pk3", true, mi)
        assertTrue(e.supports("ctf"))
        assertFalse(e.supports("race"))
        assertEquals("Storm Keep", e.title)
        assertTrue(MapEntry("x", "p", false).supports("race"), "unknown mapinfo: allowed")

        val cat = MapCatalog.fromEntries("a.pk3", listOf("maps/stormkeep.bsp", "maps/stormkeep.jpg", "maps/dance.bsp", "maps/sub/x.bsp"))
        assertEquals(listOf("stormkeep" to true, "dance" to false), cat.map { it.name to it.hasPreview })
        val merged = MapCatalog.merge(listOf(cat, listOf(MapEntry("Dance", "b.pk3", true))))
        assertEquals(listOf("Dance", "stormkeep"), merged.map { it.name })
    }
}
