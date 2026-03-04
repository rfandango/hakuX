package com.rfandango.xemuandroid

import android.content.Context
import android.content.SharedPreferences

class ControllerSettings(context: Context) {
  private val prefs: SharedPreferences = context.getSharedPreferences(
    "controller_settings",
    Context.MODE_PRIVATE
  )

  companion object {
    private const val KEY_SHOW_ON_SCREEN_CONTROLLER = "show_on_screen_controller"
    private const val KEY_CONTROLLER_OPACITY = "controller_opacity"
    private const val KEY_CONTROLLER_SCALE = "controller_scale"
    private const val KEY_VIBRATION_ENABLED = "vibration_enabled"
  }

  var showOnScreenController: Boolean
    get() = prefs.getBoolean(KEY_SHOW_ON_SCREEN_CONTROLLER, true)
    set(value) = prefs.edit().putBoolean(KEY_SHOW_ON_SCREEN_CONTROLLER, value).apply()

  var controllerOpacity: Float
    get() = prefs.getFloat(KEY_CONTROLLER_OPACITY, 0.7f)
    set(value) = prefs.edit().putFloat(KEY_CONTROLLER_OPACITY, value.coerceIn(0f, 1f)).apply()

  var controllerScale: Float
    get() = prefs.getFloat(KEY_CONTROLLER_SCALE, 1.0f)
    set(value) = prefs.edit().putFloat(KEY_CONTROLLER_SCALE, value.coerceIn(0.5f, 2.0f)).apply()

  var vibrationEnabled: Boolean
    get() = prefs.getBoolean(KEY_VIBRATION_ENABLED, true)
    set(value) = prefs.edit().putBoolean(KEY_VIBRATION_ENABLED, value).apply()
}
