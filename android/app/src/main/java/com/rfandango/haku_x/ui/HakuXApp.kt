package com.rfandango.haku_x.ui

import android.net.Uri
import android.widget.Toast
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navArgument
import com.rfandango.haku_x.GpuDriverHelper
import com.rfandango.haku_x.R
import com.rfandango.haku_x.data.AppPrefs
import com.rfandango.haku_x.data.Game
import com.rfandango.haku_x.data.GameOverrides
import com.rfandango.haku_x.data.SaveStateStore
import com.rfandango.haku_x.data.Updates
import com.rfandango.haku_x.ui.controllers.ControllerMappingScreen
import com.rfandango.haku_x.ui.controllers.ControllersScreen
import com.rfandango.haku_x.ui.common.ConfirmDialog
import com.rfandango.haku_x.ui.controls.ControlsEditorScreen
import com.rfandango.haku_x.ui.driver.GpuDriverScreen
import com.rfandango.haku_x.ui.hdd.HddScreen
import com.rfandango.haku_x.ui.insignia.InsigniaWizard
import com.rfandango.haku_x.ui.library.LibraryScreen
import com.rfandango.haku_x.ui.mods.ModsScreen
import com.rfandango.haku_x.ui.settings.GameSettingsScreen
import com.rfandango.haku_x.ui.settings.SettingsScreen
import com.rfandango.haku_x.ui.setup.MigrateScreen
import com.rfandango.haku_x.ui.setup.SetupScreen
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

private object Routes {
  const val MIGRATE = "migrate"
  /** The migration again, from the settings: back there when done. */
  const val MIGRATE_AGAIN = "migrate_again"
  const val SETUP = "setup"
  const val LIBRARY = "library"
  const val SETTINGS = "settings"
  const val CONTROLS = "controls"
  const val CONTROLLERS = "controllers"
  const val HDD = "hdd"
  const val GPU_DRIVER = "gpu_driver"
  /** Insignia's setup wizard (Xbox Live). */
  const val INSIGNIA = "insignia"
  /** One controller's mapping: its input device id. */
  const val MAPPING = "mapping/{id}"

  fun mapping(id: Int) = "mapping/$id"
  /** One game's settings: its override key and its title. */
  const val GAME = "game?key={key}&title={title}"

  fun game(key: String, title: String) = "game?key=${Uri.encode(key)}&title=${Uri.encode(title)}"
  /** One disc's mods and cheats: its game key and title. */
  const val MODS = "mods?key={key}&title={title}"

  fun mods(key: String, title: String) = "mods?key=${Uri.encode(key)}&title=${Uri.encode(title)}"
}

/**
 * The app's screens: the first start after the update from the xemu-based release (its data brought
 * over), first-run setup, the game library, the settings, a game's settings.
 */
@Composable
fun HakuXApp(
  needsMigration: Boolean,
  needsSetup: Boolean,
  onLaunchGame: (Game) -> Unit,
  /** Start a game right into one of its savestates (the library's Load state). */
  onLaunchState: (Game, SaveStateStore, Int) -> Unit,
  /** Start the dashboard (installed on C:), with no disc. */
  onLaunchDashboard: () -> Unit,
  /** Start a disc image that isn't a library game (Insignia's Setup Assistant). */
  onLaunchSetup: (Uri) -> Unit,
  /** Insignia's setup started a game that has closed: the setup opens again, over the library. */
  openInsignia: Boolean = false,
) {
  val nav = rememberNavController()
  // A newer release on GitHub, looked for once as the app's screens start: told in a toast, the
  // library's update button then shows it (Updates).
  val context = LocalContext.current
  LaunchedEffect(Unit) {
    Updates.checkAtStart(context)?.let {
      Toast.makeText(context, context.getString(R.string.update_toast, it.version), Toast.LENGTH_LONG).show()
    }
  }
  val start = when {
    needsMigration -> Routes.MIGRATE
    needsSetup -> Routes.SETUP
    else -> Routes.LIBRARY
  }
  LaunchedEffect(Unit) {
    if (openInsignia && start == Routes.LIBRARY) nav.navigate(Routes.INSIGNIA)
  }
  NavHost(nav, startDestination = start) {
    composable(Routes.MIGRATE) {
      MigrateScreen(onDone = {
        nav.navigate(if (needsSetup) Routes.SETUP else Routes.LIBRARY) { popUpTo(Routes.MIGRATE) { inclusive = true } }
      })
    }
    composable(Routes.SETUP) {
      SetupScreen(onDone = {
        nav.navigate(Routes.LIBRARY) { popUpTo(Routes.SETUP) { inclusive = true } }
      })
    }
    composable(Routes.LIBRARY) {
      // once, on a Qualcomm phone running on its own driver older than Vulkan 1.3: Turnip recommended
      var turnipNote by remember { mutableStateOf(false) }
      LaunchedEffect(Unit) {
        val prefs = AppPrefs(context)
        if (!prefs.turnipNoteShown && withContext(Dispatchers.IO) { GpuDriverHelper.turnipRecommended(context) }) {
          prefs.turnipNoteShown = true
          turnipNote = true
        }
      }
      if (turnipNote) {
        ConfirmDialog(
          title = stringResource(R.string.turnip_note_title),
          text = stringResource(R.string.turnip_note_text),
          confirm = stringResource(R.string.turnip_note_open),
          dismiss = stringResource(R.string.turnip_note_later),
          onConfirm = {
            turnipNote = false
            nav.navigate(Routes.GPU_DRIVER)
          },
          onDismiss = { turnipNote = false },
        )
      }
      LibraryScreen(
        onLaunch = onLaunchGame,
        onLoadState = onLaunchState,
        onDashboard = onLaunchDashboard,
        onOpenSettings = { nav.navigate(Routes.SETTINGS) },
        onGameSettings = { game, disc ->
          val key = GameOverrides.gameKey(disc?.titleId ?: 0L, disc?.version ?: 0L, game.relativePath)
          nav.navigate(Routes.game(key, game.title))
        },
        onMods = { game, disc ->
          val key = GameOverrides.gameKey(disc.titleId, disc.version, game.relativePath)
          nav.navigate(Routes.mods(key, game.title))
        },
      )
    }
    composable(
      Routes.MODS,
      arguments = listOf(
        navArgument("key") { type = NavType.StringType; defaultValue = "" },
        navArgument("title") { type = NavType.StringType; defaultValue = "" },
      ),
    ) { entry ->
      ModsScreen(
        gameKey = entry.arguments?.getString("key").orEmpty(),
        title = entry.arguments?.getString("title").orEmpty(),
        onBack = { nav.popBackStack() },
      )
    }
    composable(Routes.SETTINGS) {
      SettingsScreen(
        onBack = { nav.popBackStack() },
        onEditControls = { nav.navigate(Routes.CONTROLS) },
        onControllers = { nav.navigate(Routes.CONTROLLERS) },
        onHardDisk = { nav.navigate(Routes.HDD) },
        onGpuDriver = { nav.navigate(Routes.GPU_DRIVER) },
        onMigrate = { nav.navigate(Routes.MIGRATE_AGAIN) },
        onInsignia = { nav.navigate(Routes.INSIGNIA) },
      )
    }
    composable(Routes.INSIGNIA) {
      InsigniaWizard(
        onBack = { nav.popBackStack() },
        onLaunchDashboard = onLaunchDashboard,
        onLaunchSetup = onLaunchSetup,
      )
    }
    composable(Routes.GPU_DRIVER) {
      GpuDriverScreen(onBack = { nav.popBackStack() })
    }
    composable(Routes.MIGRATE_AGAIN) {
      MigrateScreen(onDone = { nav.popBackStack() })
    }
    composable(Routes.HDD) {
      HddScreen(onBack = { nav.popBackStack() })
    }
    composable(Routes.CONTROLLERS) {
      ControllersScreen(onBack = { nav.popBackStack() }, onMap = { nav.navigate(Routes.mapping(it)) })
    }
    composable(Routes.MAPPING, arguments = listOf(navArgument("id") { type = NavType.IntType })) { entry ->
      ControllerMappingScreen(deviceId = entry.arguments?.getInt("id") ?: -1, onBack = { nav.popBackStack() })
    }
    composable(Routes.CONTROLS) {
      ControlsEditorScreen(onClose = { nav.popBackStack() })
    }
    composable(
      Routes.GAME,
      arguments = listOf(
        navArgument("key") { type = NavType.StringType; defaultValue = "" },
        navArgument("title") { type = NavType.StringType; defaultValue = "" },
      ),
    ) { entry ->
      GameSettingsScreen(
        gameKey = entry.arguments?.getString("key").orEmpty(),
        title = entry.arguments?.getString("title").orEmpty(),
        onBack = { nav.popBackStack() },
      )
    }
  }
}
