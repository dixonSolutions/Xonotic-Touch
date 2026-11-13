package io.github.dixonsolutions.xonotictouch

import android.app.Activity
import android.app.AlertDialog
import android.app.Application
import android.graphics.Color
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast

internal object NativeBridge {
    init { System.loadLibrary("nativeui") }
    private external fun enqueueBytes(command: ByteArray): Boolean
    private external fun statusBytes(): ByteArray
    private external fun permissionsBytes(): ByteArray
    fun enqueue(command: String): Boolean = enqueueBytes(command.toByteArray(Charsets.UTF_8))
    fun status(): String = statusBytes().toString(Charsets.UTF_8)
    fun permissions(): String = permissionsBytes().toString(Charsets.UTF_8)
}

class NativeUIApplication : Application(), Application.ActivityLifecycleCallbacks {
    override fun onCreate() {
        super.onCreate()
        registerActivityLifecycleCallbacks(this)
    }
    override fun onActivityResumed(activity: Activity) {
        if (activity !is XonoticActivity) return
        val root = activity.window.decorView as? ViewGroup ?: return
        if (root.findViewWithTag<android.view.View>("native-console-button") != null) return
        val button = Button(activity).apply {
            tag = "native-console-button"
            text = ">_"
            contentDescription = "Open console and plugins"
            setTextColor(Color.WHITE)
            setBackgroundColor(0xBB202632.toInt())
            setOnClickListener { showConsole(activity) }
        }
        val density = activity.resources.displayMetrics.density
        root.addView(button, FrameLayout.LayoutParams((52 * density).toInt(), (48 * density).toInt(), Gravity.TOP or Gravity.END))
    }
    override fun onActivityCreated(activity: Activity, state: Bundle?) {}
    override fun onActivityStarted(activity: Activity) {}
    override fun onActivityPaused(activity: Activity) {}
    override fun onActivityStopped(activity: Activity) {}
    override fun onActivitySaveInstanceState(activity: Activity, state: Bundle) {}
    override fun onActivityDestroyed(activity: Activity) {
        dialogs.remove(activity)?.toList()?.forEach { it.dismiss() }
    }

    private val dialogs = mutableMapOf<Activity, MutableSet<AlertDialog>>()
    private fun showDialog(activity: Activity, dialog: AlertDialog, onDismiss: () -> Unit = {}) {
        dialogs.getOrPut(activity) { mutableSetOf() }.add(dialog)
        dialog.setOnDismissListener {
            dialogs[activity]?.let { set ->
                set.remove(dialog)
                if (set.isEmpty()) dialogs.remove(activity)
            }
            onDismiss()
        }
        dialog.show()
    }

    private val history = ArrayList<String>()
    private fun send(activity: Activity, command: String): Boolean {
        val ok = NativeBridge.enqueue(command)
        if (!ok) Toast.makeText(activity, "Command queue full or command too long", Toast.LENGTH_SHORT).show()
        return ok
    }
    private fun showConsole(activity: Activity) {
        val layout = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL; setPadding(24, 8, 24, 8) }
        val log = TextView(activity).apply { text = history.joinToString("\n") { "> $it" } }
        val scroll = ScrollView(activity).apply { addView(log) }
        layout.addView(scroll, LinearLayout.LayoutParams(-1, 0, 1f))
        val input = EditText(activity).apply { hint = "Engine command"; isSingleLine = true; imeOptions = EditorInfo.IME_ACTION_SEND }
        layout.addView(input)
        val buttons = LinearLayout(activity)
        val previous = Button(activity).apply { text = "Previous" }
        val run = Button(activity).apply { text = "Run" }
        val plugins = Button(activity).apply { text = "Plugins" }
        buttons.addView(previous); buttons.addView(run); buttons.addView(plugins)
        layout.addView(buttons)
        val dialog = AlertDialog.Builder(activity).setTitle("Console").setView(layout).setNegativeButton("Close", null).create()
        var historyIndex = history.size
        fun execute() {
            val command = input.text.toString().trim()
            if (command.isNotEmpty() && send(activity, command)) {
                history.add(command)
                if (history.size > 100) history.removeAt(0)
                historyIndex = history.size
                log.text = history.joinToString("\n") { "> $it" }
                input.text.clear()
                scroll.post { scroll.fullScroll(ScrollView.FOCUS_DOWN) }
            }
        }
        previous.setOnClickListener {
            if (history.isNotEmpty()) {
                historyIndex = (historyIndex - 1).coerceAtLeast(0)
                input.setText(history[historyIndex]); input.setSelection(input.length())
            }
        }
        run.setOnClickListener { execute() }
        input.setOnEditorActionListener { _, action, _ -> if (action == EditorInfo.IME_ACTION_SEND) { execute(); true } else false }
        plugins.setOnClickListener { showPlugins(activity) }
        showDialog(activity, dialog)
        dialog.window?.setLayout((activity.resources.displayMetrics.widthPixels * .8).toInt(), (activity.resources.displayMetrics.heightPixels * .8).toInt())
    }
    private fun showPlugins(activity: Activity) {
        val list = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL; setPadding(24, 8, 24, 8) }
        val scroll = ScrollView(activity).apply { addView(list) }
        val dialog = AlertDialog.Builder(activity).setTitle("Client plugins").setView(scroll).setNegativeButton("Close", null).create()
        val handler = Handler(Looper.getMainLooper())
        var last: String? = null
        val refresh = object : Runnable {
            override fun run() {
                if (!dialog.isShowing) return
                val current = NativeBridge.status()
                if (current != last) {
                    last = current
                    list.removeAllViews()
                    val rows = current.lineSequence().filter { it.isNotBlank() }.toList()
                    if (rows.isEmpty()) list.addView(TextView(activity).apply { text = "No client plugins found." })
                    rows.forEach { row ->
                        val fields = row.split('\t')
                        if (fields.size < 2) return@forEach
                        val name = fields[0]
                        val state = fields[1]
                        val title = fields.getOrNull(2).orEmpty().ifEmpty { name }
                        val detail = listOf(state, fields.getOrNull(3).orEmpty(), fields.getOrNull(4).orEmpty()).filter { it.isNotBlank() }.joinToString(" · ")
                        list.addView(TextView(activity).apply { text = "$title\n$detail" })
                        val action = Button(activity).apply {
                            text = if (state == "off") "Enable" else "Disable"
                            isEnabled = name.matches(Regex("[A-Za-z0-9_-]+"))
                            setOnClickListener { send(activity, "lua_mod_${if (state == "off") "enable" else "disable"} $name") }
                        }
                        list.addView(action)
                        list.addView(Button(activity).apply {
                            text = "Permissions"
                            isEnabled = name.matches(Regex("[A-Za-z0-9_-]+"))
                            setOnClickListener { showPermissions(activity, name, title) }
                        })
                    }
                }
                handler.postDelayed(this, 500)
            }
        }
        showDialog(activity, dialog) { handler.removeCallbacks(refresh) }
        send(activity, "lua_mods")
        handler.post(refresh)
    }
    private fun showPermissions(activity: Activity, name: String, title: String) {
        val list = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL; setPadding(24, 8, 24, 8) }
        val dialog = AlertDialog.Builder(activity).setTitle("Permissions: $title")
            .setView(ScrollView(activity).apply { addView(list) }).setNegativeButton("Close", null).create()
        val handler = Handler(Looper.getMainLooper())
        var last: String? = null
        val refresh = object : Runnable {
            override fun run() {
                if (!dialog.isShowing) return
                val row = NativeBridge.permissions().lineSequence()
                    .map { it.split('\t') }.firstOrNull { it.size >= 4 && it[0] == name }
                val current = row?.joinToString("\t").orEmpty()
                if (current != last) {
                    last = current
                    list.removeAllViews()
                    if (row == null || row[1] == "invalid") {
                        list.addView(TextView(activity).apply { text = "Permissions unavailable." })
                    } else {
                        val granted = row[3].split(',').toSet()
                        val requested = row[2].split(',').filter { it.isNotEmpty() }
                        if (requested.isEmpty()) list.addView(TextView(activity).apply { text = "No permissions requested." })
                        requested.forEach { scope ->
                            if (!scope.matches(Regex("[a-z_]+"))) return@forEach
                            list.addView(CheckBox(activity).apply {
                                text = scope.replace('_', ' ')
                                isChecked = scope in granted
                                setOnClickListener {
                                    if (send(activity, "lua_mod_permission $name ${if (isChecked) "grant" else "revoke"} $scope")) {
                                        // Re-read the engine's effective permissions after the command runs.
                                        last = null
                                    }
                                }
                            })
                        }
                    }
                }
                handler.postDelayed(this, 500)
            }
        }
        showDialog(activity, dialog) { handler.removeCallbacks(refresh) }
        handler.post(refresh)
    }

}
