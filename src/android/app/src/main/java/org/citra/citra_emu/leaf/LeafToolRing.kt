// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

package org.citra.citra_emu.leaf

import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.features.settings.model.view.InputBindingSetting
import kotlin.math.atan2
import kotlin.math.hypot
import kotlin.math.roundToInt

/**
 * The tool ring (as in New Horizons): R2 shows the tools in the pockets around the player while
 * they stand still; the left stick (or the D-pad) points at one, A takes it out (or puts away the
 * one in hand), B or R2 closes the ring. The game gets no other input while the ring is open.
 *
 * The switch itself is done by the game (see Leaf::ToolRing); this only shows and picks.
 */
object LeafToolRing {
    data class Tool(val slot: Int, val kind: Int, val variant: Int, val icon: Int) {
        val inHand: Boolean get() = slot < 0
    }

    var listener: (() -> Unit)? = null

    /** Places around the ring (always drawn, empty ones too). */
    const val PLACES = 8

    /** Tools in the ring's places, clockwise from the top; empty while closed. */
    var tools: List<Tool> = emptyList()
        private set

    /** Selected place (0 until PLACES); it may be an empty one. */
    var selected = 0
        private set
    val isOpen: Boolean get() = tools.isNotEmpty()

    private val handler = Handler(Looper.getMainLooper())
    private var lastToggle = 0L
    private var triggerDown = false

    private const val STICK_SELECT = 0.5f
    private const val TRIGGER_THRESHOLD = 0.5f
    private const val TOGGLE_DEBOUNCE_MS = 200L // pads that send R2 as a key and as an axis

    private fun read(): List<Tool> {
        val raw = LeafNative.toolRing()
        return (0 until raw.size / 4).map { i ->
            Tool(raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], raw[i * 4 + 3])
        }
    }

    private fun toggle() {
        val now = SystemClock.uptimeMillis()
        if (now - lastToggle < TOGGLE_DEBOUNCE_MS) return
        lastToggle = now
        if (isOpen) close() else open()
    }

    private fun open() {
        val now = read()
        if (now.isEmpty()) return
        tools = now
        selected = 0
        LeafNative.setToolRingOpen(true)
        handler.post(poll)
        listener?.invoke()
    }

    fun close() {
        if (!isOpen) return
        tools = emptyList()
        LeafNative.setToolRingOpen(false)
        handler.removeCallbacks(poll)
        listener?.invoke()
    }

    /** Closes the ring when the player can no longer switch (an event, a visitor, ...). */
    private val poll = object : Runnable {
        override fun run() {
            val now = read()
            if (now.isEmpty()) {
                close()
                return
            }
            if (now != tools) {
                tools = now
                listener?.invoke()
            }
            handler.postDelayed(this, 100)
        }
    }

    private fun confirm() {
        val tool = tools.getOrNull(selected) ?: return
        LeafNative.selectTool(tool.slot)
        close()
    }

    private fun step(delta: Int) {
        selected = (selected + delta).mod(PLACES)
        listener?.invoke()
    }

    /** Returns true when the event was used (or swallowed) by the ring. */
    fun handleKeyEvent(event: KeyEvent): Boolean {
        val down = event.action == KeyEvent.ACTION_DOWN
        if (event.keyCode == KeyEvent.KEYCODE_BUTTON_R2) {
            if (down && event.repeatCount == 0) toggle()
            return true
        }
        if (!isOpen) return false
        if (!down || event.repeatCount != 0) return true
        when (event.keyCode) {
            KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_UP -> step(-1)
            KeyEvent.KEYCODE_DPAD_RIGHT, KeyEvent.KEYCODE_DPAD_DOWN -> step(1)
            KeyEvent.KEYCODE_BACK -> close()
            else -> {
                val buttons = InputBindingSetting.getButtonSet(event)
                when {
                    buttons.contains(NativeLibrary.ButtonType.BUTTON_A) -> confirm()
                    buttons.contains(NativeLibrary.ButtonType.BUTTON_B) -> close()
                }
            }
        }
        return true
    }

    /** Returns true when the game must not see the event (the ring is open). */
    fun onMotionEvent(event: MotionEvent): Boolean {
        if (event.source and InputDevice.SOURCE_CLASS_JOYSTICK == 0) return false
        val device = event.device ?: return isOpen
        fun axis(id: Int) =
            if (device.getMotionRange(id, event.source) != null) event.getAxisValue(id) else 0f

        // Analog-only triggers (some handhelds do not send R2 key events).
        val trigger = maxOf(axis(MotionEvent.AXIS_RTRIGGER), axis(MotionEvent.AXIS_GAS))
        val pressed = trigger > TRIGGER_THRESHOLD
        if (pressed && !triggerDown) toggle()
        triggerDown = pressed
        if (!isOpen) return false

        // Left stick (the game doesn't see it while the ring is open).
        val x = axis(MotionEvent.AXIS_X)
        val y = axis(MotionEvent.AXIS_Y)
        if (hypot(x, y) >= STICK_SELECT) {
            // Clockwise from the top, like the ring (y points down).
            val degrees = Math.toDegrees(atan2(y, x).toDouble()) + 90.0
            val index = (degrees / (360.0 / PLACES)).roundToInt().mod(PLACES)
            if (index != selected) {
                selected = index
                listener?.invoke()
            }
        }
        return true
    }

    /** English name of a tool (the game's own names). */
    fun name(tool: Tool): String {
        val kind = when (tool.kind) {
            0x0E -> listOf("rental axe", "axe", "silver axe", "golden axe")
            0x0F -> listOf("rental net", "net", "silver net", "golden net")
            0x10 -> listOf("rental rod", "fishing rod", "silver rod", "golden rod")
            0x11 -> listOf("rental shovel", "shovel", "silver shovel", "golden shovel")
            0x12 -> listOf("rental slingshot", "slingshot", "silver slingshot", "golden slingshot")
            0x13 -> listOf("rental can", "watering can", "silver watering can", "golden can")
            else -> listOf("megaphone")
        }
        return kind[tool.variant.coerceIn(0, kind.size - 1)].replaceFirstChar { it.uppercase() }
    }
}
