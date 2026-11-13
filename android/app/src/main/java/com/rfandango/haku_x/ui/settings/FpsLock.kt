package com.rfandango.haku_x.ui.settings

import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringResource
import com.rfandango.haku_x.R

/** The FPS lock's rates in Hz; 0: none. */
internal val FPS_LOCKS = listOf(0, 30, 45, 60, 90, 120, 144)

@Composable
internal fun fpsLockLabel(value: Int) =
  if (value == 0) stringResource(R.string.settings_fps_lock_off) else stringResource(R.string.settings_fps_lock_value, value)
