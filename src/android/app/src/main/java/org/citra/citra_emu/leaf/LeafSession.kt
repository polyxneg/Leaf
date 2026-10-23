// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

package org.citra.citra_emu.leaf

import android.app.Activity
import android.content.Context
import android.hardware.input.InputManager
import android.os.Build
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import org.citra.citra_emu.features.settings.model.BooleanSetting
import org.citra.citra_emu.features.settings.model.view.InputBindingSetting
import org.citra.citra_emu.utils.EmulationMenuSettings
import org.citra.citra_emu.utils.Log

/**
 * Per-game-session glue for the Leaf single-screen experience:
 *  - applies the layout / camera aspect before the ROM boots,
 *  - auto-maps hardware controllers and hides touch controls when one is present,
 *  - R2 opens the tool ring (see LeafToolRing),
 *  - L3 / ZL / R3 are swallowed: the raw bottom screen is never shown on its own.
 *
 * ACNL does not use ZL, ZR or the C-stick, so these never collide with game controls.
 */
object LeafSession {
    /** Must run before the game boots: the camera aspect is read once by the game at startup. */
    fun configure(activity: Activity) {
        // The single screen is centred, so display cutouts fall into the side bars: use the
        // whole display instead of shrinking the game around them.
        BooleanSetting.EXPAND_TO_CUTOUT_AREA.boolean = true
        val (width, height) = windowSize(activity)
        val mode = LeafSettings.resolvedDisplayMode(width, height)
        LeafNative.setLayout(true, mode)
        LeafNative.setCameraAspect(LeafNative.cameraAspectFor(mode, width, height))
        LeafNative.setAutoStack(LeafSettings.autoStack)
        LeafNative.setPanelStyle(
            LeafSettings.panelScale,
            LeafSettings.panelOpacity,
            LeafSettings.panelAnchor
        )
        @Suppress("DEPRECATION")
        val display = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            activity.display
        } else {
            activity.windowManager.defaultDisplay
        }
        LeafNative.setDisplayRefresh(display?.refreshRate ?: 0f)
        LeafNative.setPanelVisible(false)
        refreshInputMode(activity)
    }

    /** Re-evaluates touch-control visibility and controller mappings (device (dis)connects). */
    fun refreshInputMode(context: Context) {
        EmulationMenuSettings.showOverlay = LeafSettings.touchControlsVisible(context)
        autoMapControllerIfNeeded(context)
    }

    private fun windowSize(activity: Activity): Pair<Int, Int> {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val bounds = activity.windowManager.currentWindowMetrics.bounds
            Pair(maxOf(bounds.width(), bounds.height()), minOf(bounds.width(), bounds.height()))
        } else {
            val metrics = activity.resources.displayMetrics
            Pair(
                maxOf(metrics.widthPixels, metrics.heightPixels),
                minOf(metrics.widthPixels, metrics.heightPixels)
            )
        }
    }

    // --- Controller auto-mapping -------------------------------------------------------------

    private fun firstGamepad(context: Context): InputDevice? {
        val inputManager = context.getSystemService(Context.INPUT_SERVICE) as InputManager
        return inputManager.inputDeviceIds.asSequence()
            .mapNotNull { inputManager.getInputDevice(it) }
            .firstOrNull { !it.isVirtual && LeafSettings.isGamepad(it) }
    }

    fun autoMapControllerIfNeeded(context: Context, force: Boolean = false) {
        if (!LeafSettings.autoMapControllers && !force) return
        val device = firstGamepad(context) ?: return
        val signature = "${device.descriptor}|${LeafSettings.nintendoLayout}"
        if (!force && signature == LeafSettings.mappedSignature) return

        val axisDpad = device.motionRanges.any {
            it.axis == MotionEvent.AXIS_HAT_X || it.axis == MotionEvent.AXIS_HAT_Y
        } || !device.hasKeys(KeyEvent.KEYCODE_DPAD_UP).first()

        InputBindingSetting.clearAllBindings()
        if (InputBindingSetting.isJoyCon(device)) {
            InputBindingSetting.applyJoyConBindings()
        } else {
            InputBindingSetting.applyAutoMapBindings(LeafSettings.nintendoLayout, axisDpad)
        }
        LeafSettings.mappedSignature = signature
        Log.info("[Leaf] Auto-mapped ${device.name} (nintendo=${LeafSettings.nintendoLayout})")
    }

    // --- Hotkeys -----------------------------------------------------------------------------

    /** Returns true when the event was consumed by a Leaf hotkey. */
    fun handleKeyEvent(event: KeyEvent): Boolean {
        if (LeafKeyboardHand.handleKeyEvent(event)) return true
        if (LeafToolRing.handleKeyEvent(event)) return true
        return when (event.keyCode) {
            // Unused by the game; consumed so no emulator hotkey can bring up the bottom screen.
            KeyEvent.KEYCODE_BUTTON_THUMBL, KeyEvent.KEYCODE_BUTTON_L2,
            KeyEvent.KEYCODE_BUTTON_THUMBR -> true

            else -> false
        }
    }

    /** Observes joystick motion; returns true when the game must not see it (tool ring open). */
    fun onMotionEvent(event: MotionEvent): Boolean {
        if (event.source and InputDevice.SOURCE_CLASS_JOYSTICK == 0) return false
        LeafKeyboardHand.onMotionEvent(event)
        return LeafToolRing.onMotionEvent(event)
    }
}
