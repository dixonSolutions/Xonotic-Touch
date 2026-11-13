// SPDX-License-Identifier: GPL-3.0-or-later
@file:OptIn(ExperimentalForeignApi::class)

package xt.ui

import gtk.*
import kotlinx.cinterop.*
import xt.core.*

/**
 * Lists the folders in [dir] as the engine's Lua mod manager sees them, with
 * their permission state from <userdir>/luastate/<mods|plugins>/grants/.
 * Runs on an I/O thread.
 */
fun scanMods(dir: String): List<ModEntry> {
    val enabled = Sys.readText("$dir/enabled.txt")?.let { Mods.parseEnabled(it) } ?: emptySet()
    val realm = dir.trimEnd('/').substringAfterLast('/')
    val grants = dir.trimEnd('/').substringBeforeLast('/') + "/luastate/$realm/grants"
    return Sys.list(dir).filter { Mods.isModName(it) && Sys.isDir("$dir/$it") }.map { name ->
        val info = Sys.readText("$dir/$name/mod.txt", 16384)?.let { Mods.parseInfo(it) }
        val g = if (Sys.exists("$grants/$name.txt")) Sys.readText("$grants/$name.txt", 256) else null
        Mods.entry(name, info, enabled, Sys.exists("$dir/$name/init.lua"), g, withPermissions = true)
    }
}

/**
 * "Install from a file": the path of a .zip (or a mod folder) goes to the game,
 * which shows what it asks for in its Mods tab before installing anything
 * (docs/PLUGIN_STANDARD.md). The launcher never unpacks or enables a mod itself.
 */
fun installGroup(ctx: Ctx, userDir: String): Widget {
    var path = ""
    var plugin = false
    val g = group("Install from a file",
        "A mod's .zip (or folder). The game checks it and shows what it asks for; it installs switched off, with no permissions.")
    g.add(entryRow("Path to the .zip", "") { path = it.trim() })
    g.add(switchRow("It is a server plugin", "Off: a client mod", false) { plugin = it })
    val go = actionRow("Review it in the game", "Start the game: Settings, Mobile, Mods shows the review", "system-software-install-symbolic")
    go.suffix(button("Send", null, "suggested-action") {
        val text = Mods.installRequest(plugin, path)
        val dir = "$userDir/luastate/incoming"
        val ok = path.isNotEmpty() && text != null && Sys.exists(path) && run { Sys.mkdirs(dir); Sys.writeText("$dir/request.txt", text) }
        ctx.toast(if (ok) "Start the game to review ${path.substringAfterLast('/')}" else "No such file, or the path has quotes or semicolons")
    })
    g.add(go)
    return g
}

/**
 * A boxed list of mod switches for one folder (client mods or server
 * plugins). Turning one on or off rewrites enabled.txt the way the engine
 * does, so the in-game Mods dialog and lua_mod_enable agree with it.
 */
class ModList(private val ctx: Ctx, private val dir: String, title: String, description: String, private val emptyText: String) {
    val group: Widget = group(title, description)
    private val rows = mutableListOf<Widget>()
    private var generation = 0

    init {
        group.headerSuffix(hbox(6,
            button(null, "folder-open-symbolic", "flat") { Sys.mkdirs(dir); openFolder(ctx.window, dir) }
                .also { gtk_widget_set_tooltip_text(it, "Open the folder") },
            button(null, "view-refresh-symbolic", "flat") { refresh() }
                .also { gtk_widget_set_tooltip_text(it, "Look again") },
        ))
        refresh()
    }

    fun refresh() {
        val request = ++generation
        background({ scanMods(dir) }) { mods -> if (request == generation) show(mods) }
    }

    private fun show(mods: List<ModEntry>) {
        rows.forEach { group.remove(it) }
        rows.clear()
        if (mods.isEmpty()) {
            val r = actionRow(emptyText, dir, "folder-symbolic")
            rows += r
            group.add(r)
            return
        }
        for (m in mods) {
            var reverting = false
            lateinit var r: Widget
            r = switchRow(m.title, m.subtitle.ifEmpty { m.name }, m.enabled) { on ->
                if (!reverting && !toggle(m.name, on)) {
                    reverting = true
                    adw_switch_row_set_active(r.cast(), if (on) 0 else 1)
                    reverting = false
                }
            }
            if (!m.hasInit) r.sensitive(false)
            rows += r
            group.add(r)
        }
    }

    private fun toggle(name: String, on: Boolean): Boolean {
        // Invalidate in-flight scans before writing; their snapshots may be stale.
        generation++
        val path = "$dir/enabled.txt"
        val ok = Mods.setEnabled(name, on, {
            if (Sys.exists(path)) Sys.readText(path) else ""
        }, { Sys.writeText(path, it) })
        ctx.toast(if (!ok) "Could not write $path" else if (on) "$name is on for the next game" else "$name is off")
        return ok
    }
}

fun modsPage(ctx: Ctx): Widget {
    val client = ModList(ctx, ctx.paths.modsDir, "Client mods",
        "Lua mods that run in your game: HUD, sounds, helpers. A new mod starts off.",
        "No mods yet. Put a mod folder with an init.lua here:")
    val intro = label(
        "Mods run in a sandbox: no files, no network, only the cvars and commands the game allows, " +
            "and only the permissions you grant (in the game: Settings, Mobile, Mods). " +
            "Server plugins are on the Host page.", "dim-label", wrap = true, xalign = 0f)
    gtk_label_set_justify(intro.cast(), GtkJustification.GTK_JUSTIFY_LEFT)
    val install = installGroup(ctx, ctx.paths.modsDir.substringBeforeLast('/'))
    val content = vbox(18, intro, client.group, install).margins(24, 16, 32, 16)
    return scrolled(clamp(content, 720))
}
