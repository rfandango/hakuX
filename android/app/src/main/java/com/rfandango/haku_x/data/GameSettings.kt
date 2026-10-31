package com.rfandango.haku_x.data

import android.app.Application
import android.content.Context
import androidx.core.content.edit
import com.rfandango.haku_x.BuildConfig
import java.util.Locale

/** The settings a game can override; the rest are the app's for every game. */
enum class GameSetting(val key: String) {
  UNCAPPED("uncapped"),
  RENDER_SCALE("render_scale"),
  ASYNC_SHADERS("async_shaders"),
  PREBUILD_SHADERS("prebuild_shaders"),
  CONSOLE_TIMING("console_timing"),
  ASPECT("aspect"),
  FXAA("fxaa"),
  UPSCALE("upscale"),
  LANGUAGE("language"),
  SCREEN_FORMAT("screen_format"),
  VIDEO_480P("480p"),
  VIDEO_720P("720p"),
  VIDEO_1080I("1080i"),
  PAL60("pal60"),
  AV_PACK("av_pack"),
}

/**
 * One game's overrides, in per_game_settings as "<game>/<setting>" -> value;
 * a setting without one follows the app's. A game is its disc's title ID and
 * certificate version (gameKey), so an image converted or moved keeps them;
 * its path when the disc has no ID.
 */
class GameOverrides(context: Context, val game: String) {
  private val sp = context.getSharedPreferences(NAME, Context.MODE_PRIVATE)
  /** The bare title ID the overrides were kept under before the version joined the key. */
  private val legacy = if (!game.startsWith(PATH) && '.' in game) game.substringBefore('.') else null

  init {
    // Overrides from before the version joined the key were the bare title
    // ID's: the first of its discs to open takes them. In the main process
    // only: one written by the game's (its own process) is overwritten by
    // the settings' cached copy; the game's reads them where they are (get).
    if (legacy != null && Application.getProcessName() == context.packageName) {
      val old = GameSetting.entries.mapNotNull { s -> sp.getString("$legacy/${s.key}", null)?.let { s to it } }
      if (old.isNotEmpty()) {
        sp.edit {
          for ((s, value) in old) {
            if (!sp.contains(key(s))) putString(key(s), value)
            remove("$legacy/${s.key}")
          }
        }
      }
    }
  }

  operator fun get(setting: GameSetting): String? =
    sp.getString(key(setting), null) ?: legacy?.let { sp.getString("$it/${setting.key}", null) }

  operator fun set(setting: GameSetting, value: String?) = sp.edit {
    if (value == null) remove(key(setting)) else putString(key(setting), value)
  }

  fun clear() = sp.edit { GameSetting.entries.forEach { remove(key(it)) } }

  val isEmpty: Boolean get() = GameSetting.entries.none { sp.contains(key(it)) }

  private fun key(setting: GameSetting) = "$game/${setting.key}"

  companion object {
    const val NAME = "per_game_settings"
    private const val PATH = "path:"

    /**
     * A disc's game: "<title ID>.<version>" (54430006.5 and 54430006.6 are
     * Dead or Alive Ultimate's two discs); its path when the disc has no ID.
     */
    fun gameKey(titleId: Long, version: Long, relativePath: String?): String =
      if (titleId != 0L) "%08X.%d".format(Locale.ROOT, titleId, version) else PATH + relativePath.orEmpty()
  }
}

/** What a run uses: the app's settings with the game's overrides on top. */
class RunSettings(private val app: AppPrefs, private val game: GameOverrides?) {
  private fun bool(s: GameSetting, global: Boolean) = game?.get(s)?.toBooleanStrictOrNull() ?: global
  private fun text(s: GameSetting, global: String) = game?.get(s) ?: global

  val uncapped get() = bool(GameSetting.UNCAPPED, app.uncapped)
  val framePace get() = app.framePace
  val renderScale get() = game?.get(GameSetting.RENDER_SCALE)?.toIntOrNull() ?: app.renderScale
  val asyncShaders get() = bool(GameSetting.ASYNC_SHADERS, app.asyncShaders)
  val prebuildShaders get() = bool(GameSetting.PREBUILD_SHADERS, app.prebuildShaders)
  /** A debug build's setting (off for A/B); always on in the others. */
  val consoleTiming get() = !BuildConfig.DEBUG || bool(GameSetting.CONSOLE_TIMING, app.consoleTiming)
  val aspectRatio get() = text(GameSetting.ASPECT, app.aspectRatio)
  val fxaa get() = bool(GameSetting.FXAA, app.fxaa)
  val upscaleFilter get() = text(GameSetting.UPSCALE, app.upscaleFilter)
  val language get() = text(GameSetting.LANGUAGE, app.language)
  val screenFormat get() = text(GameSetting.SCREEN_FORMAT, app.screenFormat)
  val video480p get() = bool(GameSetting.VIDEO_480P, app.video480p)
  val video720p get() = bool(GameSetting.VIDEO_720P, app.video720p)
  val video1080i get() = bool(GameSetting.VIDEO_1080I, app.video1080i)
  val pal60 get() = bool(GameSetting.PAL60, app.pal60)
  val avPack get() = text(GameSetting.AV_PACK, app.avPack)

  fun videoFlags(): Int = AppPrefs.videoFlags(screenFormat, video480p, video720p, video1080i, pal60)
  fun xboxLanguage(): Int = AppPrefs.xboxLanguage(language)
}
