package com.rfandango.haku_x.ui.controllers

import android.os.SystemClock
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.rfandango.haku_x.R
import com.rfandango.haku_x.data.Binding
import com.rfandango.haku_x.data.Controllers
import com.rfandango.haku_x.data.PadOut
import com.rfandango.haku_x.data.PadProfile
import com.rfandango.haku_x.data.PadState
import com.rfandango.haku_x.data.XInput
import com.rfandango.haku_x.input.InputCapture
import com.rfandango.haku_x.ui.common.ActionItem
import com.rfandango.haku_x.ui.common.EmptyText
import com.rfandango.haku_x.ui.common.Section
import com.rfandango.haku_x.ui.common.SubScreen
import com.rfandango.haku_x.ui.common.SwitchItem
import kotlinx.coroutines.delay

private const val TEST_IDLE_MS = 4000L

/** The Duke's inputs, as the screen names them. */
private val INPUT_NAMES = mapOf(
  XInput.A to R.string.xinput_a, XInput.B to R.string.xinput_b, XInput.X to R.string.xinput_x,
  XInput.Y to R.string.xinput_y, XInput.BLACK to R.string.xinput_black, XInput.WHITE to R.string.xinput_white,
  XInput.START to R.string.xinput_start, XInput.BACK to R.string.xinput_back,
  XInput.LTHUMB to R.string.xinput_lthumb, XInput.RTHUMB to R.string.xinput_rthumb,
  XInput.DPAD_UP to R.string.xinput_dpad_up, XInput.DPAD_DOWN to R.string.xinput_dpad_down,
  XInput.DPAD_LEFT to R.string.xinput_dpad_left, XInput.DPAD_RIGHT to R.string.xinput_dpad_right,
  XInput.LT to R.string.xinput_lt, XInput.RT to R.string.xinput_rt,
  XInput.LS_X to R.string.xinput_ls_x, XInput.LS_Y to R.string.xinput_ls_y,
  XInput.RS_X to R.string.xinput_rs_x, XInput.RS_Y to R.string.xinput_rs_y,
)

/**
 * The binding an event makes for input, against the axes at rest when the
 * capture began: a key pressed (not for a stick), or the axis moved furthest
 * past half its range, in the direction it moved (for a stick, the user
 * pushes right or up). Of two axes moving together (a trigger reported as
 * LTRIGGER and BRAKE), the first in axis order.
 */
private fun captured(e: android.view.InputEvent, input: XInput, rest: FloatArray): Binding? {
  if (e is KeyEvent) {
    if (input.stick || e.action != KeyEvent.ACTION_DOWN || e.repeatCount != 0) return null
    return Binding.Key(e.keyCode)
  }
  if (e !is MotionEvent || e.actionMasked != MotionEvent.ACTION_MOVE) return null
  var best = -1
  var delta = 0f
  for (a in rest.indices) {
    val d = e.getAxisValue(a) - rest[a]
    if (kotlin.math.abs(d) > kotlin.math.abs(delta)) {
      delta = d
      best = a
    }
  }
  if (best < 0 || kotlin.math.abs(delta) < 0.01f) return null
  return Binding.Axis(best, if (delta < 0) -1 else 1)
}

/** What the controller's Duke is doing, as one line. */
private fun describe(o: PadOut): String {
  val parts = mutableListOf<String>()
  for (input in XInput.entries) {
    if (input.stick || !o.active(input)) continue
    parts += input.name
  }
  fun stick(x: Int, y: Int) = "(%.2f, %.2f)".format(x / 32767f, y / 32767f)
  if (o.lx != 0 || o.ly != 0) parts += "LS " + stick(o.lx, o.ly)
  if (o.rx != 0 || o.ry != 0) parts += "RS " + stick(o.rx, o.ry)
  return parts.joinToString("  ")
}

/**
 * One controller's mapping onto the Xbox controller: tap an input, press the
 * button or move the stick that should be it. The mapping is the
 * controller model's (every controller of that model), or this controller's
 * own; the test line shows the Duke as the game will see it.
 */
@Composable
fun ControllerMappingScreen(deviceId: Int, onBack: () -> Unit) {
  val context = LocalContext.current
  val device = InputDevice.getDevice(deviceId)
  var config by remember { mutableStateOf(Controllers.load(context)) }
  var unit by remember { mutableStateOf(device != null && config.hasUnitProfile(device)) }
  var profile by remember { mutableStateOf(device?.let { config.profileFor(it) } ?: Controllers.defaults(null)) }
  var capturing by remember { mutableStateOf<XInput?>(null) }
  var rest by remember { mutableStateOf(FloatArray(0)) }
  var live by remember { mutableStateOf("") }
  var testing by remember { mutableStateOf(false) }
  var lastInput by remember { mutableLongStateOf(0L) }
  val pad = remember { PadState() }

  fun store(p: PadProfile, ownUnit: Boolean = unit) {
    val d = device ?: return
    val profiles = config.profiles.toMutableMap()
    val names = config.names.toMutableMap()
    profiles.remove(Controllers.unitKey(d))
    val key = if (ownUnit) Controllers.unitKey(d) else Controllers.modelKey(d)
    profiles[key] = p
    names[key] = d.name ?: ""
    config = config.copy(profiles = profiles, names = names)
    Controllers.save(context, config)
    profile = p
  }

  /* back to what the controller had: its model's buttons, or the defaults (not stored, so they stay current) */
  fun reset() {
    val d = device ?: return
    val key = if (unit) Controllers.unitKey(d) else Controllers.modelKey(d)
    config = config.copy(profiles = config.profiles - key, names = config.names - key)
    Controllers.save(context, config)
    unit = config.hasUnitProfile(d)
    profile = config.profileFor(d)
  }

  DisposableEffect(deviceId) {
    InputCapture.listener = { e ->
      if (e.deviceId != deviceId) {
        false
      } else {
        when (e) {
          is KeyEvent -> pad.onKey(e)
          is MotionEvent -> pad.onMotion(e)
        }
        val input = capturing
        when {
          input != null -> {
            captured(e, input, rest)?.let { b ->
              store(profile.copy(bind = profile.bind + (input to listOf(b))))
              capturing = null
            }
            true      /* while capturing, the controller's input is the capture's */
          }
          testing -> {
            /* the test's too: else A would select (Android's fallback) and the sticks move the focus */
            live = describe(pad.compute(profile))
            lastInput = SystemClock.uptimeMillis()
            true
          }
          else -> false
        }
      }
    }
    onDispose { InputCapture.listener = null }
  }

  /* a test ends a few seconds after the controller's last input (a controller-only user's way out) */
  LaunchedEffect(testing) {
    while (testing) {
      delay(500)
      if (SystemClock.uptimeMillis() - lastInput > TEST_IDLE_MS) {
        testing = false
        live = ""
      }
    }
  }

  BackHandler(enabled = capturing != null) { capturing = null }

  Box(Modifier.fillMaxSize()) {
    SubScreen(title = stringResource(R.string.controllers_mapping_title), subtitle = device?.name, onBack = onBack) { padding ->
      Column(Modifier.padding(padding).fillMaxSize().verticalScroll(rememberScrollState())) {
        if (device == null) {
          EmptyText(stringResource(R.string.controllers_gone))
          return@Column
        }
        ListItem(
          headlineContent = { Text(stringResource(R.string.controllers_test)) },
          supportingContent = {
            Text(
              when {
                !testing -> stringResource(R.string.controllers_test_off)
                else -> live.ifEmpty { stringResource(R.string.controllers_test_idle) }
              },
            )
          },
          modifier = Modifier.clickable {
            testing = !testing
            live = ""
            lastInput = SystemClock.uptimeMillis()
          },
        )
        SwitchItem(
          title = stringResource(R.string.controllers_own),
          summary = stringResource(R.string.controllers_own_desc),
          checked = unit,
          onChange = { unit = it; store(profile, it) },
        )
        ActionItem(
          title = stringResource(R.string.controllers_reset),
          summary = stringResource(R.string.controllers_reset_desc),
          icon = null,
        ) { reset() }
        ActionItem(title = stringResource(R.string.controllers_swap_face), summary = null, icon = null) {
          store(profile.swapFaceButtons())
        }

        Section(stringResource(R.string.controllers_inputs))
        for (input in XInput.entries) {
          val binds = profile.bind[input].orEmpty()
          ListItem(
            headlineContent = { Text(stringResource(INPUT_NAMES.getValue(input))) },
            supportingContent = {
              Text(binds.joinToString(" / ") { Controllers.label(it) }.ifEmpty { stringResource(R.string.controllers_unbound) })
            },
            modifier = Modifier.clickable {
              rest = pad.axes.copyOf()
              testing = false
              capturing = input
            },
          )
        }

        Section(stringResource(R.string.controllers_sticks))
        LabeledSlider(stringResource(R.string.controllers_deadzone_l), profile.deadzoneL, 0f..0.5f) {
          store(profile.copy(deadzoneL = it))
        }
        LabeledSlider(stringResource(R.string.controllers_deadzone_r), profile.deadzoneR, 0f..0.5f) {
          store(profile.copy(deadzoneR = it))
        }
        LabeledSlider(stringResource(R.string.controllers_threshold), profile.threshold, 0.1f..0.9f) {
          store(profile.copy(threshold = it))
        }
      }
    }

    /*
     * The capture prompt is drawn in the screen, not as a dialog: a dialog is a
     * window of its own, which the controller's events go to (B closes it as
     * Back) instead of the activity's capture.
     */
    capturing?.let { input ->
      Box(
        Modifier.fillMaxSize()
          .background(Color.Black.copy(alpha = 0.32f))
          .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) { capturing = null },
        contentAlignment = Alignment.Center,
      ) {
        Surface(
          shape = MaterialTheme.shapes.extraLarge,
          tonalElevation = 6.dp,
          modifier = Modifier.widthIn(max = 560.dp).padding(24.dp)
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {},
        ) {
          Column(Modifier.padding(24.dp)) {
            Text(stringResource(INPUT_NAMES.getValue(input)), style = MaterialTheme.typography.headlineSmall)
            Text(
              stringResource(
                if (input.stick) R.string.controllers_capture_stick else R.string.controllers_capture,
                device?.name ?: "",
              ),
              style = MaterialTheme.typography.bodyMedium,
              modifier = Modifier.padding(top = 16.dp, bottom = 24.dp),
            )
            Row(Modifier.align(Alignment.End), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
              TextButton(onClick = { capturing = null }) { Text(stringResource(android.R.string.cancel)) }
              TextButton(onClick = {
                store(profile.copy(bind = profile.bind - input))
                capturing = null
              }) { Text(stringResource(R.string.controllers_clear)) }
            }
          }
        }
      }
    }
  }
}

/** A slider with its value; a controller's left and right move it by 0.01 (Material's takes no keys). */
@Composable
private fun LabeledSlider(title: String, value: Float, range: ClosedFloatingPointRange<Float>, onDone: (Float) -> Unit) {
  var v by remember(value) { mutableStateOf(value) }
  Column(Modifier.padding(horizontal = 16.dp, vertical = 4.dp)) {
    Text("$title: %.2f".format(v), style = MaterialTheme.typography.bodyMedium)
    Slider(
      value = v,
      onValueChange = { v = it },
      valueRange = range,
      onValueChangeFinished = { onDone(v) },
      modifier = Modifier.onKeyEvent { e ->
        val step = when (e.key) {
          Key.DirectionLeft -> -0.01f
          Key.DirectionRight -> 0.01f
          else -> return@onKeyEvent false
        }
        if (e.type == KeyEventType.KeyDown) v = (v + step).coerceIn(range) else onDone(v)
        true
      },
    )
  }
}
