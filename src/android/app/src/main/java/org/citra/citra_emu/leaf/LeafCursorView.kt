// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

package org.citra.citra_emu.leaf

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
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

    /** Leaf's own buttons (big map, closing a cat task's details): icon and view rectangle. */
    private data class Button(
        val icon: Int,
        val rect: RectF,
        val cornerRadius: Float,
        val cornerShade: Float
    )

    private var buttons: List<Button> = emptyList()
    private val closeFill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.rgb(255, 250, 235) }
    private val closeLine = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        color = Color.rgb(120, 86, 50)
    }

    /** Polls the keyboard scene and overlays (they are driven by the emulated game). */
    private val keyboardPoll = object : Runnable {
        override fun run() {
            val shown = LeafKeyboardHand.active()
            if (shown != keyboardShown) {
                keyboardShown = shown
                invalidate()
            }
            val raw = LeafNative.buttons()
            val now = (0 until raw.size / 7).map { i ->
                val v = i * 7
                Button(raw[v].toInt(), RectF(raw[v + 1], raw[v + 2], raw[v + 3], raw[v + 4]),
                    raw[v + 5], raw[v + 6])
            }
            if (now != buttons) {
                buttons = now
                invalidate()
            }
            postDelayed(this, if (buttons.isNotEmpty()) 50 else 120)
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

    private val cornerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.BLACK }
    private val cornerPath = Path().apply { fillType = Path.FillType.EVEN_ODD }

    /**
     * Dims the corners of a rectangular crop outside the rounded window it shows, like the
     * backdrop around it (the crop's corners would show the game undimmed).
     */
    private fun drawRoundCorners(canvas: Canvas, button: Button) {
        cornerPath.reset()
        cornerPath.addRect(button.rect, Path.Direction.CW)
        cornerPath.addRoundRect(button.rect, button.cornerRadius, button.cornerRadius,
            Path.Direction.CW)
        cornerPaint.alpha = (button.cornerShade * 255f).toInt().coerceIn(0, 255)
        canvas.drawPath(cornerPath, cornerPaint)
    }

    /** A round cream button in the game's colours: x (1), expand (2) or collapse (3). */
    private fun drawButton(canvas: Canvas, button: Button) {
        if (button.icon == ICON_ROUND_CORNERS) {
            drawRoundCorners(canvas, button)
            return
        }
        val rect = button.rect
        val r = minOf(rect.width(), rect.height()) / 2f
        val cx = rect.centerX()
        val cy = rect.centerY()
        canvas.drawCircle(cx + r * 0.06f, cy + r * 0.1f, r, shadow)
        canvas.drawCircle(cx, cy, r, closeFill)
        closeLine.strokeWidth = r * 0.12f
        canvas.drawCircle(cx, cy, r - closeLine.strokeWidth / 2f, closeLine)
        closeLine.strokeWidth = r * 0.16f
        val d = r * 0.38f
        when (button.icon) {
            ICON_CLOSE -> {
                canvas.drawLine(cx - d, cy - d, cx + d, cy + d, closeLine)
                canvas.drawLine(cx - d, cy + d, cx + d, cy - d, closeLine)
            }
            ICON_EXPAND, ICON_COLLAPSE -> {
                // Two corner arrows, pointing out (expand) or in (collapse).
                val k = d * 0.55f
                for (sx in listOf(-1f, 1f)) {
                    val tipX = cx + sx * d
                    val tipY = cy - sx * d
                    val baseX = cx + sx * d * 0.15f
                    val baseY = cy - sx * d * 0.15f
                    val (cornerX, cornerY, dir) =
                        if (button.icon == ICON_EXPAND) Triple(tipX, tipY, -sx)
                        else Triple(baseX, baseY, sx)
                    canvas.drawLine(baseX, baseY, tipX, tipY, closeLine)
                    canvas.drawLine(cornerX, cornerY, cornerX + dir * k, cornerY, closeLine)
                    canvas.drawLine(cornerX, cornerY, cornerX, cornerY - dir * k, closeLine)
                }
            }
        }
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        buttons.forEach { drawButton(canvas, it) }
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

    private companion object {
        // Leaf::Ui::Icon
        const val ICON_CLOSE = 1
        const val ICON_EXPAND = 2
        const val ICON_COLLAPSE = 3
        const val ICON_ROUND_CORNERS = 4
    }
}
