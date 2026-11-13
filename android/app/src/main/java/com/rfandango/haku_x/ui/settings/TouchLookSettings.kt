package com.rfandango.haku_x.ui.settings

import androidx.compose.foundation.clickable
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material3.ListItem
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import com.rfandango.haku_x.R
import com.rfandango.haku_x.data.TouchLookConfig
import com.rfandango.haku_x.data.TouchLookStore
import com.rfandango.haku_x.ui.common.ChoiceDialog
import com.rfandango.haku_x.ui.common.Hint
import com.rfandango.haku_x.ui.common.Section
import com.rfandango.haku_x.ui.common.SwitchItem

private enum class LookDialog { SENSITIVITY, VERTICAL, DEAD_ZONE, ACCEL, SMOOTHING, AREA, TAP_GAP, TURBO_RATE }

/** Not one of the pad's buttons: marks no choice in the list of them (None is a choice too). */
private const val NO_CHOICE = "-"

/**
 * Swipe to look (TouchLookEngine): a finger dragged over the screen turns the camera, in place of
 * the on-screen right stick; and what goes with it: the dynamic left stick, the custom buttons (T, 0,
 * 1, 2 in the controls editor), the double taps, the turbo and the axes' inversion.
 *
 * gameKey null: the defaults for every game. A game's key: that game's own (the game's menu, Touch camera,
 * sets the same): it shows the app's until the game is given its own by changing one, and can go back to
 * the app's. Each change is kept at once (TouchLookStore) and read at the next game start.
 */
@Composable
fun TouchLookSettings(gameKey: String? = null) {
  val context = LocalContext.current
  var config by remember(gameKey) {
    mutableStateOf(if (gameKey == null) TouchLookStore.global(context) else TouchLookStore.forGame(context, gameKey))
  }
  var own by remember(gameKey) { mutableStateOf(gameKey != null && TouchLookStore.hasOwn(context, gameKey)) }
  var dialog by remember { mutableStateOf<LookDialog?>(null) }
  // choosing what a source presses: which source, the step (first button, then second), the first one chosen
  var mapSource by remember { mutableStateOf<Int?>(null) }
  var mapStep by remember { mutableStateOf(0) }
  var mapFirst by remember { mutableStateOf("") }
  fun change(next: TouchLookConfig) {
    config = next
    if (gameKey == null) {
      TouchLookStore.setGlobal(context, next)
    } else {
      TouchLookStore.setGame(context, gameKey, next)
      own = true
    }
  }

  Section(stringResource(R.string.settings_section_look))
  if (gameKey == null) {
    Hint(stringResource(R.string.settings_look_desc))
  } else {
    Hint(stringResource(R.string.settings_look_game_desc))
    if (own) {
      ListItem(
        headlineContent = { Text(stringResource(R.string.look_game_own)) },
        supportingContent = { Text(stringResource(R.string.look_game_use_app)) },
        modifier = Modifier.clickable {
          TouchLookStore.setGame(context, gameKey, null)
          config = TouchLookStore.global(context)
          own = false
        },
      )
    } else {
      ListItem(headlineContent = { Text(stringResource(R.string.look_game_follows_app)) })
    }
  }
  ListItem(
    headlineContent = { Text(stringResource(R.string.settings_look_sensitivity)) },
    supportingContent = { Text("${config.sensitivity}%. " + stringResource(R.string.settings_look_sensitivity_desc)) },
    modifier = Modifier.clickable { dialog = LookDialog.SENSITIVITY },
  )
  ListItem(
    headlineContent = { Text(stringResource(R.string.settings_look_vertical)) },
    supportingContent = {
      Text(
        (if (config.sensitivityY == TouchLookConfig.SAME) stringResource(R.string.look_vertical_same) else "${config.sensitivityY}%") +
          ". " + stringResource(R.string.settings_look_vertical_desc),
      )
    },
    modifier = Modifier.clickable { dialog = LookDialog.VERTICAL },
  )
  ListItem(
    headlineContent = { Text(stringResource(R.string.settings_look_deadzone)) },
    supportingContent = { Text("${config.deadZone}%. " + stringResource(R.string.settings_look_deadzone_desc)) },
    modifier = Modifier.clickable { dialog = LookDialog.DEAD_ZONE },
  )
  ListItem(
    headlineContent = { Text(stringResource(R.string.settings_look_accel)) },
    supportingContent = {
      Text(stringResource(TouchLookConfig.accelLabel(config.accel)) + ". " + stringResource(R.string.settings_look_accel_desc))
    },
    modifier = Modifier.clickable { dialog = LookDialog.ACCEL },
  )
  ListItem(
    headlineContent = { Text(stringResource(R.string.settings_look_smoothing)) },
    supportingContent = {
      Text(
        stringResource(TouchLookConfig.smoothingLabel(config.smoothing)) + ". " +
          stringResource(R.string.settings_look_smoothing_desc),
      )
    },
    modifier = Modifier.clickable { dialog = LookDialog.SMOOTHING },
  )
  ListItem(
    headlineContent = { Text(stringResource(R.string.settings_look_area)) },
    supportingContent = { Text(stringResource(TouchLookConfig.areaLabel(config.area))) },
    modifier = Modifier.clickable { dialog = LookDialog.AREA },
  )
  SwitchItem(
    title = stringResource(R.string.settings_look_invert_x),
    summary = stringResource(R.string.settings_look_invert_desc),
    checked = config.invertX,
    onChange = { change(config.copy(invertX = it)) },
  )
  SwitchItem(
    title = stringResource(R.string.settings_look_invert_y),
    summary = stringResource(R.string.settings_look_invert_desc),
    checked = config.invertY,
    onChange = { change(config.copy(invertY = it)) },
  )
  DynamicStickRow(
    checked = config.dynamicStick,
    enabled = config.area != TouchLookConfig.AREA_ALL,
    onChange = { change(config.copy(dynamicStick = it)) },
  )

  Section(stringResource(R.string.settings_section_look_buttons))
  Hint(stringResource(R.string.settings_look_buttons_desc))
  for (source in 0 until TouchLookConfig.SOURCES) {
    ListItem(
      headlineContent = { Text(sourceName(source)) },
      supportingContent = { Text(mappingText(config, source)) },
      modifier = Modifier.clickable {
        mapSource = source
        mapStep = 0
      },
    )
  }
  ListItem(
    headlineContent = { Text(stringResource(R.string.settings_look_tap_gap)) },
    supportingContent = { Text("${config.tapMs} ms. " + stringResource(R.string.settings_look_tap_gap_desc)) },
    modifier = Modifier.clickable { dialog = LookDialog.TAP_GAP },
  )

  Section(stringResource(R.string.settings_section_look_turbo))
  Hint(stringResource(R.string.settings_look_turbo_desc))
  for (source in 0 until TouchLookConfig.SOURCES) {
    SwitchItem(
      title = sourceName(source),
      summary = mappingText(config, source),
      checked = config.hasTurbo(source),
      onChange = { change(config.withTurbo(source, it)) },
    )
  }
  ListItem(
    headlineContent = { Text(stringResource(R.string.settings_look_turbo_rate)) },
    supportingContent = { Text("${config.turboMs} ms. " + stringResource(R.string.settings_look_turbo_rate_desc)) },
    modifier = Modifier.clickable { dialog = LookDialog.TURBO_RATE },
  )

  val source = mapSource
  if (source != null) {
    ChoiceDialog(
      title = stringResource(
        if (mapStep == 0) R.string.look_pick_first else R.string.look_pick_second,
        sourceName(source),
      ),
      options = (listOf("") + TouchLookConfig.TARGETS).map {
        it to (if (it.isEmpty()) stringResource(R.string.look_target_none) else stringResource(TouchLookConfig.targetLabel(it)))
      },
      current = NO_CHOICE,
      onPick = { picked ->
        if (mapStep == 0) {
          mapFirst = picked
          mapStep = 1
        } else {
          change(config.withTargets(source, listOf(mapFirst, picked)))
          mapSource = null
          mapStep = 0
        }
      },
      onDismiss = {
        mapSource = null
        mapStep = 0
      },
    )
  }

  when (dialog) {
    LookDialog.SENSITIVITY -> ChoiceDialog(
      title = stringResource(R.string.settings_look_sensitivity),
      options = TouchLookConfig.SENSITIVITIES.map { it to "$it%" },
      current = config.sensitivity,
      onPick = {
        change(config.copy(sensitivity = it))
        dialog = null
      },
      onDismiss = { dialog = null },
    )
    LookDialog.VERTICAL -> ChoiceDialog(
      title = stringResource(R.string.settings_look_vertical),
      options = TouchLookConfig.VERTICALS.map {
        it to (if (it == TouchLookConfig.SAME) stringResource(R.string.look_vertical_same) else "$it%")
      },
      current = config.sensitivityY,
      onPick = {
        change(config.copy(sensitivityY = it))
        dialog = null
      },
      onDismiss = { dialog = null },
    )
    LookDialog.DEAD_ZONE -> ChoiceDialog(
      title = stringResource(R.string.settings_look_deadzone),
      options = TouchLookConfig.DEAD_ZONES.map { it to "$it%" },
      current = config.deadZone,
      onPick = {
        change(config.copy(deadZone = it))
        dialog = null
      },
      onDismiss = { dialog = null },
    )
    LookDialog.ACCEL -> ChoiceDialog(
      title = stringResource(R.string.settings_look_accel),
      options = TouchLookConfig.ACCELS.map { it to stringResource(TouchLookConfig.accelLabel(it)) },
      current = config.accel,
      onPick = {
        change(config.copy(accel = it))
        dialog = null
      },
      onDismiss = { dialog = null },
    )
    LookDialog.SMOOTHING -> ChoiceDialog(
      title = stringResource(R.string.settings_look_smoothing),
      options = TouchLookConfig.SMOOTHINGS.map { it to stringResource(TouchLookConfig.smoothingLabel(it)) },
      current = config.smoothing,
      onPick = {
        change(config.copy(smoothing = it))
        dialog = null
      },
      onDismiss = { dialog = null },
    )
    LookDialog.AREA -> ChoiceDialog(
      title = stringResource(R.string.settings_look_area),
      options = TouchLookConfig.AREAS.map { it to stringResource(TouchLookConfig.areaLabel(it)) },
      current = config.area,
      onPick = {
        // the dynamic stick takes the left half, which the whole screen would give to the look
        change(config.copy(area = it, dynamicStick = config.dynamicStick && it != TouchLookConfig.AREA_ALL))
        dialog = null
      },
      onDismiss = { dialog = null },
    )
    LookDialog.TAP_GAP -> ChoiceDialog(
      title = stringResource(R.string.settings_look_tap_gap),
      options = TouchLookConfig.TAP_GAPS.map { it to "$it ms" },
      current = config.tapMs,
      onPick = {
        change(config.copy(tapMs = it))
        dialog = null
      },
      onDismiss = { dialog = null },
    )
    LookDialog.TURBO_RATE -> ChoiceDialog(
      title = stringResource(R.string.settings_look_turbo_rate),
      options = TouchLookConfig.TURBO_PERIODS.map { it to "$it ms" },
      current = config.turboMs,
      onPick = {
        change(config.copy(turboMs = it))
        dialog = null
      },
      onDismiss = { dialog = null },
    )
    null -> {}
  }
}

/** The dynamic joystick's switch: off and unchangeable while the whole screen turns the camera. */
@Composable
private fun DynamicStickRow(checked: Boolean, enabled: Boolean, onChange: (Boolean) -> Unit) {
  ListItem(
    headlineContent = { Text(stringResource(R.string.settings_look_dynamic)) },
    supportingContent = {
      Text(
        stringResource(R.string.settings_look_dynamic_desc) +
          if (enabled) "" else " " + stringResource(R.string.look_dynamic_needs_right),
      )
    },
    trailingContent = { Switch(checked = checked && enabled, onCheckedChange = null, enabled = enabled) },
    modifier = Modifier.toggleable(value = checked, enabled = enabled, role = Role.Switch, onValueChange = onChange),
  )
}

/** What a source is called: a custom button, or a double tap on a half of the screen. */
@Composable
private fun sourceName(source: Int): String = when (source) {
  TouchLookConfig.TAP_LEFT -> stringResource(R.string.look_tap_left)
  TouchLookConfig.TAP_RIGHT -> stringResource(R.string.look_tap_right)
  else -> stringResource(R.string.look_button_n, source)
}

/** The pad buttons a source presses, as text ("A + R", or None). */
@Composable
private fun mappingText(config: TouchLookConfig, source: Int): String {
  val targets = config.targets(source)
  if (targets.isEmpty()) return stringResource(R.string.look_target_none)
  return targets.map { stringResource(TouchLookConfig.targetLabel(it)) }.joinToString(" + ")
}
