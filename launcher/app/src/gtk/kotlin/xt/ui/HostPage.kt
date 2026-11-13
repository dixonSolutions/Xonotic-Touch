// SPDX-License-Identifier: GPL-3.0-or-later
@file:OptIn(ExperimentalForeignApi::class)

package xt.ui

import gtk.*
import kotlinx.cinterop.*
import xt.core.*

/** The one local server this launcher can run; stopped when the launcher quits. */
private var server: CPointer<GSubprocess>? = null
private var serverIn: CPointer<GOutputStream>? = null

fun stopServer() {
    server?.let { g_subprocess_force_exit(it) }
}

private class HostView(val ctx: Ctx) {
    val cfg = ctx.cfg
    val buffer: CPointer<GtkTextBuffer> = gtk_text_buffer_new(null)!!
    val view: Widget = gtk_text_view_new_with_buffer(buffer)!!
    val endMark: CPointer<GtkTextMark>
    lateinit var startButton: Widget
    lateinit var cmdEntry: Widget
    lateinit var consoleGroup: Widget
    var lines = 0

    init {
        gtk_text_view_set_editable(view.cast(), 0)
        gtk_text_view_set_cursor_visible(view.cast(), 0)
        gtk_text_view_set_wrap_mode(view.cast(), GtkWrapMode.GTK_WRAP_WORD_CHAR)
        gtk_text_view_set_monospace(view.cast(), 1)
        view.css("console")
        endMark = memScoped {
            val it = alloc<GtkTextIter>()
            gtk_text_buffer_get_end_iter(buffer, it.ptr)
            gtk_text_buffer_create_mark(buffer, "end", it.ptr, 0)!!
        }
    }

    fun append(text: String) {
        memScoped {
            val it = alloc<GtkTextIter>()
            gtk_text_buffer_get_end_iter(buffer, it.ptr)
            gtk_text_buffer_insert(buffer, it.ptr, text + "\n", -1)
            lines++
            // Keep the console light: drop the oldest lines past 2000.
            if (lines > 2000) {
                val a = alloc<GtkTextIter>()
                val b = alloc<GtkTextIter>()
                gtk_text_buffer_get_start_iter(buffer, a.ptr)
                gtk_text_buffer_get_iter_at_line(buffer, b.ptr, 200)
                gtk_text_buffer_delete(buffer, a.ptr, b.ptr)
                lines -= 200
            }
            gtk_text_buffer_get_end_iter(buffer, it.ptr)
            gtk_text_buffer_move_mark(buffer, endMark, it.ptr)
        }
        gtk_text_view_scroll_mark_onscreen(view.cast(), endMark)
    }

    fun running() = server != null

    fun refreshButton() {
        gtk_button_set_label(startButton.cast(), if (running()) "Stop server" else "Start server")
        gtk_widget_remove_css_class(startButton, if (running()) "suggested-action" else "destructive-action")
        startButton.css(if (running()) "destructive-action" else "suggested-action")
        cmdEntry.sensitive(running())
    }

    fun toggle() {
        if (running()) {
            send("quit")
            append("> quit")
            return
        }
        val engine = ctx.paths.engine
        if (engine == null) {
            ctx.toast("The game engine was not found. Set its folder in Settings.")
            return
        }
        start(Launch.server(cfg, engine, ctx.paths))
    }

    fun start(plan: LaunchPlan) = memScoped {
        val flags = G_SUBPROCESS_FLAGS_STDOUT_PIPE or G_SUBPROCESS_FLAGS_STDERR_MERGE or G_SUBPROCESS_FLAGS_STDIN_PIPE
        val launcher = g_subprocess_launcher_new(flags)
        plan.cwd?.let { Sys.mkdirs(it); g_subprocess_launcher_set_cwd(launcher, it) }
        for ((k, v) in plan.env) g_subprocess_launcher_setenv(launcher, k, v, 1)
        val err = alloc<CPointerVar<GError>>()
        val argv = allocArrayOf((listOf(plan.program) + plan.args).map { it.cstr.ptr } + listOf(null))
        val proc = g_subprocess_launcher_spawnv(launcher, argv, err.ptr)
        g_object_unref(launcher)
        if (proc == null) {
            val msg = err.value?.pointed?.message?.toKString() ?: "unknown error"
            g_clear_error(err.ptr)
            append("Could not start the server: $msg")
            return@memScoped
        }
        server = proc
        serverIn = g_subprocess_get_stdin_pipe(proc)
        consoleGroup.visible(true)
        append("$ ${plan.program.substringAfterLast('/')} ${plan.args.joinToString(" ")}")
        val stream = g_data_input_stream_new(g_subprocess_get_stdout_pipe(proc))!!
        readLine(stream)
        val (cb, data) = asyncCallback { _, res ->
            val code = if (g_subprocess_get_if_exited(proc) != 0) g_subprocess_get_exit_status(proc) else -1
            g_subprocess_wait_finish(proc, res, null)
            append(if (code >= 0) "Server stopped (exit $code)" else "Server stopped")
            server = null
            serverIn = null
            g_object_unref(proc)
            refreshButton()
        }
        g_subprocess_wait_async(proc, null, cb, data)
        refreshButton()
    }

    fun readLine(stream: CPointer<GDataInputStream>) {
        val (cb, data) = asyncCallback { _, res ->
            val line = memScoped {
                val len = alloc<gsizeVar>()
                val raw = g_data_input_stream_read_line_finish(stream, res, len.ptr, null)
                if (raw == null) null else {
                    // Quake text is not always UTF-8; make it valid before GTK sees it.
                    val valid = g_utf8_make_valid(raw.reinterpret(), len.value.toLong())
                    g_free(raw)
                    val s = valid?.toKString()
                    g_free(valid)
                    s
                }
            }
            if (line == null) {
                g_object_unref(stream)
            } else {
                append(Console.stripColors(line))
                readLine(stream)
            }
        }
        g_data_input_stream_read_line_async(stream, 0, null, cb, data)
    }

    fun send(cmd: String) {
        val out = serverIn ?: return
        val bytes = (cmd + "\n").encodeToByteArray()
        bytes.usePinned { g_output_stream_write_all(out, it.addressOf(0), bytes.size.toULong(), null, null, null) }
    }
}

fun hostPage(ctx: Ctx): Widget {
    val v = HostView(ctx)
    val cfg = ctx.cfg

    // Server settings
    var maps: List<String> = emptyList()
    val mapRow = comboRow("Map", "Looking at your packs…", listOf(cfg.string(Keys.HOST_MAP, Launch.DEFAULT_MAP)), 0) { i ->
        maps.getOrNull(i)?.let { cfg[Keys.HOST_MAP] = it; ctx.save() }
    }
    adw_combo_row_set_enable_search(mapRow.cast(), 1)
    ctx.onData {
        val list = ctx.maps ?: return@onData
        maps = list
        if (list.isEmpty()) {
            mapRow.setSubtitle("No maps found. Download the game data first.")
        } else {
            val want = cfg.string(Keys.HOST_MAP, Launch.DEFAULT_MAP)
            mapRow.setComboItems(list, list.indexOfFirst { it.equals(want, true) }.coerceAtLeast(0))
            mapRow.setSubtitle("${list.size} maps in your packs")
        }
    }
    val types = Choices.gametypes
    val typeRow = comboRow("Game mode", null, types.map { it.title }, Choices.indexOf(types, cfg.string(Keys.HOST_GAMETYPE, "dm"))) { i ->
        cfg[Keys.HOST_GAMETYPE] = types[i].id
        ctx.save()
    }
    val bots = spinRow("Bots", "Computer players that join at once", 0.0, 32.0, cfg.int(Keys.HOST_BOTS, 4).toDouble()) {
        cfg[Keys.HOST_BOTS] = it; ctx.save()
    }
    val maxp = spinRow("Player slots", null, 2.0, 64.0, cfg.int(Keys.HOST_MAXPLAYERS, 8).toDouble()) {
        cfg[Keys.HOST_MAXPLAYERS] = it; ctx.save()
    }
    val port = spinRow("Port", "UDP; others join with +connect <your address>:<port>", 1024.0, 65535.0,
        cfg.int(Keys.HOST_PORT, 26000).toDouble()) { cfg[Keys.HOST_PORT] = it; ctx.save() }
    val name = entryRow("Server name", cfg.string(Keys.HOST_NAME, "Xonotic Touch server")) { cfg[Keys.HOST_NAME] = it.trim(); ctx.save() }
    val public = switchRow("List on the public server list", "Off keeps it to your network", cfg.bool(Keys.HOST_PUBLIC, false)) {
        cfg[Keys.HOST_PUBLIC] = it; ctx.save()
    }
    val settings = group("Server", null, mapRow, typeRow, bots, maxp, name, port, public)

    val plugins = ModList(ctx, ctx.paths.pluginsDir, "Server plugins",
        "Lua plugins this server runs. A new plugin starts off.",
        "No plugins yet. Put a plugin folder with an init.lua here:")

    // Console
    v.startButton = button("Start server", null, "big-action", "suggested-action") { v.toggle() }
    v.cmdEntry = gtk_entry_new()!!
    gtk_entry_set_placeholder_text(v.cmdEntry.cast(), "Server command, e.g. status or kick <name>")
    v.cmdEntry.on("activate") {
        val t = editableText(v.cmdEntry).trim()
        if (t.isNotEmpty()) {
            v.append("> $t")
            v.send(t)
            gtk_editable_set_text(v.cmdEntry.cast(), "")
        }
    }
    v.cmdEntry.expand(h = true).sensitive(false)
    val consoleScroll = gtk_scrolled_window_new()!!
    gtk_scrolled_window_set_child(consoleScroll.cast(), v.view)
    gtk_scrolled_window_set_min_content_height(consoleScroll.cast(), 260)
    consoleScroll.css("console-frame")
    gtk_widget_set_overflow(consoleScroll, GtkOverflow.GTK_OVERFLOW_HIDDEN)
    val consoleGroup = group("Console", "Live output of the local server")
    consoleGroup.add(vbox(10, consoleScroll, v.cmdEntry))
    // Shown once a server runs, right under the button, so a phone sees it without scrolling.
    v.consoleGroup = consoleGroup.visible(server != null)

    v.append("Ready. The server uses the packs in ${ctx.paths.userBase}.")
    if (server != null) v.refreshButton()
    if (startServerNow) {
        startServerNow = false
        v.toggle()
    }

    val actions = hbox(0, v.startButton).align(h = GTK_ALIGN_CENTER)
    val content = vbox(24, actions, consoleGroup, settings, plugins.group).margins(24, 16, 32, 16)
    return scrolled(clamp(content, 820))
}
