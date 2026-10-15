// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

package org.citra.citra_emu.leaf

import android.content.Context
import android.content.SharedPreferences
import android.hardware.input.InputManager
import android.view.InputDevice
import androidx.core.content.edit
import androidx.preference.PreferenceManager
import org.citra.citra_emu.CitraApplication

/** Preferences specific to the Leaf app; emulator settings stay in Azahar's own config. */
object LeafSettings {
    // Must match Leaf::DisplayMode in src/core/leaf/leaf.h
    const val DISPLAY_16_9 = 0
    const val DISPLAY_4_3 = 1
    const val DISPLAY_ORIGINAL = 2
    const val DISPLAY_AUTO = 100 // resolved to 16:9 or 4:3 from the device screen

    const val TOUCH_CONTROLS_AUTO = 0 // hidden while a gamepad is connected
    const val TOUCH_CONTROLS_ON = 1
    const val TOUCH_CONTROLS_OFF = 2

    const val ANCHOR_CENTER = 0
    const val ANCHOR_RIGHT = 1
    const val ANCHOR_LEFT = 2

    private const val KEY_ROM_URI = "leaf_rom_uri"
    private const val KEY_ROM_TITLE = "leaf_rom_title"
    private const val KEY_DISPLAY_MODE = "leaf_display_mode"
    private const val KEY_TOUCH_CONTROLS = "leaf_touch_controls"
    private const val KEY_AUTO_MAP = "leaf_auto_map"
    private const val KEY_NINTENDO_LAYOUT = "leaf_nintendo_layout"
    private const val KEY_MAPPED_SIGNATURE = "leaf_mapped_signature"
    private const val KEY_PANEL_SCALE = "leaf_panel_scale"
    private const val KEY_PANEL_OPACITY = "leaf_panel_opacity"
    private const val KEY_PANEL_ANCHOR = "leaf_panel_anchor"
    private const val KEY_SHOW_CLOCK = "leaf_show_clock"
    private const val KEY_SHOW_HINTS = "leaf_show_hints"
    private const val KEY_RESOLUTION = "leaf_resolution"
    private const val KEY_SHARP_UI_REMOVED = "leaf_sharp_ui_removed"
    private const val KEY_CPU_CLOCK = "leaf_cpu_clock"
    private const val KEY_DEFAULTS_APPLIED = "leaf_defaults_applied_v1"

    private val prefs: SharedPreferences
        get() = PreferenceManager.getDefaultSharedPreferences(CitraApplication.appContext)

    var romUri: String
        get() = prefs.getString(KEY_ROM_URI, "") ?: ""
        set(value) = prefs.edit { putString(KEY_ROM_URI, value) }

    var romTitle: String
        get() = prefs.getString(KEY_ROM_TITLE, "") ?: ""
        set(value) = prefs.edit { putString(KEY_ROM_TITLE, value) }

    var displayMode: Int
        get() = prefs.getInt(KEY_DISPLAY_MODE, DISPLAY_AUTO)
        set(value) = prefs.edit { putInt(KEY_DISPLAY_MODE, value) }

    var touchControls: Int
        get() = prefs.getInt(KEY_TOUCH_CONTROLS, TOUCH_CONTROLS_AUTO)
        set(value) = prefs.edit { putInt(KEY_TOUCH_CONTROLS, value) }

    var autoMapControllers: Boolean
        get() = prefs.getBoolean(KEY_AUTO_MAP, true)
        set(value) = prefs.edit { putBoolean(KEY_AUTO_MAP, value) }

    /** True when the controller reports Nintendo-style codes (A on the right). */
    var nintendoLayout: Boolean
        get() = prefs.getBoolean(KEY_NINTENDO_LAYOUT, false)
        set(value) = prefs.edit { putBoolean(KEY_NINTENDO_LAYOUT, value) }

    /** Identifies the controller + layout that the current bindings were generated for. */
    var mappedSignature: String
        get() = prefs.getString(KEY_MAPPED_SIGNATURE, "") ?: ""
        set(value) = prefs.edit { putString(KEY_MAPPED_SIGNATURE, value) }

    var panelScale: Float
        get() = prefs.getFloat(KEY_PANEL_SCALE, 0.82f)
        set(value) = prefs.edit { putFloat(KEY_PANEL_SCALE, value) }

    var panelOpacity: Float
        get() = prefs.getFloat(KEY_PANEL_OPACITY, 0.96f)
        set(value) = prefs.edit { putFloat(KEY_PANEL_OPACITY, value) }

    var panelAnchor: Int
        get() = prefs.getInt(KEY_PANEL_ANCHOR, ANCHOR_CENTER)
        set(value) = prefs.edit { putInt(KEY_PANEL_ANCHOR, value) }

    var showClock: Boolean
        get() = prefs.getBoolean(KEY_SHOW_CLOCK, true)
        set(value) = prefs.edit { putBoolean(KEY_SHOW_CLOCK, value) }

    var showHints: Boolean
        get() = prefs.getBoolean(KEY_SHOW_HINTS, true)
        set(value) = prefs.edit { putBoolean(KEY_SHOW_HINTS, value) }

    /**
     * Earlier builds switched Azahar's custom textures on for a generated "sharp UI" pack; that
     * feature is gone, so the setting is turned back off once (users can enable it again).
     */
    var sharpUiRemoved: Boolean
        get() = prefs.getBoolean(KEY_SHARP_UI_REMOVED, false)
        set(value) = prefs.edit { putBoolean(KEY_SHARP_UI_REMOVED, value) }

    /** Emulated CPU clock in percent of the 3DS's (more removes the game's own slowdowns). */
    var cpuClock: Int
        get() = prefs.getInt(KEY_CPU_CLOCK, 150)
        set(value) = prefs.edit { putInt(KEY_CPU_CLOCK, value.coerceIn(25, 400)) }

    /** Internal rendering resolution multiplier: 1 = native 400x240, 2 = 800x480, 3 = 1200x720. */
    var resolution: Int
        get() = prefs.getInt(KEY_RESOLUTION, 2)
        set(value) = prefs.edit { putInt(KEY_RESOLUTION, value.coerceIn(1, 3)) }

    var defaultsApplied: Boolean
        get() = prefs.getBoolean(KEY_DEFAULTS_APPLIED, false)
        set(value) = prefs.edit { putBoolean(KEY_DEFAULTS_APPLIED, value) }

    /** Display mode actually used, with [DISPLAY_AUTO] resolved against the screen size. */
    fun resolvedDisplayMode(widthPx: Int, heightPx: Int): Int {
        val mode = displayMode
        if (mode != DISPLAY_AUTO) return mode
        val longSide = maxOf(widthPx, heightPx).toFloat()
        val shortSide = minOf(widthPx, heightPx).toFloat().coerceAtLeast(1f)
        // 4:3 = 1.33, 16:9 = 1.78: pick whichever the screen is closer to.
        return if (longSide / shortSide < 1.55f) DISPLAY_4_3 else DISPLAY_16_9
    }

    fun gamepadConnected(context: Context): Boolean {
        val inputManager = context.getSystemService(Context.INPUT_SERVICE) as InputManager
        return inputManager.inputDeviceIds.any { id ->
            val device = inputManager.getInputDevice(id) ?: return@any false
            !device.isVirtual && isGamepad(device)
        }
    }

    fun isGamepad(device: InputDevice): Boolean {
        val sources = device.sources
        return (sources and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD) ||
            (sources and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK)
    }

    fun touchControlsVisible(context: Context): Boolean = when (touchControls) {
        TOUCH_CONTROLS_ON -> true
        TOUCH_CONTROLS_OFF -> false
        else -> !gamepadConnected(context)
    }
}
