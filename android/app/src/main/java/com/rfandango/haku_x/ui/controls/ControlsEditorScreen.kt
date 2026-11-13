package com.rfandango.haku_x.ui.controls

import android.content.pm.ActivityInfo
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.ViewGroup
import android.widget.FrameLayout
import androidx.activity.compose.BackHandler
import androidx.activity.compose.LocalActivity
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.rfandango.haku_x.OnScreenController
import com.rfandango.haku_x.overlay.OverlayEditorBar

/**
 * The on-screen controller's layout editor outside a game: the same
 * controller and editor bar as in-game, full screen in landscape like the
 * game, over a dark backdrop. With a game's key it edits that game's own
 * layout (kept for it alone); without one, the app's, which every game uses
 * until it has one of its own.
 */
@Composable
fun ControlsEditorScreen(gameKey: String? = null, onClose: () -> Unit) {
  val activity = LocalActivity.current
  DisposableEffect(activity) {
    val window = activity?.window
    val insets = window?.let { WindowCompat.getInsetsController(it, it.decorView) }
    activity?.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
    insets?.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
    insets?.hide(WindowInsetsCompat.Type.systemBars())
    onDispose {
      insets?.show(WindowInsetsCompat.Type.systemBars())
      activity?.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR
    }
  }

  val controller = remember { arrayOfNulls<OnScreenController>(1) }
  BackHandler {
    controller[0]?.reloadLayout() // back: drop the edits
    onClose()
  }

  AndroidView(
    modifier = Modifier.fillMaxSize(),
    factory = { ctx ->
      FrameLayout(ctx).apply {
        background = GradientDrawable(
          GradientDrawable.Orientation.TL_BR,
          intArrayOf(0xFF2B3A2E.toInt(), 0xFF141A16.toInt(), 0xFF0A0C0B.toInt()),
        )
        val overlay = OnScreenController(ctx).apply {
          editing = true
          layoutKey = gameKey // before it is sized: that is when the layout is loaded
        }
        controller[0] = overlay
        addView(overlay, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        addView(
          OverlayEditorBar(ctx, overlay) { onClose() },
          FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.TOP or Gravity.CENTER_HORIZONTAL,
          ),
        )
      }
    },
  )
}
