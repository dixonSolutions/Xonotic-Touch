// SPDX-License-Identifier: GPL-3.0-or-later
@file:OptIn(ExperimentalForeignApi::class)

package xt.ui

import gtk.*
import kotlinx.cinterop.*
import xt.core.*

fun settingsPage(ctx: Ctx): Widget {
    val cfg = ctx.cfg

    // Game data
    val dataRow = actionRow("Game data", "Checking…", "folder-download-symbolic")
    val setUp = button("Set Up", null, "suggested-action") { ctx.play() }.align(v = GTK_ALIGN_CENTER)
    gtk_widget_set_tooltip_text(setUp, "Starts the game, which downloads the data it needs")
    dataRow.suffix(setUp.visible(false))
    val folderRow = actionRow("Data folder", ctx.paths.userBase, "folder-symbolic")
    folderRow.suffix(button(null, "folder-open-symbolic", "flat") { Sys.mkdirs(ctx.paths.userBase); openFolder(ctx.window, ctx.paths.userBase) }
        .align(v = GTK_ALIGN_CENTER))
    val gameRow = actionRow("Game", ctx.paths.startScript ?: "Not found", "applications-games-symbolic")
    ctx.onData {
        val d = ctx.data ?: return@onData
        dataRow.setSubtitle(if (d.ready) "Ready · ${d.summary}" else "Not downloaded yet. Set Up starts the game's downloader (about 3 GB).")
        setUp.visible(!d.ready)
        folderRow.setSubtitle(ctx.paths.userBase)
        gameRow.setSubtitle(ctx.paths.startScript ?: "Not found")
    }
    val baseRow = entryRow("Custom data folder (empty = default)", cfg.string(Keys.USER_BASE)) { t ->
        cfg[Keys.USER_BASE] = t.trim()
        ctx.save()
    }
    adw_entry_row_set_show_apply_button(baseRow.cast(), 1)
    baseRow.on("apply") { ctx.reresolve(); ctx.toast("Data folder: ${ctx.paths.userBase}") }
    val data = group("Game", null, dataRow, gameRow, folderRow, baseRow)

    // Touch and aiming, passed to the game on each start
    fun choiceRow(title: String, subtitle: String, key: String, list: List<Choice>) =
        comboRow(title, subtitle, list.map { it.title }, Choices.indexOf(list, cfg[key])) { i ->
            cfg[key] = list[i].id
            ctx.save()
        }
    val touch = group("Touch and aiming", "Default leaves whatever you chose in the game alone.",
        choiceRow("Touch controls", "When the on-screen controls show", Keys.TOUCH_MODE, Choices.touchModes),
        choiceRow("Aim assist", "Slows and pulls your aim near a target", Keys.AIM_ASSIST, Choices.onOff),
        choiceRow("Auto-shoot", "Fires when your aim is on a target", Keys.AUTO_SHOOT, Choices.onOff),
    )

    // Launcher behaviour
    val skip = switchRow("Open the launcher", "Off: the app icon starts the game at once. The icon’s Launcher action brings this back.",
        !cfg.bool(Keys.SKIP_LAUNCHER, false)) { on ->
        cfg[Keys.SKIP_LAUNCHER] = if (on) "" else "1"
        ctx.save()
    }
    val cfgRow = actionRow("Launcher settings file", ctx.configPath, "document-properties-symbolic")
    val launcher = group("Launcher", null, skip, cfgRow)

    val content = vbox(24, data, touch, launcher).margins(24, 16, 32, 16)
    return scrolled(clamp(content, 720))
}
