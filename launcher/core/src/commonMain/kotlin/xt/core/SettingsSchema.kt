// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core

import xt.core.Setting.Phase.STARTUP
import xt.core.Setting.Store.LAUNCHER
import xt.core.Setting.Type.*

/**
 * The settings players actually change, by page. Names and defaults are the
 * engine's and the game code's (cvar definitions in the engine sources,
 * xonotic-client.cfg, touch/xonotic.cfg). The long tail stays in the game's
 * own menu ("All settings…").
 */
object SettingsSchema {
    data class Page(val id: String, val title: String, val icon: String)

    val pages = listOf(
        Page("video", "Video", "video-display-symbolic"),
        Page("audio", "Audio", "audio-volume-high-symbolic"),
        Page("game", "Game", "applications-games-symbolic"),
        Page("input", "Input", "input-mouse-symbolic"),
        Page("player", "Player", "avatar-default-symbolic"),
        Page("touch", "Touch", "input-touchpad-symbolic"),
        Page("privacy", "Privacy", "security-high-symbolic"),
        Page("updates", "Updates", "software-update-available-symbolic"),
        Page("data", "Data", "folder-download-symbolic"),
    )

    val qualityPresets = listOf(
        Choice("", "Keep current"),
        Choice("low", "Low"),
        Choice("med", "Medium"),
        Choice("normal", "Normal"),
        Choice("high", "High"),
        Choice("ultra", "Ultra"),
        Choice("ultimate", "Ultimate"),
    )

    val colors = listOf(
        "White", "Brown", "Light blue", "Green", "Red", "Gold", "Orange", "Pink",
        "Purple", "Magenta", "Beige", "Cyan", "Yellow", "Blue", "Orange red", "Dark red",
    ).mapIndexed { i, t -> Choice("$i", t) }

    val playerModels = listOf("erebus", "gak", "ignis", "megaerebus", "nyx", "pyria", "seraphina", "umbra")

    private fun s(page: String, group: String, key: String, type: Setting.Type, label: String, description: String = "",
                  default: String = "0", min: Double = 0.0, max: Double = 1.0, step: Double = 1.0,
                  choices: List<Choice> = emptyList(), phase: Setting.Phase = Setting.Phase.GAME,
                  store: Setting.Store = Setting.Store.CVAR, on: String = "1", off: String = "0", note: String = "") =
        Setting(key, type, label, description, default, min, max, step, choices, page, group, phase, store, on, off, note)

    val all: List<Setting> = listOf(
        // Video
        s("video", "Display", "vid_fullscreen", BOOL, "Fullscreen", default = "1", phase = STARTUP),
        s("video", "Display", Keys.RESOLUTION, CHOICE, "Resolution", "Automatic uses the screen's own size",
            default = "", choices = Choices.resolutions, store = LAUNCHER, phase = STARTUP),
        s("video", "Display", "vid_vsync", BOOL, "Vertical sync", "Ties the frame rate to the screen; no tearing, a little more input delay"),
        s("video", "Display", "cl_maxfps", INT, "Frame rate limit", "Highest frame rate the game draws (0 = no limit). Lower is cooler and lasts longer on battery",
            default = "0", min = 0.0, max = 360.0, step = 10.0),
        s("video", "Display", "fov", INT, "Field of view", "Degrees across the screen", default = "100", min = 60.0, max = 130.0, step = 5.0),
        s("video", "Quality", Keys.PERF_PROFILE, CHOICE, "Touch performance profile",
            "Applied at every start when chosen; Automatic keeps the game's first-run choice and what you change in its menus",
            default = "", store = LAUNCHER, choices = listOf(Choice("", "Automatic"))),
        s("video", "Quality", "quality_preset", CHOICE, "Effects preset", "Applies the game's effects preset once, at the next start",
            default = "", choices = qualityPresets, store = LAUNCHER),
        s("video", "Quality", "r_viewscale", FLOAT, "3D resolution scale", "1 = full resolution", default = "1", min = 0.4, max = 1.0, step = 0.05),
        s("video", "Quality", "r_viewscale_fpsscaling", BOOL, "Dynamic resolution", "Lowers the 3D resolution to hold the frame rate"),
        s("video", "Quality", "vid_samples", CHOICE, "Anti-aliasing", default = "1", phase = STARTUP,
            choices = listOf(Choice("1", "Off"), Choice("2", "2×"), Choice("4", "4×"), Choice("8", "8×"))),
        s("video", "Quality", "gl_texture_anisotropy", CHOICE, "Anisotropic filtering", default = "1",
            choices = listOf(Choice("1", "Off"), Choice("2", "2×"), Choice("4", "4×"), Choice("8", "8×"), Choice("16", "16×"))),
        s("video", "Quality", "cl_particles_quality", FLOAT, "Particle quality", default = "1", min = 0.25, max = 3.0, step = 0.25),
        s("video", "Picture", "v_gamma", FLOAT, "Gamma", default = "1.125", min = 0.5, max = 2.0, step = 0.05),
        s("video", "Picture", "v_contrast", FLOAT, "Contrast", default = "1", min = 1.0, max = 3.0, step = 0.1),
        s("video", "Picture", "cl_showfps", BOOL, "Show frame rate"),

        // Audio
        s("audio", "Volume", "volume", FLOAT, "Master volume", default = "0.7", min = 0.0, max = 1.0, step = 0.05),
        s("audio", "Volume", "bgmvolume", FLOAT, "Music", default = "1", min = 0.0, max = 1.0, step = 0.05),
        s("audio", "Volume", "snd_staticvolume", FLOAT, "Ambient sounds", default = "1", min = 0.0, max = 1.0, step = 0.05),
        s("audio", "Sounds", "cl_hitsound", CHOICE, "Hit sound", "A sound when your shot hits",
            default = "1", choices = listOf(Choice("0", "Off"), Choice("1", "Constant"), Choice("2", "Higher for more damage"), Choice("3", "Lower for more damage"))),
        s("audio", "Sounds", "snd_mutewhenidle", BOOL, "Mute when the window is in the background", default = "1"),

        // Game
        s("game", "View", "r_drawviewmodel", BOOL, "Show your weapon", default = "1"),
        s("game", "View", "cl_bobcycle", BOOL, "View bob", "Sway the view while running", default = "0", on = "0.6", off = "0"),
        s("game", "View", "crosshair_size", FLOAT, "Crosshair size", default = "0.6", min = 0.1, max = 2.0, step = 0.05),
        s("game", "View", "cl_gentle", BOOL, "Gentle mode", "No blood or gore"),
        s("game", "Weapons", "cl_autoswitch", BOOL, "Switch to a better weapon on pickup", default = "1"),
        s("game", "Weapons", "cl_damagetext", BOOL, "Damage numbers", "Show the damage each hit does", default = "1"),

        // Input
        s("input", "Mouse", "sensitivity", FLOAT, "Mouse sensitivity", default = "6", min = 0.5, max = 30.0, step = 0.5),
        s("input", "Mouse", "m_pitch", BOOL, "Invert mouse Y", default = "0.022", on = "-0.022", off = "0.022"),
        s("input", "Mouse", "m_filter", BOOL, "Smooth mouse movement"),
        s("input", "Controller", "joy_enable", BOOL, "Use a game controller", default = "1"),

        // Player
        s("player", "Profile", "_cl_name", STRING, "Name", "What other players see", default = "Player"),
        s("player", "Profile", "_cl_playermodel", CHOICE, "Model", default = "models/player/erebus.iqm",
            choices = playerModels.map { Choice("models/player/$it.iqm", it.replaceFirstChar { c -> c.uppercaseChar() }) }),
        s("player", "Profile", "_cl_color", COLOR, "Colours", "Shirt and pants", default = "0", max = 255.0, choices = colors),

        // Touch / mobile
        s("touch", "Controls", Keys.TOUCH_PROFILE, CHOICE, "Controls preset", "The layout start.sh applies at every start",
            default = "", store = LAUNCHER, choices = listOf(Choice("", "Standard"))),
        s("touch", "Controls", "vid_touchscreen_mode", CHOICE, "Touch controls", default = "1",
            choices = listOf(Choice("1", "Dynamic (touch or keyboard, whichever you use)"), Choice("2", "Always touch"), Choice("0", "Keyboard and mouse"))),
        s("touch", "Controls", "touch_handedness", BOOL, "Left-handed"),
        s("touch", "Controls", "touch_fire_mode", CHOICE, "Fire button", default = "0", choices = listOf(Choice("0", "Hold"), Choice("1", "Toggle"))),
        s("touch", "HUD", "touch_opacity", FLOAT, "Control opacity", default = "0.9", min = 0.3, max = 1.0, step = 0.05),
        s("touch", "HUD", "touch_scale", FLOAT, "Control size", default = "1", min = 0.7, max = 1.4, step = 0.05),
        s("touch", "HUD", "touch_hud_scale", FLOAT, "HUD size", default = "0.85", min = 0.6, max = 1.4, step = 0.05),
        s("touch", "Look", "touch_sens_base", FLOAT, "Look speed", default = "2.8", min = 0.1, max = 50.0, step = 0.1),
        s("touch", "Look", "touch_invert_y", BOOL, "Invert look Y"),
        s("touch", "Assists", "cl_touch_aimassist", BOOL, "Aim assist", "Slows and pulls your aim near an enemy", default = "1"),
        s("touch", "Assists", "cl_touch_autoshoot", BOOL, "Auto-shoot", "Fires when your aim is on an enemy", default = "1"),
        s("touch", "Assists", "cl_touch_autododge", BOOL, "Auto dodge", default = "1"),
        s("touch", "Assists", "cl_touch_hook_rescue", BOOL, "Hook rescue", "Catch me with the hook when I fall off the map", default = "1"),
        s("touch", "Tactics", "cl_touch_esp_enabled", BOOL, "ESP and practice helpers", "See enemies through walls, pickup magnet and the other practice aids",
            default = "local", on = "local", off = "off",
            note = "Offline only: these helpers work in matches on this device (bots, a LAN game you host) and never on another server."),

        // Privacy
        s("privacy", "Player statistics (XonStat)", "cl_allow_uidtracking", BOOL, "Send my match statistics to XonStat",
            "Servers that report to XonStat send your results with your player ID. Off: you are recorded as an anonymous player",
            note = "Off on a new install. A change applies the next time you join a server."),
        s("privacy", "Player statistics (XonStat)", "cl_allow_uid2name", BOOL, "Show my player name there",
            "Your name appears with your statistics and in race and CTF records"),
        s("privacy", "Player statistics (XonStat)", "cl_allow_uidranking", BOOL, "Rank me in leaderboards",
            "Your statistics count toward Elo rankings and leaderboards"),
    )

    fun page(id: String) = all.filter { it.page == id }

    /** "Shirt" and "pants" are one engine cvar, _cl_color = shirt * 16 + pants. */
    fun colorOf(clColor: String?): Pair<Int, Int> {
        val c = clColor?.toDoubleOrNull()?.toInt() ?: 0
        return ((c shr 4) and 15) to (c and 15)
    }

    fun clColor(shirt: Int, pants: Int) = ((shirt and 15) shl 4) or (pants and 15)
}
