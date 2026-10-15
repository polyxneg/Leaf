// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

package org.citra.citra_emu.leaf

import android.app.Activity
import android.content.Context
import android.hardware.input.InputManager
import android.os.Build
import android.os.SystemClock
import android.view.Choreographer
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.features.settings.model.BooleanSetting
import org.citra.citra_emu.features.settings.model.view.InputBindingSetting
import org.citra.citra_emu.utils.EmulationMenuSettings
import org.citra.citra_emu.utils.Log
import kotlin.math.abs
import kotlin.math.hypot

/**
 * Per-game-session glue for the Leaf single-screen experience:
 *  - applies the layout / camera aspect before the ROM boots,
 *  - auto-maps hardware controllers and hides touch controls when one is present,
 *  - drives a gamepad cursor so the bottom-screen elements placed on the main screen can be
 *    used without touching the screen (right stick moves, R3 / ZR taps),
 *  - L3 / ZL are swallowed: the raw bottom screen is never shown on its own.
 *
 * ACNL does not use ZL, ZR or the C-stick, so these never collide with game controls.
 */
object LeafSession {
    interface CursorListener {
        fun onCursorChanged()
    }

    var cursorListener: CursorListener? = null

    // Cursor in surface (view) pixels.
    var cursorX = 0f
        private set
    var cursorY = 0f
        private set
    var cursorPressed = false
        private set
    private var cursorLastUsed = 0L
    private var stickX = 0f
    private var stickY = 0f
    private var viewWidth = 0
    private var viewHeight = 0
    private var ticking = false
    private var lastFrameNanos = 0L
    private var triggerTapDown = false

    private const val CURSOR_SPEED_DP_PER_SECOND = 900f
    private const val CURSOR_HIDE_AFTER_MS = 2500L
    private const val STICK_DEADZONE = 0.18f
    private const val TRIGGER_THRESHOLD = 0.5f

    val cursorVisible: Boolean
        get() = SystemClock.uptimeMillis() - cursorLastUsed < CURSOR_HIDE_AFTER_MS || cursorPressed

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
        cursorX = width * 0.5f
        cursorY = height * 0.5f
    }

    /** Re-evaluates touch-control visibility and controller mappings (device (dis)connects). */
    fun refreshInputMode(context: Context) {
        EmulationMenuSettings.showOverlay = LeafSettings.touchControlsVisible(context)
        autoMapControllerIfNeeded(context)
    }

    fun onViewSize(width: Int, height: Int) {
        viewWidth = width
        viewHeight = height
        cursorX = cursorX.coerceIn(0f, width.toFloat())
        cursorY = cursorY.coerceIn(0f, height.toFloat())
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
        val down = event.action == KeyEvent.ACTION_DOWN
        val up = event.action == KeyEvent.ACTION_UP
        if (!down && !up) return false
        return when (event.keyCode) {
            // Unused by the game; consumed so no emulator hotkey can bring up the bottom screen.
            KeyEvent.KEYCODE_BUTTON_THUMBL, KeyEvent.KEYCODE_BUTTON_L2 -> true

            KeyEvent.KEYCODE_BUTTON_THUMBR, KeyEvent.KEYCODE_BUTTON_R2 -> {
                if (event.repeatCount == 0) setTap(down)
                true
            }

            else -> false
        }
    }

    /** Observes joystick motion; never consumes it (the left stick still drives the game). */
    fun onMotionEvent(event: MotionEvent) {
        if (event.source and InputDevice.SOURCE_CLASS_JOYSTICK == 0) return
        LeafKeyboardHand.onMotionEvent(event)
        val device = event.device ?: return
        fun axis(id: Int) =
            if (device.getMotionRange(id, event.source) != null) event.getAxisValue(id) else 0f

        // Right stick: Z/RZ on most Android pads, RX/RY on some.
        var x = axis(MotionEvent.AXIS_Z)
        var y = axis(MotionEvent.AXIS_RZ)
        if (abs(x) < STICK_DEADZONE && abs(y) < STICK_DEADZONE) {
            x = axis(MotionEvent.AXIS_RX)
            y = axis(MotionEvent.AXIS_RY)
        }
        stickX = if (abs(x) < STICK_DEADZONE) 0f else x
        stickY = if (abs(y) < STICK_DEADZONE) 0f else y
        if (stickX != 0f || stickY != 0f) startTicking()

        // Analog-only triggers (some handhelds do not send L2/R2 key events).
        val right = maxOf(axis(MotionEvent.AXIS_RTRIGGER), axis(MotionEvent.AXIS_GAS))
        val tapDown = right > TRIGGER_THRESHOLD
        if (tapDown != triggerTapDown) setTap(tapDown)
        triggerTapDown = tapDown
    }

    private fun setTap(pressed: Boolean) {
        if (pressed == cursorPressed) return
        if (pressed && !cursorVisible) {
            // First press only reveals the cursor, so a stray press never taps blindly.
            cursorLastUsed = SystemClock.uptimeMillis()
            cursorListener?.onCursorChanged()
            return
        }
        cursorPressed = pressed
        cursorLastUsed = SystemClock.uptimeMillis()
        NativeLibrary.onTouchEvent(cursorX, cursorY, pressed)
        cursorListener?.onCursorChanged()
    }

    private fun startTicking() {
        if (ticking) return
        ticking = true
        lastFrameNanos = 0L
        Choreographer.getInstance().postFrameCallback(frameCallback)
    }

    private val frameCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            val dt = if (lastFrameNanos == 0L) 0f else (frameTimeNanos - lastFrameNanos) / 1e9f
            lastFrameNanos = frameTimeNanos
            val magnitude = hypot(stickX, stickY)
            if (magnitude == 0f) {
                ticking = false
                cursorListener?.onCursorChanged()
                return
            }
            val density = org.citra.citra_emu.CitraApplication.appContext.resources
                .displayMetrics.density
            val speed = CURSOR_SPEED_DP_PER_SECOND * density * magnitude // accelerate with tilt
            cursorX = (cursorX + stickX / magnitude * speed * dt)
                .coerceIn(0f, viewWidth.toFloat().coerceAtLeast(1f))
            cursorY = (cursorY + stickY / magnitude * speed * dt)
                .coerceIn(0f, viewHeight.toFloat().coerceAtLeast(1f))
            cursorLastUsed = SystemClock.uptimeMillis()
            if (cursorPressed) NativeLibrary.onTouchMoved(cursorX, cursorY)
            cursorListener?.onCursorChanged()
            Choreographer.getInstance().postFrameCallback(this)
        }
    }
}
