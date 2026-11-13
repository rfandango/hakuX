package com.rfandango.haku_x.overlay

import android.content.Context
import org.json.JSONObject
import java.io.File

/** How a control is drawn and how it takes touches. */
enum class ControlKind { FACE, GEM, PILL, STICK, DPAD, TRIGGER, CLICK, MENU, CUSTOM }

/** The on-screen controller's controls, laid out like the original Xbox controller (the "Duke"). */
enum class ControlId(val kind: ControlKind, val label: String, val title: String) {
  A(ControlKind.FACE, "A", "A button"),
  B(ControlKind.FACE, "B", "B button"),
  X(ControlKind.FACE, "X", "X button"),
  Y(ControlKind.FACE, "Y", "Y button"),
  WHITE(ControlKind.GEM, "", "White button"),
  BLACK(ControlKind.GEM, "", "Black button"),
  BACK(ControlKind.PILL, "BACK", "Back"),
  START(ControlKind.PILL, "START", "Start"),
  LEFT_TRIGGER(ControlKind.TRIGGER, "L", "Left trigger"),
  RIGHT_TRIGGER(ControlKind.TRIGGER, "R", "Right trigger"),
  LEFT_STICK(ControlKind.STICK, "", "Left stick"),
  RIGHT_STICK(ControlKind.STICK, "", "Right stick"),
  LEFT_STICK_CLICK(ControlKind.CLICK, "LS", "Left stick click"),
  RIGHT_STICK_CLICK(ControlKind.CLICK, "RS", "Right stick click"),
  DPAD(ControlKind.DPAD, "", "D-pad"),
  MENU(ControlKind.MENU, "", "Menu"),

  /** Switches swipe to look on and off while playing. */
  TOUCH_LOOK(ControlKind.CUSTOM, "T", "Touch look switch"),

  /** Custom buttons: each presses the one or two pad buttons it is mapped to (0 and 1 can be swiped from while held). */
  CUSTOM_0(ControlKind.CUSTOM, "0", "Custom button 0"),
  CUSTOM_1(ControlKind.CUSTOM, "1", "Custom button 1"),
  CUSTOM_2(ControlKind.CUSTOM, "2", "Custom button 2"),
}

/**
 * One control's place on screen, in pixels of the overlay; radius is its
 * size, opacity how much of the game it hides (0.1-1).
 */
class ControlSpec(
  val id: ControlId,
  var cx: Float,
  var cy: Float,
  var radius: Float,
  var visible: Boolean = true,
  var opacity: Float = OverlayStore.DEFAULT_OPACITY,
) {
  fun copy() = ControlSpec(id, cx, cy, radius, visible, opacity)
}

/** All controls. */
class OverlayLayout(val controls: MutableMap<ControlId, ControlSpec>) {
  operator fun get(id: ControlId): ControlSpec = controls.getValue(id)
  fun copy() = OverlayLayout(controls.mapValuesTo(LinkedHashMap()) { it.value.copy() })
}

/**
 * The layout the user made, kept as a file in the app's storage so the
 * settings (main process) and the game (the core's process) read the same
 * one: positions as fractions of the overlay's width and height, sizes as
 * fractions of its height. Missing: the defaults for the screen at hand.
 */
object OverlayStore {
  private const val FILE = "overlay_layout.json"

  /**
   * The layout file: the app's (game null), or a game's own, named after its key. A game without
   * one of its own uses the app's.
   */
  private fun file(context: Context, game: String?): File =
    if (game == null) {
      File(context.filesDir, FILE)
    } else {
      File(context.filesDir, "overlay_layout_" + game.replace(Regex("[^A-Za-z0-9._-]"), "_").take(80) + ".json")
    }

  /** Whether a game has a layout of its own. */
  fun hasOwn(context: Context, game: String): Boolean = file(context, game).isFile
  /** Half see-through: the game stays visible under the controls. */
  const val DEFAULT_OPACITY = 0.5f

  /** A game's own layout if it has one, else the app's (the defaults if there is none). */
  fun load(context: Context, w: Float, h: Float, game: String? = null): OverlayLayout {
    val layout = defaults(w, h)
    val own = if (game != null) file(context, game) else null
    val file = if (own != null && own.isFile) own else file(context, null)
    if (!file.isFile) {
      return layout
    }
    try {
      val root = JSONObject(file.readText())
      val controls = root.optJSONObject("controls") ?: return layout
      for (id in ControlId.entries) {
        val c = controls.optJSONObject(id.name) ?: continue
        val spec = layout[id]
        spec.cx = (c.optDouble("x", (spec.cx / w).toDouble()).toFloat() * w).coerceIn(0f, w)
        spec.cy = (c.optDouble("y", (spec.cy / h).toDouble()).toFloat() * h).coerceIn(0f, h)
        spec.radius = (c.optDouble("r", (spec.radius / h).toDouble()).toFloat() * h).coerceIn(minRadius(h), maxRadius(h))
        spec.visible = id == ControlId.MENU || c.optBoolean("v", true)
        spec.opacity = c.optDouble("o", DEFAULT_OPACITY.toDouble()).toFloat().coerceIn(MIN_OPACITY, 1f)
      }
    } catch (_: Exception) {
      return defaults(w, h)
    }
    return layout
  }

  /** Keeps the layout: as the app's (game null) or as the game's own. */
  fun save(context: Context, layout: OverlayLayout, w: Float, h: Float, game: String? = null) {
    val controls = JSONObject()
    for ((id, spec) in layout.controls) {
      controls.put(id.name, JSONObject().apply {
        put("x", (spec.cx / w).toDouble())
        put("y", (spec.cy / h).toDouble())
        put("r", (spec.radius / h).toDouble())
        put("v", spec.visible)
        put("o", spec.opacity.toDouble())
      })
    }
    val root = JSONObject().apply {
      put("version", 2)
      put("controls", controls)
    }
    val file = file(context, game)
    val tmp = File(file.parentFile, file.name + ".tmp")
    tmp.writeText(root.toString())
    tmp.renameTo(file)
  }

  /** The file goes: a game goes back to the app's layout, the app's back to the defaults. */
  fun reset(context: Context, game: String? = null) {
    file(context, game).delete()
  }

  const val MIN_OPACITY = 0.1f
  fun minRadius(h: Float) = h * 0.025f
  fun maxRadius(h: Float) = h * 0.35f

  /**
   * The Duke's arrangement on a landscape screen: left stick up and left with
   * the D-pad below and inside it; the face buttons in two offset columns
   * (Y over X, B over A, the right column half a row lower) with white and
   * black above them; the right stick below and inside them; back, the jewel
   * (the menu) and start at the bottom centre; the triggers in the top corners.
   */
  fun defaults(w: Float, h: Float): OverlayLayout {
    val c = LinkedHashMap<ControlId, ControlSpec>()
    fun put(id: ControlId, x: Float, y: Float, r: Float) {
      c[id] = ControlSpec(id, x, y, r)
    }
    val face = h * 0.072f
    val sx = face * 2.15f
    val sy = face * 2.3f
    val fx = w - h * 0.27f
    val fy = h * 0.47f
    put(ControlId.Y, fx - sx / 2, fy - sy * 0.75f, face)
    put(ControlId.X, fx - sx / 2, fy + sy * 0.25f, face)
    put(ControlId.B, fx + sx / 2, fy - sy * 0.25f, face)
    put(ControlId.A, fx + sx / 2, fy + sy * 0.75f, face)
    val gem = h * 0.05f
    put(ControlId.WHITE, fx - sx * 0.05f, fy - sy * 1.55f, gem)
    put(ControlId.BLACK, fx + sx * 0.95f, fy - sy * 1.15f, gem)

    put(ControlId.LEFT_STICK, h * 0.27f, h * 0.43f, h * 0.135f)
    put(ControlId.DPAD, h * 0.58f, h * 0.78f, h * 0.125f)
    put(ControlId.RIGHT_STICK, w - h * 0.58f, h * 0.78f, h * 0.125f)
    put(ControlId.LEFT_STICK_CLICK, h * 0.10f, h * 0.72f, h * 0.045f)
    put(ControlId.RIGHT_STICK_CLICK, w - h * 0.33f, h * 0.92f, h * 0.045f)

    val pill = h * 0.05f
    put(ControlId.MENU, w / 2, h * 0.86f, h * 0.06f)
    put(ControlId.BACK, w / 2 - h * 0.2f, h * 0.9f, pill)
    put(ControlId.START, w / 2 + h * 0.2f, h * 0.9f, pill)

    put(ControlId.LEFT_TRIGGER, h * 0.12f, h * 0.12f, h * 0.075f)
    put(ControlId.RIGHT_TRIGGER, w - h * 0.12f, h * 0.12f, h * 0.075f)

    // the custom buttons: a row across the top centre, clear of the triggers
    val custom = h * 0.045f
    put(ControlId.TOUCH_LOOK, w / 2 - h * 0.30f, h * 0.11f, custom)
    put(ControlId.CUSTOM_0, w / 2 - h * 0.10f, h * 0.11f, custom)
    put(ControlId.CUSTOM_1, w / 2 + h * 0.10f, h * 0.11f, custom)
    put(ControlId.CUSTOM_2, w / 2 + h * 0.30f, h * 0.11f, custom)
    return OverlayLayout(c)
  }
}
