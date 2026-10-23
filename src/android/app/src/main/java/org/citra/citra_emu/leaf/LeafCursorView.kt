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
import org.citra.citra_emu.R

/**
 * Draws Leaf's overlays over the game: its own buttons, the keyboard's pointing hand and the tool
 * ring. It never handles touches itself.
 */
class LeafCursorView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {
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
        LeafKeyboardHand.listener = { postInvalidateOnAnimation() }
        LeafToolRing.listener = { postInvalidateOnAnimation() }
        post(keyboardPoll)
    }

    override fun onDetachedFromWindow() {
        LeafKeyboardHand.listener = null
        LeafToolRing.listener = null
        LeafToolRing.close()
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
        if (LeafToolRing.isOpen) drawToolRing(canvas)
    }

    // --- Tool ring ---------------------------------------------------------------------------

    private val icons = HashMap<Int, Bitmap?>()
    private val ringShade = Paint().apply { color = Color.argb(70, 0, 0, 0) }
    private val bubbleFill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.rgb(44, 178, 158) }
    private val bubbleText = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(255, 250, 230)
        textAlign = Paint.Align.CENTER
        typeface = android.graphics.Typeface.DEFAULT_BOLD
    }
    private val discRim = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        color = Color.rgb(44, 178, 158)
    }
    private val checkLine = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
        color = Color.WHITE
    }
    private val bubblePath = Path()

    private fun icon(index: Int): Bitmap? = icons.getOrPut(index) {
        LeafNative.getItemIcon(index)?.let { Bitmap.createBitmap(it, 32, 32, Bitmap.Config.ARGB_8888) }
    }

    /** The ring's places around the player, the selected one bigger with its name above it. */
    private fun drawToolRing(canvas: Canvas) {
        val tools = LeafToolRing.tools
        val h = height.toFloat()
        val cx = width / 2f
        val cy = h * 0.56f
        val radius = h * 0.22f
        val disc = h * 0.062f
        canvas.drawRect(0f, 0f, width.toFloat(), h, ringShade)
        val places = LeafToolRing.PLACES
        var selectedAt: Triple<Float, Float, Float>? = null
        for (i in 0 until places) {
            val angle = Math.toRadians(-90.0 + i * 360.0 / places)
            val x = cx + radius * Math.cos(angle).toFloat()
            val y = cy + radius * Math.sin(angle).toFloat()
            val tool = tools.getOrNull(i)
            val selected = i == LeafToolRing.selected
            val r = if (selected) disc * 1.2f else disc
            canvas.drawCircle(x + r * 0.05f, y + r * 0.09f, r, shadow)
            canvas.drawCircle(x, y, r, closeFill)
            if (selected) {
                discRim.strokeWidth = r * 0.09f
                canvas.drawCircle(x, y, r - discRim.strokeWidth / 2f, discRim)
                selectedAt = Triple(x, y, r)
            }
            if (tool == null) continue
            icon(tool.icon)?.let {
                val s = r * 1.5f
                canvas.drawBitmap(it, null, RectF(x - s / 2f, y - s / 2f, x + s / 2f, y + s / 2f),
                    handPaint)
            }
            if (tool.inHand) drawCheck(canvas, x - r * 0.68f, y - r * 0.68f, r * 0.3f)
        }
        val (sx, sy, sr) = selectedAt ?: return
        LeafToolRing.tools.getOrNull(LeafToolRing.selected)?.let { tool ->
            drawBubble(canvas, sx, sy - sr - h * 0.03f, LeafToolRing.name(tool),
                if (tool.inHand) context.getString(R.string.leaf_tool_ring_put_away) else null)
        }
        drawPointingHand(canvas, sx + sr * 0.4f, sy - sr * 0.05f, h * 0.11f)
    }

    /** The name bubble, its bottom edge at [bottom], with an optional hint line. */
    private fun drawBubble(canvas: Canvas, cx: Float, bottom: Float, text: String, hint: String?) {
        val h = height.toFloat()
        bubbleText.textSize = h * 0.05f
        val hintSize = h * 0.032f
        val lineH = bubbleText.textSize * 1.25f
        val boxH = lineH + (if (hint != null) hintSize * 1.3f else 0f) + h * 0.02f
        var boxW = bubbleText.measureText(text)
        if (hint != null) {
            bubbleText.textSize = hintSize
            boxW = maxOf(boxW, bubbleText.measureText(hint))
            bubbleText.textSize = h * 0.05f
        }
        boxW += h * 0.08f
        val left = (cx - boxW / 2f).coerceIn(4f, maxOf(4f, width - boxW - 4f))
        val rect = RectF(left, bottom - boxH, left + boxW, bottom)
        canvas.drawRoundRect(rect.left + 2f, rect.top + 4f, rect.right + 2f, rect.bottom + 4f,
            boxH / 2f, boxH / 2f, shadow)
        canvas.drawRoundRect(rect, boxH / 2f, boxH / 2f, bubbleFill)
        // A small tail pointing down at the ring.
        val t = h * 0.018f
        bubblePath.reset()
        bubblePath.moveTo(cx - t, bottom - 1f)
        bubblePath.lineTo(cx + t, bottom - 1f)
        bubblePath.lineTo(cx, bottom + t)
        bubblePath.close()
        canvas.drawPath(bubblePath, bubbleFill)
        val baseline = rect.top + h * 0.01f + bubbleText.textSize
        canvas.drawText(text, rect.centerX(), baseline, bubbleText)
        if (hint != null) {
            bubbleText.textSize = hintSize
            canvas.drawText(hint, rect.centerX(), baseline + hintSize * 1.35f, bubbleText)
        }
    }

    /** The teal check mark on the tool in hand. */
    private fun drawCheck(canvas: Canvas, x: Float, y: Float, r: Float) {
        canvas.drawCircle(x, y, r, bubbleFill)
        checkLine.strokeWidth = r * 0.28f
        bubblePath.reset()
        bubblePath.moveTo(x - r * 0.45f, y + r * 0.02f)
        bubblePath.lineTo(x - r * 0.1f, y + r * 0.38f)
        bubblePath.lineTo(x + r * 0.48f, y - r * 0.35f)
        canvas.drawPath(bubblePath, checkLine)
    }

    /** The game's pointing hand with its fingertip at ([x], [y]). */
    private fun drawPointingHand(canvas: Canvas, x: Float, y: Float, size: Float) {
        val bitmap = handBitmap() ?: return
        // Hotspot: the tip of the index finger at pixel (2.5, 26.5) of the 32x32 texture.
        val left = x - size * (2.5f / 32f)
        val top = y - size * (26.5f / 32f)
        canvas.drawBitmap(bitmap, null, RectF(left, top, left + size, top + size), handPaint)
    }

    private companion object {
        // Leaf::Ui::Icon
        const val ICON_CLOSE = 1
        const val ICON_EXPAND = 2
        const val ICON_COLLAPSE = 3
        const val ICON_ROUND_CORNERS = 4
    }
}
