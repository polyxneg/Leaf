// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

package org.citra.citra_emu.leaf

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View

/**
 * Draws the gamepad cursor used to tap the bottom-screen elements placed on the main screen.
 * It never handles touches itself.
 */
class LeafCursorView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs), LeafSession.CursorListener {
    private val density = resources.displayMetrics.density
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE }
    private val outline = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2.5f * density
        color = Color.rgb(96, 64, 32)
    }
    private val shadow = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(70, 0, 0, 0) }

    init {
        isClickable = false
        // Holds focus so Android's touch mode never swallows a D-pad press (see EmulationFragment).
        isFocusable = true
        isFocusableInTouchMode = true
    }

    private var hand: Bitmap? = null
    private var handPressed: Bitmap? = null
    private var handLoaded = false
    private var keyboardShown = false
    private val handPaint = Paint(Paint.FILTER_BITMAP_FLAG)

    /** Polls whether the keyboard scene is up (it is driven by the emulated game). */
    private val keyboardPoll = object : Runnable {
        override fun run() {
            val shown = LeafKeyboardHand.active()
            if (shown != keyboardShown) {
                keyboardShown = shown
                invalidate()
            }
            postDelayed(this, 120)
        }
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        LeafSession.cursorListener = this
        LeafKeyboardHand.listener = { postInvalidateOnAnimation() }
        post(keyboardPoll)
    }

    override fun onDetachedFromWindow() {
        if (LeafSession.cursorListener === this) LeafSession.cursorListener = null
        LeafKeyboardHand.listener = null
        removeCallbacks(keyboardPoll)
        super.onDetachedFromWindow()
    }

    private fun handBitmap(): Bitmap? {
        if (!handLoaded) {
            handLoaded = true
            hand = LeafNative.getHandCursor(false)?.let {
                Bitmap.createBitmap(it, 32, 32, Bitmap.Config.ARGB_8888)
            }
            handPressed = LeafNative.getHandCursor(true)?.let {
                Bitmap.createBitmap(it, 32, 32, Bitmap.Config.ARGB_8888)
            }
        }
        return if (LeafKeyboardHand.pressed) handPressed ?: hand else hand
    }

    private fun drawKeyboardHand(canvas: Canvas) {
        val bitmap = handBitmap() ?: return
        val at = LeafNative.bottomToWindow(LeafKeyboardHand.x, LeafKeyboardHand.y) ?: return
        val next = LeafNative.bottomToWindow(LeafKeyboardHand.x + 10f, LeafKeyboardHand.y)
        val pxPerUnit = if (next != null) (next[0] - at[0]) / 10f else 4f
        val size = 32f * pxPerUnit
        // Hotspot: the tip of the index finger, pointing down-left, at pixel (2.5, 26.5) of the
        // 32x32 texture.
        val left = at[0] - size * (2.5f / 32f)
        val top = at[1] - size * (26.5f / 32f)
        canvas.drawBitmap(bitmap, null, RectF(left, top, left + size, top + size), handPaint)
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        LeafSession.onViewSize(w, h)
    }

    override fun onCursorChanged() {
        postInvalidateOnAnimation()
        // Repaint once more after the hide delay so the cursor disappears.
        removeCallbacks(hideCheck)
        postDelayed(hideCheck, 2600)
    }

    private val hideCheck = Runnable { invalidate() }

    override fun onTouchEvent(event: MotionEvent): Boolean = false

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        if (keyboardShown && LeafSettings.gamepadConnected(context)) {
            drawKeyboardHand(canvas)
            return
        }
        if (!LeafSession.cursorVisible) return
        val x = LeafSession.cursorX
        val y = LeafSession.cursorY
        val r = (if (LeafSession.cursorPressed) 9f else 11f) * density
        canvas.drawCircle(x + 2 * density, y + 3 * density, r, shadow)
        canvas.drawCircle(x, y, r, fill)
        canvas.drawCircle(x, y, r, outline)
        canvas.drawCircle(x, y, 2.5f * density, outline)
    }
}
