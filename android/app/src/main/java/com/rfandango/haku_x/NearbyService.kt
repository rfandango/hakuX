package com.rfandango.haku_x

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.IBinder
import androidx.core.app.NotificationCompat
import android.widget.Toast
import androidx.core.app.ServiceCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import com.rfandango.haku_x.data.Nearby

/**
 * Runs nearby system link (data/Nearby.kt) while games run in the core's process: it keeps the app's,
 * which the library leaves as a game starts. Its notification says who is linked (and a toast when that
 * changes) and ends it (Stop); going back to the library ends it too (LauncherActivity).
 */
class NearbyService : Service() {
  override fun onBind(intent: Intent?): IBinder? = null

  override fun onDestroy() {
    scope.cancel()
    Nearby.stop()
    super.onDestroy()
  }

  private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

  override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
    if (intent?.action == ACTION_STOP) {
      stopSelf()
      return START_NOT_STICKY
    }
    val nm = getSystemService(NotificationManager::class.java)
    nm.createNotificationChannel(
      NotificationChannel(CHANNEL, getString(R.string.nearby_channel), NotificationManager.IMPORTANCE_LOW))
    ServiceCompat.startForeground(this, 1, notification(Nearby.status.value),
      ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
    Nearby.start(this)
    scope.launch {
      var linked = 0
      Nearby.status.collect { st ->
        nm.notify(1, notification(st))
        if (st.linked != linked) {
          Toast.makeText(this@NearbyService,
            if (st.linked > 0) resources.getQuantityString(R.plurals.nearby_linked, st.linked, st.linked)
            else getString(R.string.nearby_unlinked), Toast.LENGTH_SHORT).show()
          linked = st.linked
        }
      }
    }
    return START_NOT_STICKY
  }

  private fun notification(st: Nearby.Status): Notification {
    val stop = PendingIntent.getService(this, 0, Intent(this, NearbyService::class.java).setAction(ACTION_STOP),
      PendingIntent.FLAG_IMMUTABLE)
    return NotificationCompat.Builder(this, CHANNEL)
      .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
      .setContentTitle(getString(R.string.nearby_notification_title))
      .setContentText(
        if (st.linked > 0) resources.getQuantityString(R.plurals.nearby_linked, st.linked, st.linked)
        else getString(R.string.nearby_notification_text))
      .setOngoing(true)
      .addAction(0, getString(R.string.nearby_stop), stop)
      .build()
  }

  companion object {
    private const val CHANNEL = "nearby"
    private const val ACTION_STOP = "stop"
    fun start(context: Context) {
      context.startForegroundService(Intent(context, NearbyService::class.java))
    }

    fun stop(context: Context) {
      Nearby.stop()
      context.stopService(Intent(context, NearbyService::class.java))
    }
  }
}
