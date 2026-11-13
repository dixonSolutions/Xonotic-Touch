// SPDX-License-Identifier: GPL-3.0-or-later
@file:OptIn(ExperimentalForeignApi::class)

package xt.ui

import gtk.*
import kotlinx.cinterop.*

/**
 * A small Kotlin layer over the GTK/libadwaita C API: signals take Kotlin
 * lambdas (kept alive by a StableRef the closure frees), widgets are built
 * with a few helpers. Everything else calls the C functions directly.
 */
typealias Widget = CPointer<GtkWidget>

@Suppress("NOTHING_TO_INLINE")
inline fun <T : CPointed> CPointer<*>.cast(): CPointer<T> = reinterpret()

val CPointer<*>.w: Widget get() = reinterpret()

private val voidTramp = staticCFunction { _: COpaquePointer?, d: COpaquePointer? ->
    d!!.asStableRef<() -> Unit>().get().invoke()
}
private val notifyTramp = staticCFunction { _: COpaquePointer?, _: COpaquePointer?, d: COpaquePointer? ->
    d!!.asStableRef<() -> Unit>().get().invoke()
}
private val freeRef = staticCFunction { d: COpaquePointer?, _: CPointer<GClosure>? ->
    d?.asStableRef<Any>()?.dispose()
    Unit
}

/** Connects a signal whose handler takes only the instance (clicked, activate, changed, apply...). */
fun CPointer<*>.on(signal: String, f: () -> Unit) {
    g_signal_connect_data(cast(), signal, voidTramp.reinterpret(), StableRef.create(f).asCPointer(), freeRef, 0u)
}

/** Runs [f] when a property changes. */
fun CPointer<*>.onNotify(property: String, f: () -> Unit) {
    g_signal_connect_data(cast(), "notify::$property", notifyTramp.reinterpret(), StableRef.create(f).asCPointer(), freeRef, 0u)
}

// --- main loop and threads ---------------------------------------------------

private val idleTramp = staticCFunction { d: COpaquePointer? ->
    val ref = d!!.asStableRef<() -> Unit>()
    val f = ref.get()
    ref.dispose()
    f()
    0
}

/** Queues [f] on the GTK main loop (safe from any thread). */
fun runOnMain(f: () -> Unit) {
    g_idle_add(idleTramp, StableRef.create(f).asCPointer())
}

private val threadTramp = staticCFunction { d: COpaquePointer? ->
    val ref = d!!.asStableRef<() -> Unit>()
    val f = ref.get()
    ref.dispose()
    f()
    null as COpaquePointer?
}

/** Runs [work] on a new thread and hands its result to [done] on the main loop. */
fun <T> background(work: () -> T, done: (T) -> Unit) {
    val job: () -> Unit = {
        val r = runCatching(work)
        runOnMain { r.onSuccess(done).onFailure { eprintln("background job failed: $it") } }
    }
    g_thread_unref(g_thread_new("xt-io", threadTramp, StableRef.create(job).asCPointer()))
}

private val asyncTramp = staticCFunction { src: CPointer<GObject>?, res: CPointer<GAsyncResult>?, d: COpaquePointer? ->
    val ref = d!!.asStableRef<(CPointer<GObject>?, CPointer<GAsyncResult>?) -> Unit>()
    val f = ref.get()
    ref.dispose()
    f(src, res)
}

/** A one-shot GAsyncReadyCallback and its user data. */
fun asyncCallback(f: (CPointer<GObject>?, CPointer<GAsyncResult>?) -> Unit) =
    asyncTramp to StableRef.create(f).asCPointer()

fun eprintln(s: String) {
    platform.posix.fprintf(platform.posix.stderr, "%s\n", "xt-launcher: $s")
}

// --- widgets ----------------------------------------------------------------

fun Widget.css(vararg classes: String): Widget {
    for (c in classes) gtk_widget_add_css_class(this, c)
    return this
}

fun Widget.margins(top: Int, end: Int = top, bottom: Int = top, start: Int = end): Widget {
    gtk_widget_set_margin_top(this, top)
    gtk_widget_set_margin_end(this, end)
    gtk_widget_set_margin_bottom(this, bottom)
    gtk_widget_set_margin_start(this, start)
    return this
}

fun Widget.expand(h: Boolean = false, v: Boolean = false): Widget {
    gtk_widget_set_hexpand(this, if (h) 1 else 0)
    gtk_widget_set_vexpand(this, if (v) 1 else 0)
    return this
}

fun Widget.align(h: GtkAlign? = null, v: GtkAlign? = null): Widget {
    h?.let { gtk_widget_set_halign(this, it) }
    v?.let { gtk_widget_set_valign(this, it) }
    return this
}

fun Widget.visible(v: Boolean): Widget {
    gtk_widget_set_visible(this, if (v) 1 else 0)
    return this
}

fun Widget.sensitive(v: Boolean): Widget {
    gtk_widget_set_sensitive(this, if (v) 1 else 0)
    return this
}

fun box(vertical: Boolean, spacing: Int = 0, vararg children: Widget): Widget {
    val b = gtk_box_new(if (vertical) GtkOrientation.GTK_ORIENTATION_VERTICAL else GtkOrientation.GTK_ORIENTATION_HORIZONTAL, spacing)!!
    for (c in children) gtk_box_append(b.cast(), c)
    return b
}

fun vbox(spacing: Int = 0, vararg children: Widget) = box(true, spacing, *children)
fun hbox(spacing: Int = 0, vararg children: Widget) = box(false, spacing, *children)

fun Widget.append(child: Widget): Widget {
    gtk_box_append(this.cast(), child)
    return this
}

fun Widget.clear() {
    while (true) {
        val c = gtk_widget_get_first_child(this) ?: break
        gtk_box_remove(this.cast(), c)
    }
}

fun label(text: String, vararg classes: String, wrap: Boolean = false, xalign: Float = 0.5f): Widget {
    val l = gtk_label_new(text)!!
    if (wrap) {
        gtk_label_set_wrap(l.cast(), 1)
        gtk_label_set_justify(l.cast(), GtkJustification.GTK_JUSTIFY_CENTER)
    }
    gtk_label_set_xalign(l.cast(), xalign)
    return l.css(*classes)
}

fun Widget.setLabel(text: String) = gtk_label_set_label(this.cast(), text)

fun button(text: String?, icon: String? = null, vararg classes: String, onClick: () -> Unit): Widget {
    val b = gtk_button_new()!!
    when {
        text != null && icon != null -> {
            val c = adw_button_content_new()!!
            adw_button_content_set_label(c.cast(), text)
            adw_button_content_set_icon_name(c.cast(), icon)
            gtk_button_set_child(b.cast(), c)
        }
        text != null -> gtk_button_set_label(b.cast(), text)
        icon != null -> gtk_button_set_icon_name(b.cast(), icon)
    }
    b.css(*classes)
    b.on("clicked", onClick)
    return b
}

fun scrolled(child: Widget): Widget {
    val s = gtk_scrolled_window_new()!!
    gtk_scrolled_window_set_policy(s.cast(), GtkPolicyType.GTK_POLICY_NEVER, GtkPolicyType.GTK_POLICY_AUTOMATIC)
    gtk_scrolled_window_set_child(s.cast(), child)
    return s.expand(h = true, v = true)
}

fun clamp(child: Widget, max: Int = 720): Widget {
    val c = adw_clamp_new()!!
    adw_clamp_set_maximum_size(c.cast(), max)
    adw_clamp_set_tightening_threshold(c.cast(), max - 120)
    adw_clamp_set_child(c.cast(), child)
    return c
}

/** A boxed-list group of rows with a heading. */
fun group(title: String?, description: String? = null, vararg rows: Widget): Widget {
    val g = adw_preferences_group_new()!!
    title?.let { adw_preferences_group_set_title(g.cast(), it) }
    description?.let { adw_preferences_group_set_description(g.cast(), it) }
    for (r in rows) adw_preferences_group_add(g.cast(), r)
    return g
}

fun Widget.add(row: Widget): Widget {
    adw_preferences_group_add(this.cast(), row)
    return this
}

fun Widget.remove(row: Widget) = adw_preferences_group_remove(this.cast(), row)

fun Widget.headerSuffix(w: Widget) = adw_preferences_group_set_header_suffix(this.cast(), w)

fun Widget.setTitle(t: String) = adw_preferences_row_set_title(this.cast(), t)
fun Widget.setSubtitle(t: String) = adw_action_row_set_subtitle(this.cast(), t)

fun actionRow(title: String, subtitle: String? = null, icon: String? = null): Widget {
    val r = adw_action_row_new()!!
    adw_preferences_row_set_title(r.cast(), title)
    adw_preferences_row_set_use_markup(r.cast(), 0)
    subtitle?.let { adw_action_row_set_subtitle(r.cast(), it) }
    icon?.let { adw_action_row_set_icon_name(r.cast(), it) }
    return r
}

fun Widget.suffix(w: Widget): Widget {
    adw_action_row_add_suffix(this.cast(), w)
    return this
}

fun Widget.prefix(w: Widget): Widget {
    adw_action_row_add_prefix(this.cast(), w)
    return this
}

fun Widget.activatable(w: Widget) = adw_action_row_set_activatable_widget(this.cast(), w)

fun stringList(items: List<String>): CPointer<GtkStringList> {
    val l = gtk_string_list_new(null)!!
    for (s in items) gtk_string_list_append(l, s)
    return l
}

fun comboRow(title: String, subtitle: String?, items: List<String>, selected: Int, onChange: (Int) -> Unit): Widget {
    val row = adw_combo_row_new()!!
    adw_preferences_row_set_title(row.cast(), title)
    adw_preferences_row_set_use_markup(row.cast(), 0)
    subtitle?.let { adw_action_row_set_subtitle(row.cast(), it) }
    val model = stringList(items)
    adw_combo_row_set_model(row.cast(), model.cast())
    g_object_unref(model)
    adw_combo_row_set_selected(row.cast(), selected.coerceAtLeast(0).toUInt())
    row.onNotify("selected") { onChange(adw_combo_row_get_selected(row.cast()).toInt()) }
    return row
}

fun Widget.setComboItems(items: List<String>, selected: Int) {
    val model = stringList(items)
    adw_combo_row_set_model(this.cast(), model.cast())
    g_object_unref(model)
    adw_combo_row_set_selected(this.cast(), selected.coerceAtLeast(0).toUInt())
}

fun switchRow(title: String, subtitle: String?, active: Boolean, onChange: (Boolean) -> Unit): Widget {
    val row = adw_switch_row_new()!!
    adw_preferences_row_set_title(row.cast(), title)
    adw_preferences_row_set_use_markup(row.cast(), 0)
    subtitle?.let { adw_action_row_set_subtitle(row.cast(), it) }
    adw_switch_row_set_active(row.cast(), if (active) 1 else 0)
    row.onNotify("active") { onChange(adw_switch_row_get_active(row.cast()) != 0) }
    return row
}

fun spinRow(title: String, subtitle: String?, min: Double, max: Double, value: Double, onChange: (Int) -> Unit): Widget {
    val row = adw_spin_row_new_with_range(min, max, 1.0)!!
    adw_preferences_row_set_title(row.cast(), title)
    adw_preferences_row_set_use_markup(row.cast(), 0)
    subtitle?.let { adw_action_row_set_subtitle(row.cast(), it) }
    adw_spin_row_set_value(row.cast(), value)
    row.onNotify("value") { onChange(adw_spin_row_get_value(row.cast()).toInt()) }
    return row
}

fun entryRow(title: String, text: String, onChange: (String) -> Unit): Widget {
    val row = adw_entry_row_new()!!
    adw_preferences_row_set_title(row.cast(), title)
    adw_preferences_row_set_use_markup(row.cast(), 0)
    gtk_editable_set_text(row.cast(), text)
    row.on("changed") { onChange(editableText(row)) }
    return row
}

fun editableText(w: Widget): String = gtk_editable_get_text(w.cast())?.toKString().orEmpty()

fun statusPage(icon: String, title: String, description: String?, child: Widget? = null): Widget {
    val s = adw_status_page_new()!!
    adw_status_page_set_icon_name(s.cast(), icon)
    adw_status_page_set_title(s.cast(), title)
    description?.let { adw_status_page_set_description(s.cast(), it) }
    child?.let { adw_status_page_set_child(s.cast(), it) }
    return s
}

/** Opens a folder in the file manager (through the portal inside Flatpak). */
fun openFolder(window: Widget?, path: String) {
    val file = g_file_new_for_path(path) ?: return
    val launcher = gtk_file_launcher_new(file)
    gtk_file_launcher_launch(launcher, window?.cast(), null, null, null)
    g_object_unref(launcher)
    g_object_unref(file)
}
