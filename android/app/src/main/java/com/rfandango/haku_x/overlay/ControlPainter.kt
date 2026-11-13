package com.rfandango.haku_x.overlay

import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RadialGradient
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.Typeface
import kotlin.math.hypot

/**
 * Draws the controls the way the original Xbox controller looks: face buttons
 * as glossy coloured jewels with the letter set in, white and black jewels,
 * concave dark thumbsticks with their cross grooves, the black D-pad disc with
 * its raised cross, the small black back and start buttons, and the jewel with
 * the green X in the middle.
 */
class ControlPainter {
  private val p = Paint(Paint.ANTI_ALIAS_FLAG)
  private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
    textAlign = Paint.Align.CENTER
    typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
  }
  private val rect = RectF()
  private val path = Path()
  private val bar = Path()

  private class Jewel(val light: Int, val base: Int, val dark: Int)

  private val jewels = mapOf(
    ControlId.A to Jewel(0xFF9BE07F.toInt(), 0xFF3E9F32.toInt(), 0xFF164D12.toInt()),
    ControlId.B to Jewel(0xFFFF8A7A.toInt(), 0xFFD12A22.toInt(), 0xFF5E0C08.toInt()),
    ControlId.X to Jewel(0xFF8CC4FF.toInt(), 0xFF1F6FD0.toInt(), 0xFF0A2F66.toInt()),
    ControlId.Y to Jewel(0xFFFFEE8F.toInt(), 0xFFF2BE12.toInt(), 0xFF7A5A00.toInt()),
    ControlId.WHITE to Jewel(0xFFFFFFFF.toInt(), 0xFFDADADA.toInt(), 0xFF8A8A8A.toInt()),
    ControlId.BLACK to Jewel(0xFF6A6A6A.toInt(), 0xFF262626.toInt(), 0xFF000000.toInt()),
  )
  private val clickJewel = Jewel(0xFF6E6E6E.toInt(), 0xFF2E2E2E.toInt(), 0xFF0C0C0C.toInt())

  /** The custom buttons' own colours, none of them the face buttons'. */
  private val customJewels = mapOf(
    ControlId.TOUCH_LOOK to Jewel(0xFF9FD8F5.toInt(), 0xFF2E86C1.toInt(), 0xFF0F3D63.toInt()),
    ControlId.CUSTOM_0 to Jewel(0xFFD7B5F0.toInt(), 0xFF8E44AD.toInt(), 0xFF3F1558.toInt()),
    ControlId.CUSTOM_1 to Jewel(0xFFFFC27A.toInt(), 0xFFE67E22.toInt(), 0xFF6B3300.toInt()),
    ControlId.CUSTOM_2 to Jewel(0xFF9FE8D8.toInt(), 0xFF16A085.toInt(), 0xFF05433A.toInt()),
  )

  /**
   * One control. pressed: held; kx, ky: a stick's deflection (-1..1);
   * dpad: the D-pad's held directions (DPAD_* bits); lit: a switch that is on.
   */
  fun draw(
    c: Canvas,
    s: ControlSpec,
    pressed: Boolean,
    kx: Float = 0f,
    ky: Float = 0f,
    dpad: Int = 0,
    lit: Boolean = false,
  ) {
    when (s.id.kind) {
      ControlKind.FACE -> face(c, s, pressed)
      ControlKind.GEM -> gem(c, s, pressed)
      ControlKind.PILL -> pill(c, s, pressed)
      ControlKind.STICK -> stick(c, s, kx, ky)
      ControlKind.DPAD -> dpad(c, s, dpad)
      ControlKind.TRIGGER -> trigger(c, s, pressed)
      ControlKind.CLICK -> click(c, s, pressed)
      ControlKind.MENU -> menu(c, s, pressed)
      ControlKind.CUSTOM -> custom(c, s, pressed, lit)
    }
  }

  /*
   * The dark recess the control sits in, with a faint edge to read on any
   * picture. The sticks' and the D-pad's are large, so they're only a tint
   * (fill): the game shows through them.
   */
  private fun socket(c: Canvas, cx: Float, cy: Float, r: Float, fill: Int = 150) {
    p.shader = null
    p.style = Paint.Style.FILL
    p.color = Color.argb(fill, 10, 10, 10)
    c.drawCircle(cx, cy, r, p)
    p.style = Paint.Style.STROKE
    p.strokeWidth = maxOf(2f, r * 0.03f)
    p.color = Color.argb(80, 255, 255, 255)
    c.drawCircle(cx, cy, r, p)
  }

  private fun jewel(c: Canvas, cx: Float, cy: Float, r0: Float, j: Jewel, pressed: Boolean) {
    val r = if (pressed) r0 * 0.94f else r0
    p.style = Paint.Style.FILL
    shade(RadialGradient(
      cx - r * 0.35f, cy - r * 0.4f, r * 1.45f,
      intArrayOf(if (pressed) j.base else j.light, j.base, j.dark),
      floatArrayOf(0f, 0.5f, 1f), Shader.TileMode.CLAMP,
    ))
    c.drawCircle(cx, cy, r, p)
    p.shader = null
    p.style = Paint.Style.STROKE
    p.strokeWidth = r * 0.06f
    p.color = withAlpha(j.dark, 200)
    c.drawCircle(cx, cy, r * 0.97f, p)
    // The glossy highlight across the top.
    rect.set(cx - r * 0.55f, cy - r * 0.84f, cx + r * 0.55f, cy - r * 0.16f)
    p.style = Paint.Style.FILL
    shade(LinearGradient(
      0f, rect.top, 0f, rect.bottom,
      Color.argb(if (pressed) 60 else 150, 255, 255, 255), Color.argb(0, 255, 255, 255), Shader.TileMode.CLAMP,
    ))
    c.drawOval(rect, p)
    p.shader = null
  }

  private fun face(c: Canvas, s: ControlSpec, pressed: Boolean) {
    val j = jewels.getValue(s.id)
    socket(c, s.cx, s.cy, s.radius * 1.13f)
    jewel(c, s.cx, s.cy, s.radius, j, pressed)
    // The letter set into the jewel: a light edge under a darker cut.
    text.textSize = s.radius * 0.95f
    val base = s.cy + text.textSize * 0.35f
    text.color = Color.argb(90, 255, 255, 255)
    c.drawText(s.id.label, s.cx, base + s.radius * 0.05f, text)
    text.color = withAlpha(j.dark, 215)
    c.drawText(s.id.label, s.cx, base, text)
  }

  private fun custom(c: Canvas, s: ControlSpec, pressed: Boolean, lit: Boolean) {
    val j = customJewels.getValue(s.id)
    socket(c, s.cx, s.cy, s.radius * 1.13f)
    jewel(c, s.cx, s.cy, s.radius, j, pressed)
    text.textSize = s.radius * 0.95f
    val base = s.cy + text.textSize * 0.35f
    text.color = Color.argb(90, 255, 255, 255)
    c.drawText(s.id.label, s.cx, base + s.radius * 0.05f, text)
    text.color = withAlpha(j.dark, 215)
    c.drawText(s.id.label, s.cx, base, text)
    if (lit) {
      // a switch that is on: a green ring around it
      p.shader = null
      p.style = Paint.Style.STROKE
      p.strokeWidth = s.radius * 0.16f
      p.color = Color.argb(235, 139, 195, 74)
      c.drawCircle(s.cx, s.cy, s.radius * 1.2f, p)
    }
  }

  private fun gem(c: Canvas, s: ControlSpec, pressed: Boolean) {
    socket(c, s.cx, s.cy, s.radius * 1.15f)
    jewel(c, s.cx, s.cy, s.radius, jewels.getValue(s.id), pressed)
  }

  private fun click(c: Canvas, s: ControlSpec, pressed: Boolean) {
    socket(c, s.cx, s.cy, s.radius * 1.15f)
    jewel(c, s.cx, s.cy, s.radius, clickJewel, pressed)
    text.textSize = s.radius * 0.7f
    text.color = Color.argb(220, 230, 230, 230)
    c.drawText(s.id.label, s.cx, s.cy + text.textSize * 0.35f, text)
  }

  /* Back and start: small black ovals with their name on them. */
  private fun pill(c: Canvas, s: ControlSpec, pressed: Boolean) {
    val r = s.radius
    val hw = r * 1.5f
    val hh = r * 0.68f
    rect.set(s.cx - hw - r * 0.15f, s.cy - hh - r * 0.15f, s.cx + hw + r * 0.15f, s.cy + hh + r * 0.15f)
    p.shader = null
    p.style = Paint.Style.FILL
    p.color = Color.argb(150, 10, 10, 10)
    c.drawRoundRect(rect, hh + r * 0.15f, hh + r * 0.15f, p)
    p.style = Paint.Style.STROKE
    p.strokeWidth = maxOf(2f, r * 0.04f)
    p.color = Color.argb(80, 255, 255, 255)
    c.drawRoundRect(rect, hh + r * 0.15f, hh + r * 0.15f, p)

    rect.set(s.cx - hw, s.cy - hh, s.cx + hw, s.cy + hh)
    p.style = Paint.Style.FILL
    shade(LinearGradient(
      0f, rect.top, 0f, rect.bottom,
      if (pressed) 0xFF2A2A2A.toInt() else 0xFF505050.toInt(), 0xFF0E0E0E.toInt(), Shader.TileMode.CLAMP,
    ))
    c.drawRoundRect(rect, hh, hh, p)
    rect.set(s.cx - hw * 0.7f, s.cy - hh * 0.85f, s.cx + hw * 0.7f, s.cy - hh * 0.1f)
    shade(LinearGradient(
      0f, rect.top, 0f, rect.bottom,
      Color.argb(if (pressed) 30 else 90, 255, 255, 255), Color.argb(0, 255, 255, 255), Shader.TileMode.CLAMP,
    ))
    c.drawOval(rect, p)
    p.shader = null

    text.textSize = r * 0.5f
    text.letterSpacing = 0.08f
    text.color = if (pressed) Color.argb(200, 170, 170, 170) else Color.argb(230, 225, 225, 225)
    c.drawText(s.id.label, s.cx, s.cy + text.textSize * 0.36f, text)
    text.letterSpacing = 0f
  }

  /* A thumbstick: the dark base, the cap moved by the deflection, concave with the cross grooves. */
  private fun stick(c: Canvas, s: ControlSpec, kx: Float, ky: Float) {
    val r = s.radius
    socket(c, s.cx, s.cy, r, fill = WELL_FILL)
    val kr = r * 0.58f
    val travel = r - kr * 0.75f
    val x = s.cx + kx * travel
    val y = s.cy + ky * travel

    // Everything concentric, lit straight from above: a soft shadow all round
    // the cap, the cap brighter at the top, the dish darker at the top (concave).
    p.style = Paint.Style.FILL
    shade(RadialGradient(
      x, y, kr * 1.2f,
      intArrayOf(Color.argb(110, 0, 0, 0), Color.argb(110, 0, 0, 0), Color.argb(0, 0, 0, 0)),
      floatArrayOf(0f, 0.8f, 1f), Shader.TileMode.CLAMP,
    ))
    c.drawCircle(x, y, kr * 1.2f, p)
    shade(LinearGradient(
      0f, y - kr, 0f, y + kr,
      intArrayOf(0xFF5E5E5E.toInt(), 0xFF323232.toInt(), 0xFF161616.toInt()),
      floatArrayOf(0f, 0.5f, 1f), Shader.TileMode.CLAMP,
    ))
    c.drawCircle(x, y, kr, p)
    val dish = kr * 0.74f
    shade(LinearGradient(
      0f, y - dish, 0f, y + dish,
      0xFF1C1C1C.toInt(), 0xFF3A3A3A.toInt(), Shader.TileMode.CLAMP,
    ))
    c.drawCircle(x, y, dish, p)
    p.shader = null
    p.style = Paint.Style.STROKE
    p.strokeCap = Paint.Cap.ROUND
    p.strokeWidth = kr * 0.07f
    p.color = 0xFF121212.toInt()
    for ((dx, dy) in AXES) {
      c.drawLine(x + dx * kr * 0.2f, y + dy * kr * 0.2f, x + dx * kr * 0.6f, y + dy * kr * 0.6f, p)
    }
    p.strokeCap = Paint.Cap.BUTT
  }

  /*
   * The D-pad, as on the Duke S: a black cross with round-ended arms and a
   * dished centre, in a round recess. It rocks on its centre: a held arm
   * sinks (darker towards its end) and the opposite one rises (catches the
   * light); the cross itself stays put.
   */
  private fun dpad(c: Canvas, s: ControlSpec, bits: Int) {
    val r = s.radius
    socket(c, s.cx, s.cy, r, fill = WELL_FILL)

    val cx = s.cx
    val cy = s.cy
    val aw = r * 0.29f
    val al = r * 0.84f
    // One outline for the cross (two overlapping bars would stroke a seam in the middle).
    path.reset()
    path.addRoundRect(cx - aw, cy - al, cx + aw, cy + al, aw * 0.85f, aw * 0.85f, Path.Direction.CW)
    bar.reset()
    bar.addRoundRect(cx - al, cy - aw, cx + al, cy + aw, aw * 0.85f, aw * 0.85f, Path.Direction.CW)
    path.op(bar, Path.Op.UNION)

    // Its shadow in the recess (light from above), then the cross.
    p.style = Paint.Style.FILL
    p.shader = null
    p.color = Color.argb(140, 0, 0, 0)
    c.save()
    c.translate(0f, r * 0.05f)
    c.drawPath(path, p)
    c.restore()
    shade(LinearGradient(
      0f, cy - al, 0f, cy + al, 0xFF575757.toInt(), 0xFF1C1C1C.toInt(), Shader.TileMode.CLAMP,
    ))
    c.drawPath(path, p)

    // The tilt: one gradient over the whole cross along the held direction
    // (a diagonal for two), down into the recess on the held side, clear at
    // the centre, up into the light on the other. The side arms take the
    // same slope across their width, so nothing ends in an edge.
    var tx = 0f
    var ty = 0f
    for ((bit, dx, dy) in DIRS) {
      if (bits and bit != 0) {
        tx += dx
        ty += dy
      }
    }
    val tl = hypot(tx, ty)
    if (tl > 0f) {
      val ux = tx / tl
      val uy = ty / tl
      c.save()
      c.clipPath(path)
      shade(LinearGradient(
        cx + ux * al, cy + uy * al, cx - ux * al, cy - uy * al,
        intArrayOf(Color.argb(200, 0, 0, 0), Color.argb(0, 0, 0, 0), Color.argb(0, 255, 255, 255), Color.argb(70, 255, 255, 255)),
        floatArrayOf(0f, 0.5f, 0.5f, 1f),
        Shader.TileMode.CLAMP,
      ))
      c.drawRect(cx - al, cy - al, cx + al, cy + al, p)
      c.restore()
    }
    p.shader = null
    p.style = Paint.Style.STROKE
    p.strokeWidth = maxOf(1.5f, r * 0.02f)
    p.color = Color.argb(150, 0, 0, 0)
    c.drawPath(path, p)

    // The dish in the middle, darker at the top (concave under light from above).
    val dish = aw * 0.75f
    p.style = Paint.Style.FILL
    shade(LinearGradient(
      0f, cy - dish, 0f, cy + dish, 0xFF1A1A1A.toInt(), 0xFF383838.toInt(), Shader.TileMode.CLAMP,
    ))
    c.drawCircle(cx, cy, dish, p)
    p.shader = null
  }

  /* A trigger: the rounded grip shape of the Duke's L and R, its outer top corner swept. */
  private fun trigger(c: Canvas, s: ControlSpec, pressed: Boolean) {
    val r = s.radius
    val right = s.id == ControlId.RIGHT_TRIGGER
    val hw = r * 1.05f
    val hh = r * 0.82f
    rect.set(s.cx - hw, s.cy - hh, s.cx + hw, s.cy + hh)
    val big = r * 0.95f
    val small = r * 0.32f
    // Corner radii: top-left, top-right, bottom-right, bottom-left (x, y pairs).
    val radii = if (right) {
      floatArrayOf(small, small, big, big, small, small, small, small)
    } else {
      floatArrayOf(big, big, small, small, small, small, small, small)
    }
    path.reset()
    path.addRoundRect(rect, radii, Path.Direction.CW)
    p.shader = null
    p.style = Paint.Style.FILL
    p.color = Color.argb(150, 10, 10, 10)
    c.save()
    c.translate(0f, r * 0.08f)
    c.drawPath(path, p)
    c.restore()
    shade(LinearGradient(
      0f, rect.top, 0f, rect.bottom,
      if (pressed) 0xFF7A7A7A.toInt() else 0xFF545454.toInt(),
      if (pressed) 0xFF2E2E2E.toInt() else 0xFF1A1A1A.toInt(),
      Shader.TileMode.CLAMP,
    ))
    c.drawPath(path, p)
    p.shader = null
    p.style = Paint.Style.STROKE
    p.strokeWidth = maxOf(2f, r * 0.03f)
    p.color = Color.argb(90, 255, 255, 255)
    c.drawPath(path, p)

    text.textSize = r * 0.8f
    text.color = Color.argb(225, 235, 235, 235)
    c.drawText(s.id.label, s.cx, s.cy + text.textSize * 0.35f, text)
  }

  /* The jewel in the middle of the controller: black glass with a green dot. Opens the menu. */
  private fun menu(c: Canvas, s: ControlSpec, pressed: Boolean) {
    val r = s.radius
    socket(c, s.cx, s.cy, r * 1.1f)
    p.style = Paint.Style.FILL
    shade(RadialGradient(
      s.cx - r * 0.3f, s.cy - r * 0.35f, r * 1.4f,
      intArrayOf(0xFF3E3E3E.toInt(), 0xFF101010.toInt(), 0xFF000000.toInt()),
      floatArrayOf(0f, 0.5f, 1f), Shader.TileMode.CLAMP,
    ))
    c.drawCircle(s.cx, s.cy, r, p)
    val d = r * 0.38f
    shade(LinearGradient(
      s.cx - d, s.cy - d, s.cx + d, s.cy + d,
      0xFFC6EE7A.toInt(), 0xFF3A8F1C.toInt(), Shader.TileMode.CLAMP,
    ))
    c.drawCircle(s.cx, s.cy, d, p)
    p.shader = null
    rect.set(s.cx - r * 0.6f, s.cy - r * 0.88f, s.cx + r * 0.6f, s.cy - r * 0.2f)
    shade(LinearGradient(
      0f, rect.top, 0f, rect.bottom,
      Color.argb(110, 255, 255, 255), Color.argb(0, 255, 255, 255), Shader.TileMode.CLAMP,
    ))
    c.drawOval(rect, p)
    p.shader = null
    if (pressed) {
      p.color = Color.argb(70, 255, 255, 255)
      c.drawCircle(s.cx, s.cy, r, p)
    }
  }

  /*
   * A gradient fill. The paint's colour alpha multiplies a shader, so the
   * paint goes opaque first (it may still carry an outline's faint alpha).
   */
  private fun shade(shader: Shader) {
    p.style = Paint.Style.FILL
    p.color = Color.BLACK
    p.shader = shader
  }

  private fun withAlpha(color: Int, alpha: Int) = (color and 0x00FFFFFF) or (alpha shl 24)

  companion object {
    /** The tint inside the sticks' and the D-pad's outer circle (alpha). */
    private const val WELL_FILL = 45

    const val DPAD_UP = 1
    const val DPAD_DOWN = 2
    const val DPAD_LEFT = 4
    const val DPAD_RIGHT = 8

    private val AXES = listOf(0f to -1f, 0f to 1f, -1f to 0f, 1f to 0f)
    private val DIRS = listOf(
      Triple(DPAD_UP, 0f, -1f),
      Triple(DPAD_DOWN, 0f, 1f),
      Triple(DPAD_LEFT, -1f, 0f),
      Triple(DPAD_RIGHT, 1f, 0f),
    )

    /** How far from a control's centre its touches and its edit box reach, in radii. */
    fun reach(kind: ControlKind): Float = when (kind) {
      ControlKind.FACE -> 1.15f
      ControlKind.GEM -> 1.25f
      ControlKind.PILL -> 1.6f
      ControlKind.STICK -> 1.05f
      ControlKind.DPAD -> 1.1f
      ControlKind.TRIGGER -> 1.3f
      ControlKind.CLICK -> 1.3f
      ControlKind.MENU -> 1.2f
      ControlKind.CUSTOM -> 1.15f
    }
  }
}
