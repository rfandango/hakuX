package com.rfandango.haku_x.data

import android.content.Context
import com.rfandango.haku_x.R
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/**
 * Swipe to look: how a finger dragged over the screen turns the camera, in
 * place of the on-screen right stick (see TouchLookEngine), and what goes with
 * it: the custom buttons, the double taps, the turbo and the dynamic stick.
 * Percentages are of the defaults; smoothing is the span of finger motion (ms)
 * one stick value averages over: less answers sooner, more is steadier.
 */
data class TouchLookConfig(
  val enabled: Boolean = false,
  /** How far the camera turns for a normal swipe, left and right: 100 turns the stick fully at 40 cm/s. */
  val sensitivity: Int = 600,
  /** The same up and down, in the same percentages; 0 is the same as left and right. */
  val sensitivityY: Int = 0,
  /** The game's own dead zone to skip (share of the stick's range), so slow swipes still move the camera. */
  val deadZone: Int = 40,
  val smoothing: Int = 24,
  /** 0 off, then 1 to 3: slow swipes turn less, fast flicks more, and a fast flick keeps turning a moment. */
  val accel: Int = 0,
  /** Where a finger turns the camera: AREA_RIGHT or AREA_ALL. */
  val area: Int = AREA_RIGHT,
  /** The left stick appears where a finger comes down on the left half, and goes when it lifts. Needs AREA_RIGHT. */
  val dynamicStick: Boolean = false,
  /**
   * What each of the SOURCES presses: custom buttons 0, 1 and 2, then a double tap on the left half
   * and on the right: pad button names (OnScreenController.Button), comma-separated, at most two.
   */
  val mapping: List<String> = List(SOURCES) { "" },
  /** Which sources press over and over while held: bit i for source i. */
  val turbo: Int = 0,
  /** The period (ms) of the turbo: down for the first half, up for the second. */
  val turboMs: Int = 100,
  /** The longest wait (ms) between a tap and the second of a double tap. */
  val tapMs: Int = 250,
  /** The on-screen right stick and swipe to look turn the camera the other way, left for right / up for down. */
  val invertX: Boolean = false,
  val invertY: Boolean = false,
) {
  /** The sensitivity up and down, in percent. */
  val verticalSensitivity get() = if (sensitivityY > 0) sensitivityY else sensitivity

  /** The pad buttons a source presses (names, at most two). */
  fun targets(source: Int): List<String> = parseTargets(mapping.getOrElse(source) { "" })

  fun withTargets(source: Int, targets: List<String>): TouchLookConfig =
    copy(mapping = List(SOURCES) { if (it == source) formatTargets(targets) else mapping.getOrElse(it) { "" } })

  fun hasTurbo(source: Int): Boolean = (turbo shr source) and 1 == 1

  fun withTurbo(source: Int, on: Boolean): TouchLookConfig =
    copy(turbo = if (on) turbo or (1 shl source) else turbo and (1 shl source).inv())

  fun toJson(): JSONObject = JSONObject()
    .put("on", enabled)
    .put("sens", sensitivity)
    .put("sensY", sensitivityY)
    .put("dz", deadZone)
    .put("smooth", smoothing)
    .put("accel", accel)
    .put("area", area)
    .put("dyn", dynamicStick)
    .put("map", JSONArray(mapping))
    .put("turbo", turbo)
    .put("turboMs", turboMs)
    .put("tapMs", tapMs)
    .put("invX", invertX)
    .put("invY", invertY)

  companion object {
    /** The sources of presses: custom buttons 0, 1 and 2, a double tap on the left half, one on the right. */
    const val SOURCES = 5
    const val BUTTON_0 = 0
    const val BUTTON_1 = 1
    const val BUTTON_2 = 2
    const val TAP_LEFT = 3
    const val TAP_RIGHT = 4

    /** The right half of the screen turns the camera (the left half is for the left stick). */
    const val AREA_RIGHT = 0

    /** Anywhere free of a control. */
    const val AREA_ALL = 1

    /** The vertical sensitivity that follows the horizontal one. */
    const val SAME = 0

    val SENSITIVITIES = listOf(50, 75, 100, 125, 150, 200, 250, 300, 400, 500, 600, 800)
    val VERTICALS = listOf(SAME) + SENSITIVITIES
    val DEAD_ZONES = listOf(0, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60)
    val SMOOTHINGS = listOf(8, 16, 24, 40)
    val ACCELS = listOf(0, 1, 2, 3)
    val AREAS = listOf(AREA_RIGHT, AREA_ALL)
    val TURBO_PERIODS = listOf(66, 100, 133, 166, 200, 250, 333, 500)
    val TAP_GAPS = listOf(150, 200, 250, 300, 400, 500)

    /** The pad buttons a source can press (OnScreenController.Button names), in the order they are listed. */
    val TARGETS = listOf(
      "A", "B", "X", "Y", "LEFT_TRIGGER", "RIGHT_TRIGGER", "WHITE", "BLACK", "START", "BACK",
      "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT", "LEFT_STICK_BUTTON", "RIGHT_STICK_BUTTON",
    )

    /** The valid button names in a comma-separated string, at most two, none twice. */
    fun parseTargets(s: String): List<String> = s.split(',').map { it.trim() }.filter { it in TARGETS }.distinct().take(2)

    fun formatTargets(targets: List<String>): String = targets.filter { it in TARGETS }.distinct().take(2).joinToString(",")

    /**
     * The config a JSON object holds; what it lacks (or holds out of range) is base's. An older file
     * held the vertical sensitivity as a share of the horizontal one ("vert"): that is now the number.
     */
    fun fromJson(o: JSONObject, base: TouchLookConfig = TouchLookConfig()): TouchLookConfig {
      val sens = o.optInt("sens", base.sensitivity).coerceIn(10, 2000)
      val sensY = when {
        o.has("sensY") -> o.optInt("sensY", SAME)
        o.has("vert") -> o.optInt("vert", 100).let { if (it == 100) SAME else sens * it / 100 }
        else -> base.sensitivityY
      }
      val area = if (o.optInt("area", base.area) == AREA_ALL) AREA_ALL else AREA_RIGHT
      val map = o.optJSONArray("map")
      return TouchLookConfig(
        enabled = o.optBoolean("on", base.enabled),
        sensitivity = sens,
        sensitivityY = if (sensY <= 0) SAME else sensY.coerceIn(10, 2000),
        deadZone = o.optInt("dz", base.deadZone).coerceIn(0, 80),
        smoothing = o.optInt("smooth", base.smoothing).coerceIn(4, 100),
        accel = o.optInt("accel", base.accel).coerceIn(0, 3),
        area = area,
        // a dynamic stick takes the left half, which the whole screen would give to the look
        dynamicStick = o.optBoolean("dyn", base.dynamicStick) && area == AREA_RIGHT,
        mapping = List(SOURCES) { i ->
          if (map != null) formatTargets(parseTargets(map.optString(i, ""))) else base.mapping.getOrElse(i) { "" }
        },
        turbo = o.optInt("turbo", base.turbo) and ((1 shl SOURCES) - 1),
        turboMs = o.optInt("turboMs", base.turboMs).coerceIn(40, 2000),
        tapMs = o.optInt("tapMs", base.tapMs).coerceIn(100, 1500),
        invertX = o.optBoolean("invX", base.invertX),
        invertY = o.optBoolean("invY", base.invertY),
      )
    }

    /** The string resource naming a smoothing value (the nearest preset's, for one set by hand). */
    fun smoothingLabel(ms: Int): Int = when {
      ms <= 8 -> R.string.look_smooth_minimal
      ms <= 16 -> R.string.look_smooth_responsive
      ms <= 24 -> R.string.look_smooth_balanced
      else -> R.string.look_smooth_smooth
    }

    fun accelLabel(level: Int): Int = when (level) {
      0 -> R.string.look_accel_off
      1 -> R.string.look_accel_low
      2 -> R.string.look_accel_medium
      else -> R.string.look_accel_high
    }

    fun areaLabel(area: Int): Int = if (area == AREA_ALL) R.string.look_area_all else R.string.look_area_right

    /** The string resource naming a pad button (a name from TARGETS). */
    fun targetLabel(name: String): Int = when (name) {
      "A" -> R.string.look_target_a
      "B" -> R.string.look_target_b
      "X" -> R.string.look_target_x
      "Y" -> R.string.look_target_y
      "LEFT_TRIGGER" -> R.string.look_target_lt
      "RIGHT_TRIGGER" -> R.string.look_target_rt
      "WHITE" -> R.string.look_target_white
      "BLACK" -> R.string.look_target_black
      "START" -> R.string.look_target_start
      "BACK" -> R.string.look_target_back
      "DPAD_UP" -> R.string.look_target_up
      "DPAD_DOWN" -> R.string.look_target_down
      "DPAD_LEFT" -> R.string.look_target_left
      "DPAD_RIGHT" -> R.string.look_target_right
      "LEFT_STICK_BUTTON" -> R.string.look_target_ls
      "RIGHT_STICK_BUTTON" -> R.string.look_target_rs
      else -> R.string.look_target_none
    }
  }
}

/**
 * Swipe to look's settings as a file in the app's storage, read by the
 * settings (main process) and the game (the core's process) alike and written
 * by both: the settings change the default for every game, the game's menu
 * the one game's. A file, as the on-screen layout and the mods' choices are,
 * because each process keeps its own copy of the shared preferences, and the
 * one written second would write the other's changes away.
 *
 * {"global": {...}, "games": {"<game key>": {...}}}: a game's entry is the
 * whole config it was set to, kept apart from the default after that.
 */
object TouchLookStore {
  private const val FILE = "touch_look.json"
  private val LOCK = Any()

  /** The default for every game that has none of its own. */
  fun global(context: Context): TouchLookConfig = synchronized(LOCK) {
    TouchLookConfig.fromJson(read(context).optJSONObject("global") ?: JSONObject())
  }

  fun setGlobal(context: Context, config: TouchLookConfig) {
    synchronized(LOCK) {
      write(context, read(context).put("global", config.toJson()))
    }
  }

  /** What a game runs with: its own config if it has one, else the default. */
  fun forGame(context: Context, game: String): TouchLookConfig = synchronized(LOCK) {
    val root = read(context)
    val base = TouchLookConfig.fromJson(root.optJSONObject("global") ?: JSONObject())
    val own = root.optJSONObject("games")?.optJSONObject(game)
    if (own == null) base else TouchLookConfig.fromJson(own, base)
  }

  fun hasOwn(context: Context, game: String): Boolean = synchronized(LOCK) {
    read(context).optJSONObject("games")?.has(game) == true
  }

  /** A game's own config; null takes it back to the default. */
  fun setGame(context: Context, game: String, config: TouchLookConfig?) {
    synchronized(LOCK) {
      val root = read(context)
      val games = root.optJSONObject("games") ?: JSONObject().also { root.put("games", it) }
      if (config == null) games.remove(game) else games.put(game, config.toJson())
      write(context, root)
    }
  }

  private fun read(context: Context): JSONObject {
    val file = File(context.filesDir, FILE)
    if (!file.isFile) return JSONObject()
    return try {
      JSONObject(file.readText())
    } catch (_: Exception) {
      JSONObject()
    }
  }

  private fun write(context: Context, root: JSONObject) {
    val file = File(context.filesDir, FILE)
    val tmp = File(context.filesDir, "$FILE.${android.os.Process.myPid()}.tmp")
    tmp.writeText(root.toString())
    tmp.renameTo(file)
  }
}
