// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

package org.citra.citra_emu.leaf

import android.os.Handler
import android.os.Looper
import android.view.KeyEvent
import android.view.MotionEvent
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.features.settings.model.view.InputBindingSetting

/**
 * D-pad navigation for the game's on-screen keyboard (names, letters, signs). The keyboard is
 * touch-only on a 3DS; here a pointing hand walks its keys with the D-pad, A presses the key
 * under it, B is backspace and Start confirms. Taps go straight to the 3DS touchscreen.
 */
object LeafKeyboardHand {
    // Key rows of the QWERTY page in bottom-screen pixels.
    private val rowY = floatArrayOf(79f, 101f, 122.5f, 144.5f, 166f, 192.5f, 221f)
    // Rows whose keys are not on the regular 25px grid.
    private val rowStops = mapOf(
        4 to floatArrayOf(29f, 85f, 162.5f), // case, shift, space
        5 to floatArrayOf(44f, 122.5f, 199f, 275f), // page tabs
        6 to floatArrayOf(259f) // Confirm
    )
    private const val GRID = 25f
    private const val MIN_X = 15f
    private const val MAX_X = 302f
    private val backspace = floatArrayOf(296f, 79f)
    private val confirm = floatArrayOf(259f, 221f)

    var x = 22.5f
        private set
    var row = 2
        private set
    val y: Float get() = rowY[row]

    var listener: (() -> Unit)? = null

    /** True briefly after A, to draw the hand pressing the key. */
    var pressed = false
        private set
    private val handler = Handler(Looper.getMainLooper())
    private var hatX = 0
    private var hatY = 0
    private var gridX = 22.5f // remembered column while visiting the special rows

    fun active(): Boolean = LeafNative.sceneVisible("keyboard")

    /** Returns true when the event was used for keyboard navigation. */
    fun handleKeyEvent(event: KeyEvent): Boolean {
        if (!active()) return false
        val down = event.action == KeyEvent.ACTION_DOWN
        when (event.keyCode) {
            KeyEvent.KEYCODE_DPAD_LEFT -> { if (down) move(-1, 0); return true }
            KeyEvent.KEYCODE_DPAD_RIGHT -> { if (down) move(1, 0); return true }
            KeyEvent.KEYCODE_DPAD_UP -> { if (down) move(0, -1); return true }
            KeyEvent.KEYCODE_DPAD_DOWN -> { if (down) move(0, 1); return true }
        }
        val buttons = InputBindingSetting.getButtonSet(event)
        val target = when {
            buttons.contains(NativeLibrary.ButtonType.BUTTON_A) -> floatArrayOf(x, y)
            buttons.contains(NativeLibrary.ButtonType.BUTTON_B) -> backspace
            buttons.contains(NativeLibrary.ButtonType.BUTTON_START) -> confirm
            else -> return false
        }
        if (down && event.repeatCount == 0) tap(target[0], target[1])
        return true
    }

    /** D-pads that report as a hat axis. Never consumes the event. */
    fun onMotionEvent(event: MotionEvent) {
        if (!active()) return
        val hx = Math.round(event.getAxisValue(MotionEvent.AXIS_HAT_X))
        val hy = Math.round(event.getAxisValue(MotionEvent.AXIS_HAT_Y))
        if (hx != hatX && hx != 0) move(hx, 0)
        if (hy != hatY && hy != 0) move(0, hy)
        hatX = hx
        hatY = hy
    }

    private fun move(dx: Int, dy: Int) {
        if (dy != 0) {
            val wasGrid = rowStops[row] == null
            if (wasGrid) gridX = x
            row = (row + dy).coerceIn(0, rowY.size - 1)
            x = rowStops[row]?.minByOrNull { kotlin.math.abs(it - gridX) } ?: gridX
        }
        if (dx != 0) {
            val stops = rowStops[row]
            if (stops == null) {
                x = (x + dx * GRID).coerceIn(MIN_X, MAX_X)
                gridX = x
            } else {
                val i = stops.indexOfFirst { it == x }.coerceAtLeast(0)
                x = stops[(i + dx).coerceIn(0, stops.size - 1)]
            }
        }
        listener?.invoke()
    }

    private fun tap(tx: Float, ty: Float) {
        LeafNative.touchNormalized(tx / 320f, ty / 240f, true)
        if (tx == x && ty == y) pressed = true
        handler.postDelayed({ LeafNative.touchNormalized(tx / 320f, ty / 240f, false) }, 90)
        handler.postDelayed({
            pressed = false
            listener?.invoke()
        }, 160)
        listener?.invoke()
    }
}
