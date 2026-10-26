package com.pes2017.recomp

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.DashPathEffect
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.util.AttributeSet
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * The on-screen pad: what it looks like, what a finger on it means, and the
 * editor that lets the player move it about.
 *
 * Two jobs in one view because they are the same geometry seen twice. In
 * play it reads fingers and pushes a controller state at the runtime; in
 * edit it reads fingers and moves the controls themselves. Nothing here
 * talks to the guest directly -- it all goes through NativeBridge, which is
 * the only thing the runtime sees.
 */
class TouchOverlayView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : View(context, attrs) {

    private val prefs by lazy { Prefs(context) }

    var layout: ControlLayout = ControlLayout.defaults()
        set(value) {
            field = value
            releaseEverything()
            invalidate()
        }

    /** Told when the selection changes so the editor's panel can follow. */
    var onSelectionChanged: ((ControlElement?) -> Unit)? = null

    /** Told when a control was moved or resized, so the layout can be saved. */
    var onLayoutEdited: (() -> Unit)? = null

    var editing: Boolean = false
        set(value) {
            if (field == value) return
            field = value
            releaseEverything()
            selected = if (value) selected else null
            invalidate()
        }

    var selected: ControlElement? = null
        private set

    /** PlayStation names and symbols (Cross, Circle, L1, OPTIONS) instead of Xbox ones (A, B, LB, START). */
    var playStation: Boolean = true
        set(value) {
            if (field == value) return
            field = value
            invalidate()
        }

    /** Snapping makes a tidy row possible with a thumb; the editor offers it. */
    var snapToGrid: Boolean = false

    private val density = resources.displayMetrics.density

    // ---- what is being touched right now ----------------------------------
    // A pointer is claimed by one control for as long as it is down, so a
    // thumb that slides off the fire button keeps firing -- which is what
    // every phone shooter does, and what a player expects.
    private val claims = HashMap<Int, ControlElement>()
    private val stickPositions = HashMap<String, FloatArray>()   // id -> normalised x, y
    private val held = HashSet<String>()
    private val toggled = HashSet<String>()
    private var lookPointer = -1
    private var lookLastX = 0f
    private var lookLastY = 0f

    // ---- editing -----------------------------------------------------------
    private var dragPointer = -1
    private var dragElement: ControlElement? = null
    private var dragOffsetX = 0f
    private var dragOffsetY = 0f
    private var dragMoved = false
    private var pinchStartDistance = 0f
    private var pinchStartSize = 0f
    private var pinching = false

    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f * density
    }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textAlign = Paint.Align.CENTER
        isFakeBoldText = true
    }
    private val subText = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textAlign = Paint.Align.CENTER
        isFakeBoldText = true
    }
    private val highlightPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
    }
    private val notchPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
    }
    private val dimplePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
    }
    private val dashed = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f * density
        pathEffect = DashPathEffect(floatArrayOf(10f * density, 8f * density), 0f)
    }
    private val path = Path()
    private val box = RectF()
    private val tempRect = RectF()

    init {
        isFocusable = false
        isClickable = false
        setWillNotDraw(false)
    }

    // ---- geometry ----------------------------------------------------------

    private fun centreX(element: ControlElement) = element.xFrac * width
    private fun centreY(element: ControlElement) = element.yFrac * height
    private fun radius(element: ControlElement) = element.sizeDp * density * 0.5f

    /** The look area is a rectangle; everything else is a circle. */
    private fun rectOf(element: ControlElement, into: RectF) {
        val halfWidth = element.sizeDp * density * 0.5f
        val halfHeight = halfWidth * 0.62f
        into.set(
            centreX(element) - halfWidth, centreY(element) - halfHeight,
            centreX(element) + halfWidth, centreY(element) + halfHeight,
        )
    }

    private fun hit(element: ControlElement, x: Float, y: Float): Boolean {
        if (element.kind == ControlKind.LOOK) {
            rectOf(element, box)
            return box.contains(x, y)
        }
        // A touch a little outside still counts: glass has no edges to feel,
        // and a button that needs to be hit exactly is a button that gets
        // missed in a firefight.
        val slack = 6f * density
        return hypot(x - centreX(element), y - centreY(element)) <= radius(element) + slack
    }

    /** Buttons first, the look area last: it sits under everything. */
    private fun elementAt(x: Float, y: Float, includeHidden: Boolean = false): ControlElement? {
        for (element in layout.elements) {
            if (element.kind == ControlKind.LOOK) continue
            if (!element.visible && !includeHidden) continue
            if (hit(element, x, y)) return element
        }
        for (element in layout.elements) {
            if (element.kind != ControlKind.LOOK) continue
            if (!element.visible && !includeHidden) continue
            if (hit(element, x, y)) return element
        }
        return null
    }

    // ---- the state the guest sees -------------------------------------------

    private fun pushState() {
        var buttons = 0
        var leftTrigger = 0
        var rightTrigger = 0
        var leftX = 0f
        var leftY = 0f
        var rightX = 0f
        var rightY = 0f

        for (element in layout.elements) {
            if (!element.visible) continue
            val on = held.contains(element.id) || toggled.contains(element.id)
            when (element.kind) {
                ControlKind.BUTTON -> if (on) buttons = buttons or element.value
                ControlKind.TRIGGER -> if (on) {
                    if (element.value == 0) leftTrigger = 255 else rightTrigger = 255
                }
                ControlKind.STICK -> {
                    val position = stickPositions[element.id]
                    if (position != null) {
                        if (element.value == 0) {
                            leftX += position[0]
                            leftY += position[1]
                        } else {
                            rightX += position[0]
                            rightY += position[1]
                        }
                    }
                }
                ControlKind.LOOK -> Unit   // sent as deltas, not as a stick
            }
        }
        // A macro is playing: the fingers are not heard, so a run repeats exactly.
        if (Macro.playing) return
        val lx = leftX.coerceIn(-1f, 1f); val ly = leftY.coerceIn(-1f, 1f)
        val rx = rightX.coerceIn(-1f, 1f); val ry = rightY.coerceIn(-1f, 1f)
        Macro.record(Macro.TOUCH, buttons, leftTrigger, rightTrigger, lx, ly, rx, ry)
        NativeBridge.nativeTouchState(buttons, leftTrigger, rightTrigger, lx, ly, rx, ry)
    }

    /** Everything up: called when the pad is hidden, edited, or paused. */
    fun releaseEverything() {
        claims.clear()
        stickPositions.clear()
        held.clear()
        // Toggles survive a pause -- a player who was aiming down sights
        // before the phone rang is still aiming when they come back -- but
        // not the editor, which would otherwise leave a bit stuck down.
        if (editing) toggled.clear()
        lookPointer = -1
        if (NativeBridge.isLoaded()) {
            NativeBridge.nativeLookEnd()
            pushState()
        }
        invalidate()
    }

    // ---- touches --------------------------------------------------------------

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (editing) return editTouch(event)
        return playTouch(event)
    }

    private fun playTouch(event: MotionEvent): Boolean {
        var changed = false
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val index = event.actionIndex
                changed = press(event.getPointerId(index), event.getX(index), event.getY(index))
            }

            MotionEvent.ACTION_MOVE -> {
                for (index in 0 until event.pointerCount) {
                    val id = event.getPointerId(index)
                    val x = event.getX(index)
                    val y = event.getY(index)
                    if (id == lookPointer) {
                        // Every sample between frames matters for a smooth
                        // turn, so the batched history is used too.
                        for (h in 0 until event.historySize) {
                            sendLook(event.getHistoricalX(index, h), event.getHistoricalY(index, h))
                        }
                        sendLook(x, y)
                        continue
                    }
                    val element = claims[id] ?: continue
                    when (element.kind) {
                        ControlKind.STICK -> { moveStick(element, x, y); changed = true }
                        else -> Unit
                    }
                }
            }

            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                val index = event.actionIndex
                changed = release(event.getPointerId(index))
            }

            MotionEvent.ACTION_CANCEL -> {
                releaseEverything()
                return true
            }
        }
        if (changed) {
            pushState()
            invalidate()
        }
        return true
    }

    private fun triggerHaptic() {
        if (!prefs.vibration) return
        try {
            performHapticFeedback(
                HapticFeedbackConstants.VIRTUAL_KEY,
                HapticFeedbackConstants.FLAG_IGNORE_GLOBAL_SETTING,
            )
        } catch (_: Throwable) {}
    }

    private fun press(pointerId: Int, x: Float, y: Float): Boolean {
        val element = elementAt(x, y) ?: return false
        if (element.kind == ControlKind.LOOK) {
            lookPointer = pointerId
            lookLastX = x
            lookLastY = y
            return false
        }
        triggerHaptic()
        claims[pointerId] = element
        when (element.kind) {
            ControlKind.STICK -> moveStick(element, x, y)
            ControlKind.BUTTON, ControlKind.TRIGGER -> {
                if (element.toggle) {
                    if (!toggled.remove(element.id)) toggled.add(element.id)
                } else {
                    held.add(element.id)
                }
            }
            ControlKind.LOOK -> Unit
        }
        return true
    }

    private fun release(pointerId: Int): Boolean {
        if (pointerId == lookPointer) {
            lookPointer = -1
            NativeBridge.nativeLookEnd()
            return false
        }
        val element = claims.remove(pointerId) ?: return false
        when (element.kind) {
            ControlKind.STICK -> stickPositions.remove(element.id)
            else -> held.remove(element.id)
        }
        return true
    }

    private fun sendLook(x: Float, y: Float) {
        val dx = x - lookLastX
        val dy = y - lookLastY
        lookLastX = x
        lookLastY = y
        if (dx != 0f || dy != 0f) NativeBridge.nativeLookDelta(dx, dy)
    }

    private fun moveStick(element: ControlElement, x: Float, y: Float) {
        val r = radius(element)
        var dx = (x - centreX(element)) / r
        // The guest's Y points up; the screen's points down.
        var dy = -(y - centreY(element)) / r
        val length = hypot(dx, dy)
        if (length > 1f) { dx /= length; dy /= length }
        stickPositions[element.id] = floatArrayOf(dx, dy)
    }

    // ---- editing ----------------------------------------------------------------

    fun select(element: ControlElement?) {
        selected = element
        onSelectionChanged?.invoke(element)
        invalidate()
    }

    private fun editTouch(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val x = event.x
                val y = event.y
                // Hidden controls are still reachable in the editor: turning
                // one back on is impossible otherwise.
                val element = elementAt(x, y, includeHidden = true)
                if (element != null) {
                    select(element)
                    dragPointer = event.getPointerId(0)
                    dragElement = element
                    dragOffsetX = centreX(element) - x
                    dragOffsetY = centreY(element) - y
                    dragMoved = false
                } else {
                    select(null)
                    dragElement = null
                    dragPointer = -1
                }
            }

            MotionEvent.ACTION_POINTER_DOWN -> {
                // Two fingers on the selected control resize it.
                val element = dragElement ?: selected
                if (element != null && event.pointerCount == 2) {
                    pinching = true
                    pinchStartDistance = max(1f, spread(event))
                    pinchStartSize = element.sizeDp
                }
            }

            MotionEvent.ACTION_MOVE -> {
                val element = dragElement ?: selected ?: return true
                if (pinching && event.pointerCount >= 2) {
                    val scale = spread(event) / pinchStartDistance
                    element.sizeDp = (pinchStartSize * scale)
                        .coerceIn(minimumSize(element), maximumSize(element))
                    dragMoved = true
                    onSelectionChanged?.invoke(element)
                    invalidate()
                    return true
                }
                val index = event.findPointerIndex(dragPointer)
                if (index < 0) return true
                var x = event.getX(index) + dragOffsetX
                var y = event.getY(index) + dragOffsetY
                if (snapToGrid) {
                    val step = 24f * density
                    x = (x / step).roundToInt() * step
                    y = (y / step).roundToInt() * step
                }
                // Kept on screen, with a margin so nothing hides under the
                // gesture bar or a punch-hole camera.
                val margin = 8f * density
                element.xFrac = (x.coerceIn(margin, width - margin)) / max(1, width)
                element.yFrac = (y.coerceIn(margin, height - margin)) / max(1, height)
                dragMoved = true
                invalidate()
            }

            MotionEvent.ACTION_POINTER_UP -> {
                if (event.pointerCount <= 2) pinching = false
            }

            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                if (dragMoved) {
                    // Dropped on (or grown into) a neighbour: step it aside.
                    (dragElement ?: selected)?.let { keepApart(it) }
                    onLayoutEdited?.invoke()
                }
                dragPointer = -1
                dragElement = null
                pinching = false
                dragMoved = false
            }
        }
        return true
    }

    /**
     * Moves [element] off any shown control it covers, so two controls never
     * share a touch: each pass pushes it straight away from the nearest overlap
     * until there is a small gap (more than the touch slack in [hit]), keeping it
     * on screen. A crowded spot can leave it touching after the last pass; that
     * is rare, and the player can still move it by hand.
     */
    fun keepApart(element: ControlElement) {
        if (width == 0 || height == 0 || element.kind == ControlKind.LOOK) return
        val gap = 14f * density
        val margin = 8f * density
        repeat(24) {
            var moved = false
            for (other in layout.elements) {
                if (other === element || !other.visible || other.kind == ControlKind.LOOK) continue
                val dx = centreX(element) - centreX(other)
                val dy = centreY(element) - centreY(other)
                val distance = hypot(dx, dy)
                val needed = radius(element) + radius(other) + gap
                if (distance >= needed) continue
                // Exactly on top: push towards the screen's middle, which has room.
                val ux = if (distance > 0.5f) dx / distance else if (centreX(element) > width / 2f) -1f else 1f
                val uy = if (distance > 0.5f) dy / distance else 0f
                val push = needed - distance
                val r = radius(element) + margin
                val x = (centreX(element) + ux * push).coerceIn(r, max(r, width - r))
                val y = (centreY(element) + uy * push).coerceIn(r, max(r, height - r))
                element.xFrac = x / width
                element.yFrac = y / height
                moved = true
            }
            if (!moved) {
                invalidate()
                return
            }
        }
        invalidate()
    }

    private fun minimumSize(element: ControlElement): Float =
        if (element.kind == ControlKind.LOOK) 120f else 36f

    private fun maximumSize(element: ControlElement): Float =
        if (element.kind == ControlKind.LOOK) 900f else 200f

    private fun spread(event: MotionEvent): Float {
        if (event.pointerCount < 2) return 1f
        return hypot(event.getX(0) - event.getX(1), event.getY(0) - event.getY(1))
    }

    // ---- drawing ------------------------------------------------------------------

    override fun onDraw(canvas: Canvas) {
        for (element in layout.elements) {
            if (!element.visible && !editing) continue
            val faded = editing && !element.visible
            val alpha = if (editing) max(0.28f, element.alpha) else element.alpha
            when (element.kind) {
                ControlKind.LOOK -> drawLook(canvas, element, faded)
                ControlKind.STICK -> drawStick(canvas, element, alpha, faded)
                ControlKind.BUTTON, ControlKind.TRIGGER -> drawButton(canvas, element, alpha, faded)
            }
            if (editing && element === selected) drawSelection(canvas, element)
        }
    }

    private fun shade(alpha: Float, pressed: Boolean, faded: Boolean): Int {
        val a = (alpha * (if (faded) 0.45f else 1f) * 255).toInt().coerceIn(0, 255)
        return if (pressed) Color.argb(min(255, a + 70), 255, 255, 255)
        else Color.argb(a, 235, 238, 242)
    }

    private fun isOn(element: ControlElement): Boolean =
        held.contains(element.id) || toggled.contains(element.id)

    companion object {
        // The Xbox 360 pad colours, because the game's own prompts use them
        private const val COLOR_A = 0xFF107C10.toInt()      // Emerald Green
        private const val COLOR_A_GLOW = 0xFF22C55E.toInt()
        private const val COLOR_B = 0xFFB91C1C.toInt()      // Ruby Red
        private const val COLOR_B_GLOW = 0xFFEF4444.toInt()
        private const val COLOR_X = 0xFF1D4ED8.toInt()      // Royal Blue
        private const val COLOR_X_GLOW = 0xFF3B82F6.toInt()
        private const val COLOR_Y = 0xFFD97706.toInt()      // Amber Yellow
        private const val COLOR_Y_GLOW = 0xFFF59E0B.toInt()
        private const val COLOR_RT = 0xFFE11D48.toInt()     // Right trigger
        private const val COLOR_RT_GLOW = 0xFFFB7185.toInt()
        private const val COLOR_LT = 0xFF0EA5E9.toInt()     // Left trigger
        private const val COLOR_LT_GLOW = 0xFF38BDF8.toInt()
        private const val COLOR_BUMPER_GLOW = 0xFF94A3B8.toInt()

        // The PlayStation symbol colours: blue cross, red circle, pink square, green triangle
        private const val PS_CROSS = 0xFF3B5BA9.toInt()
        private const val PS_CROSS_GLOW = 0xFF7DA2F0.toInt()
        private const val PS_CIRCLE = 0xFFB4232F.toInt()
        private const val PS_CIRCLE_GLOW = 0xFFF0626C.toInt()
        private const val PS_SQUARE = 0xFF9D3F8C.toInt()
        private const val PS_SQUARE_GLOW = 0xFFE58FD2.toInt()
        private const val PS_TRIANGLE = 0xFF1E8C6B.toInt()
        private const val PS_TRIANGLE_GLOW = 0xFF4FD1A5.toInt()
    }

    private fun drawButton(canvas: Canvas, element: ControlElement, alpha: Float, faded: Boolean) {
        val r = radius(element)
        val cx = centreX(element)
        val cy = centreY(element)
        val pressed = isOn(element)
        val fadeFactor = if (faded) 0.45f else 1f

        val id = element.id.lowercase()
        val isA = id == "a"
        val isB = id == "b"
        val isX = id == "x"
        val isY = id == "y"
        val isFaceButton = isA || isB || isX || isY
        val isRT = id == "rt"
        val isLT = id == "lt"
        val isBumper = id == "lb" || id == "rb"
        val isThumbClick = id == "l3" || id == "r3"
        val isMenu = id == "start" || id == "back"

        val label = ControlLayout.labelFor(element, playStation)
        val coreColor = when {
            isA -> if (playStation) PS_CROSS else COLOR_A
            isB -> if (playStation) PS_CIRCLE else COLOR_B
            isX -> if (playStation) PS_SQUARE else COLOR_X
            isY -> if (playStation) PS_TRIANGLE else COLOR_Y
            isRT -> COLOR_RT
            isLT -> COLOR_LT
            isBumper -> 0xFF334155.toInt()
            else -> 0xFF1E242C.toInt()
        }

        val glowColor = when {
            isA -> if (playStation) PS_CROSS_GLOW else COLOR_A_GLOW
            isB -> if (playStation) PS_CIRCLE_GLOW else COLOR_B_GLOW
            isX -> if (playStation) PS_SQUARE_GLOW else COLOR_X_GLOW
            isY -> if (playStation) PS_TRIANGLE_GLOW else COLOR_Y_GLOW
            isRT -> COLOR_RT_GLOW
            isLT -> COLOR_LT_GLOW
            isBumper -> COLOR_BUMPER_GLOW
            else -> 0xFF94A3B8.toInt()
        }

        // Pressed radiant aura glow
        if (pressed) {
            fill.color = Color.argb(
                (alpha * fadeFactor * 130).toInt().coerceIn(0, 255),
                Color.red(glowColor), Color.green(glowColor), Color.blue(glowColor),
            )
            canvas.drawCircle(cx, cy, r * 1.15f, fill)
        }

        if (isFaceButton) {
            // Xbox 360 Jewel Face Button
            // 1. Dark bezel collar
            fill.color = Color.argb(
                (alpha * fadeFactor * 140).toInt().coerceIn(0, 255),
                18, 22, 28,
            )
            canvas.drawCircle(cx, cy, r, fill)

            // Bezel rim stroke
            stroke.color = Color.argb(
                (alpha * fadeFactor * 180).toInt().coerceIn(0, 255),
                48, 56, 70,
            )
            stroke.strokeWidth = max(1.5f, r * 0.05f)
            canvas.drawCircle(cx, cy, r - stroke.strokeWidth * 0.5f, stroke)

            // 2. Translucent jewel dome
            val jewelR = r * 0.84f
            val domeAlpha = if (pressed) 230 else 125
            fill.color = Color.argb(
                (alpha * fadeFactor * domeAlpha).toInt().coerceIn(0, 255),
                Color.red(coreColor), Color.green(coreColor), Color.blue(coreColor),
            )
            canvas.drawCircle(cx, cy, jewelR, fill)

            // Jewel perimeter rim
            stroke.color = Color.argb(
                (alpha * fadeFactor * (if (pressed) 255 else 180)).toInt().coerceIn(0, 255),
                Color.red(glowColor), Color.green(glowColor), Color.blue(glowColor),
            )
            stroke.strokeWidth = max(1.5f, jewelR * 0.08f)
            canvas.drawCircle(cx, cy, jewelR - stroke.strokeWidth * 0.5f, stroke)

            // 3. Specular gloss arc highlight (top curve of the acrylic dome)
            highlightPaint.color = Color.argb(
                (alpha * fadeFactor * 150).toInt().coerceIn(0, 255),
                255, 255, 255,
            )
            highlightPaint.strokeWidth = max(1.5f, jewelR * 0.07f)
            val arcInset = jewelR * 0.22f
            tempRect.set(cx - jewelR + arcInset, cy - jewelR + arcInset * 0.6f, cx + jewelR - arcInset, cy + jewelR - arcInset * 1.4f)
            canvas.drawArc(tempRect, -145f, 110f, false, highlightPaint)

            // 4. Just the pad's own name, as on a real controller: the
            //    PlayStation symbol, or the Xbox letter.
            val ink = Color.argb((alpha * fadeFactor * 255).toInt().coerceIn(0, 255), 255, 255, 255)
            val shadow = Color.argb((alpha * fadeFactor * 140).toInt().coerceIn(0, 255), 0, 0, 0)
            if (playStation) {
                val size = jewelR * 0.40f
                drawPsSymbol(canvas, id, cx, cy + 1.2f * density, size, shadow)
                drawPsSymbol(canvas, id, cx, cy, size, ink)
            } else {
                text.textSize = jewelR * 0.95f
                val letterY = cy - (text.descent() + text.ascent()) * 0.5f
                text.color = shadow
                canvas.drawText(label, cx, letterY + 1.2f * density, text)
                text.color = ink
                canvas.drawText(label, cx, letterY, text)
            }
        } else if (isRT || isLT) {
            // Xbox Ergonomic Trigger (RT / LT)
            val fillA = (alpha * fadeFactor * (if (pressed) 190 else 95)).toInt().coerceIn(0, 255)
            fill.color = Color.argb(fillA, Color.red(coreColor), Color.green(coreColor), Color.blue(coreColor))
            canvas.drawCircle(cx, cy, r, fill)

            stroke.color = Color.argb(
                (alpha * fadeFactor * (if (pressed) 255 else 170)).toInt().coerceIn(0, 255),
                Color.red(glowColor), Color.green(glowColor), Color.blue(glowColor),
            )
            stroke.strokeWidth = max(2f, r * 0.065f)
            canvas.drawCircle(cx, cy, r - stroke.strokeWidth * 0.5f, stroke)

            text.textSize = r * 0.58f
            text.color = Color.WHITE
            canvas.drawText(label, cx, cy - (text.descent() + text.ascent()) * 0.5f, text)

            if (element.toggle && toggled.contains(element.id)) {
                fill.color = Color.argb(255, 56, 189, 248)
                canvas.drawCircle(cx, cy - r * 0.55f, 3.5f * density, fill)
            }
        } else if (isBumper) {
            // Xbox Bumper (LB / RB)
            val fillA = (alpha * fadeFactor * (if (pressed) 180 else 85)).toInt().coerceIn(0, 255)
            fill.color = Color.argb(fillA, 30, 38, 48)
            canvas.drawCircle(cx, cy, r, fill)

            stroke.color = Color.argb(
                (alpha * fadeFactor * (if (pressed) 240 else 140)).toInt().coerceIn(0, 255),
                Color.red(glowColor), Color.green(glowColor), Color.blue(glowColor),
            )
            stroke.strokeWidth = max(1.5f, r * 0.055f)
            canvas.drawCircle(cx, cy, r - stroke.strokeWidth * 0.5f, stroke)

            text.textSize = r * (if (element.sub.isEmpty()) 0.52f else 0.46f)
            text.color = Color.WHITE
            val labelY = if (element.sub.isEmpty()) cy - (text.descent() + text.ascent()) * 0.5f else cy - r * 0.06f
            canvas.drawText(label, cx, labelY, text)
            if (element.sub.isNotEmpty()) {
                subText.textSize = r * 0.27f
                subText.color = Color.argb((alpha * fadeFactor * 220).toInt().coerceIn(0, 255), 226, 232, 240)
                canvas.drawText(element.sub, cx, cy + r * 0.44f, subText)
            }
        } else if (isThumbClick) {
            // Thumbstick Click (L3 / R3)
            val fillA = (alpha * fadeFactor * (if (pressed) 180 else 80)).toInt().coerceIn(0, 255)
            fill.color = Color.argb(fillA, 24, 30, 38)
            canvas.drawCircle(cx, cy, r, fill)

            stroke.color = Color.argb(
                (alpha * fadeFactor * (if (pressed) 230 else 130)).toInt().coerceIn(0, 255),
                100, 116, 139,
            )
            stroke.strokeWidth = max(1.5f, r * 0.05f)
            canvas.drawCircle(cx, cy, r - stroke.strokeWidth * 0.5f, stroke)

            val tag = element.label

            text.textSize = r * 0.50f
            text.color = Color.WHITE
            canvas.drawText(tag, cx, cy - (text.descent() + text.ascent()) * 0.5f, text)
        } else if (isMenu) {
            // Start (▶) / Back (◀◀)
            val fillA = (alpha * fadeFactor * (if (pressed) 180 else 75)).toInt().coerceIn(0, 255)
            fill.color = Color.argb(fillA, 22, 27, 34)
            canvas.drawCircle(cx, cy, r, fill)

            stroke.color = Color.argb(
                (alpha * fadeFactor * (if (pressed) 230 else 120)).toInt().coerceIn(0, 255),
                100, 116, 139,
            )
            stroke.strokeWidth = max(1.5f, r * 0.045f)
            canvas.drawCircle(cx, cy, r - stroke.strokeWidth * 0.5f, stroke)

            text.textSize = r * (if (label.length > 5) 0.26f else 0.32f)
            text.color = Color.WHITE
            canvas.drawText(label, cx, cy - (text.descent() + text.ascent()) * (if (element.sub.isEmpty()) 0.5f else 1.1f), text)
            if (element.sub.isNotEmpty()) {
                subText.textSize = r * 0.24f
                subText.color = Color.argb((alpha * fadeFactor * 200).toInt().coerceIn(0, 255), 148, 163, 184)
                canvas.drawText(element.sub, cx, cy + r * 0.45f, subText)
            }
        } else {
            // Fallback general button
            fill.color = Color.argb((alpha * fadeFactor * (if (pressed) 160 else 75)).toInt().coerceIn(0, 255), 18, 22, 28)
            canvas.drawCircle(cx, cy, r, fill)
            stroke.color = shade(alpha, pressed, faded)
            stroke.strokeWidth = max(1.5f, r * 0.055f)
            canvas.drawCircle(cx, cy, r - stroke.strokeWidth * 0.5f, stroke)
            text.color = shade(alpha, pressed, faded)
            text.textSize = r * (if (element.label.length > 2) 0.52f else 0.8f)
            canvas.drawText(element.label, cx, cy - (text.descent() + text.ascent()) * 0.5f, text)
        }
    }

    /** The PlayStation face symbol for a face button's id, drawn as lines so no font is needed. */
    private fun drawPsSymbol(canvas: Canvas, id: String, cx: Float, cy: Float, size: Float, color: Int) {
        highlightPaint.color = color
        highlightPaint.strokeWidth = max(1.5f, size * 0.32f)
        when (id) {
            "a" -> {
                canvas.drawLine(cx - size, cy - size, cx + size, cy + size, highlightPaint)
                canvas.drawLine(cx - size, cy + size, cx + size, cy - size, highlightPaint)
            }
            "b" -> canvas.drawCircle(cx, cy, size, highlightPaint)
            "x" -> canvas.drawRect(cx - size * 0.9f, cy - size * 0.9f, cx + size * 0.9f, cy + size * 0.9f, highlightPaint)
            "y" -> {
                path.reset()
                path.moveTo(cx, cy - size * 1.05f)
                path.lineTo(cx + size * 1.1f, cy + size * 0.85f)
                path.lineTo(cx - size * 1.1f, cy + size * 0.85f)
                path.close()
                canvas.drawPath(path, highlightPaint)
            }
        }
    }

    private fun drawStick(canvas: Canvas, element: ControlElement, alpha: Float, faded: Boolean) {
        val r = radius(element)
        val cx = centreX(element)
        val cy = centreY(element)
        val fadeFactor = if (faded) 0.45f else 1f
        val position = stickPositions[element.id]
        val isDeflected = position != null

        // 1. Outer socket base (metallic dish)
        fill.color = Color.argb((alpha * fadeFactor * 75).toInt().coerceIn(0, 255), 16, 20, 26)
        canvas.drawCircle(cx, cy, r, fill)

        // Outer socket rim stroke
        stroke.color = Color.argb((alpha * fadeFactor * 130).toInt().coerceIn(0, 255), 45, 55, 70)
        stroke.strokeWidth = max(1.5f, r * 0.035f)
        canvas.drawCircle(cx, cy, r - stroke.strokeWidth * 0.5f, stroke)

        // 4 Cardinal Guide Notches on the outer socket perimeter (Xbox stick housing style)
        val notchLen = r * 0.10f
        notchPaint.color = Color.argb((alpha * fadeFactor * 160).toInt().coerceIn(0, 255), 70, 85, 105)
        notchPaint.strokeWidth = max(1.5f, 2f * density)
        // Top
        canvas.drawLine(cx, cy - r, cx, cy - r + notchLen, notchPaint)
        // Bottom
        canvas.drawLine(cx, cy + r - notchLen, cx, cy + r, notchPaint)
        // Left
        canvas.drawLine(cx - r, cy, cx - r + notchLen, cy, notchPaint)
        // Right
        canvas.drawLine(cx + r - notchLen, cy, cx + r, cy, notchPaint)

        // 2. Knob position
        val knobX = cx + (position?.get(0) ?: 0f) * r * 0.62f
        val knobY = cy - (position?.get(1) ?: 0f) * r * 0.62f

        // Directional deflection vector glow
        if (isDeflected) {
            val stickDx = knobX - cx
            val stickDy = knobY - cy
            val dist = hypot(stickDx, stickDy)
            if (dist > 4f * density) {
                notchPaint.color = Color.argb((alpha * fadeFactor * 100).toInt().coerceIn(0, 255), 16, 124, 16)
                notchPaint.strokeWidth = max(2f, 3f * density)
                canvas.drawLine(cx, cy, knobX, knobY, notchPaint)
            }
        }

        // 3. Xbox 360 Thumbstick Cap / Knob
        val knobR = r * 0.38f

        // Outer rubber traction ring
        fill.color = Color.argb((alpha * fadeFactor * (if (isDeflected) 240 else 210)).toInt().coerceIn(0, 255), 36, 42, 52)
        canvas.drawCircle(knobX, knobY, knobR, fill)

        // Raised outer rim stroke
        stroke.color = Color.argb((alpha * fadeFactor * (if (isDeflected) 220 else 160)).toInt().coerceIn(0, 255), 68, 78, 94)
        stroke.strokeWidth = max(1.5f, knobR * 0.08f)
        canvas.drawCircle(knobX, knobY, knobR - stroke.strokeWidth * 0.5f, stroke)

        // Inner concave depression bowl
        val dishR = knobR * 0.72f
        fill.color = Color.argb((alpha * fadeFactor * 240).toInt().coerceIn(0, 255), 20, 24, 30)
        canvas.drawCircle(knobX, knobY, dishR, fill)

        // Inner dish bevel
        stroke.color = Color.argb((alpha * fadeFactor * 100).toInt().coerceIn(0, 255), 12, 14, 18)
        stroke.strokeWidth = max(1f, 1.5f * density)
        canvas.drawCircle(knobX, knobY, dishR, stroke)

        // 4. Center label when idle ("LS" or "RS")
        if (!isDeflected) {
            subText.textSize = dishR * 0.34f
            subText.color = Color.argb((alpha * fadeFactor * 130).toInt().coerceIn(0, 255), 148, 163, 184)
            canvas.drawText(ControlLayout.labelFor(element, playStation), knobX, knobY - (subText.descent() + subText.ascent()) * 0.5f, subText)
        }

        // 5. The 4 Signature Xbox Rubber Grip Dimples/Dots (Top, Bottom, Left, Right)
        val dotOffset = knobR * 0.44f
        val dotRadius = max(2f, 2.8f * density)

        dimplePaint.style = Paint.Style.FILL
        dimplePaint.color = Color.argb((alpha * fadeFactor * (if (isDeflected) 230 else 170)).toInt().coerceIn(0, 255), 88, 100, 118)

        // Top dot
        canvas.drawCircle(knobX, knobY - dotOffset, dotRadius, dimplePaint)
        // Bottom dot
        canvas.drawCircle(knobX, knobY + dotOffset, dotRadius, dimplePaint)
        // Left dot
        canvas.drawCircle(knobX - dotOffset, knobY, dotRadius, dimplePaint)
        // Right dot
        canvas.drawCircle(knobX + dotOffset, knobY, dotRadius, dimplePaint)
    }

    private fun drawLook(canvas: Canvas, element: ControlElement, faded: Boolean) {
        // Invisible while playing: it is a region, not a control.
        if (!editing) return
        rectOf(element, box)
        dashed.color = if (faded) Color.argb(90, 120, 170, 255) else Color.argb(170, 120, 170, 255)
        canvas.drawRoundRect(box, 14f * density, 14f * density, dashed)
        text.color = dashed.color
        text.textSize = 15f * density
        canvas.drawText(element.label, box.centerX(), box.centerY(), text)
    }

    private fun drawSelection(canvas: Canvas, element: ControlElement) {
        stroke.color = Color.argb(230, 120, 200, 120)
        stroke.strokeWidth = 2.5f * density
        if (element.kind == ControlKind.LOOK) {
            rectOf(element, box)
            box.inset(-6f * density, -6f * density)
            canvas.drawRoundRect(box, 16f * density, 16f * density, stroke)
        } else {
            canvas.drawCircle(centreX(element), centreY(element), radius(element) + 7f * density, stroke)
        }
    }

    /** Used by the editor's panel to keep the drawing in step with a slider. */
    fun refresh() = invalidate()
}
