package com.rfandango.haku_x

import android.app.AlertDialog
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import android.graphics.Typeface
import android.hardware.input.InputManager
import android.net.Uri
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.util.TypedValue
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.PixelCopy
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.widget.ImageView
import android.widget.RelativeLayout
import android.widget.TextView
import android.widget.Toast
import android.window.OnBackInvokedDispatcher
import androidx.documentfile.provider.DocumentFile
import com.rfandango.haku_x.data.AppPrefs
import com.rfandango.haku_x.data.CheatInfo
import com.rfandango.haku_x.data.Controllers
import com.rfandango.haku_x.data.GameOverrides
import com.rfandango.haku_x.data.Logs
import com.rfandango.haku_x.data.ModCatalog
import com.rfandango.haku_x.data.ModChoices
import com.rfandango.haku_x.data.ModRun
import com.rfandango.haku_x.data.Network
import com.rfandango.haku_x.data.PlayerSlot
import com.rfandango.haku_x.data.RunSettings
import com.rfandango.haku_x.data.SaveStateStore
import com.rfandango.haku_x.data.XboxDisc
import com.rfandango.haku_x.input.PadRouter
import com.rfandango.haku_x.overlay.OverlayEditorBar
import com.rfandango.haku_x.ui.theme.AppTheme
import java.io.File
import java.util.TimeZone
import org.libsdl.app.SDLActivity

/**
 * The QEMU-free core: libhakuxcore.so (FEX JIT, reimplemented kernel and
 * devices, the HLE GPU) on SDL3, in its own process. SDL_main gets the disc as
 * a descriptor opened here and the core's hard disk folder in app storage.
 * The on-screen controller feeds the core directly; physical gamepads come
 * through SDL3.
 */
class NativeCoreActivity : SDLActivity(), InputManager.InputDeviceListener {
  private var discArg = ""
  private var onScreenController: OnScreenController? = null
  private var fpsView: TextView? = null
  private var hitchView: TextView? = null
  /* the settings' overlays, read as the game comes back (onResume) */
  private var fpsOn = false
  private var hitchesOn = false
  private val fpsHandler = Handler(Looper.getMainLooper())
  /** The overlays' redraws and the game's GPU work (the controls' and the lines below). */
  private val overlayPacer = OverlayPacer({ nativeGpuBusyMs() }, { nativeGpuGap() })
  /** The FPS and stutter lines' update waiting on overlayPacer since (uptime ms), 0: none. */
  private var linesWantedSince = 0L
  private val fpsRunnable = object : Runnable {
    override fun run() {
      if (linesWantedSince == 0L) {
        linesWantedSince = SystemClock.uptimeMillis()
      }
      if (!overlayPacer.mayDraw(linesWantedSince)) {
        fpsHandler.postDelayed(this, LINES_RETRY_MS)
        return
      }
      linesWantedSince = 0L
      val s = nativeGetFrameStats()
      if (fpsOn && s != null && s.size >= 3) {
        var text = "FPS: %.0f  |  %.1fms avg  /  %.1fms last".format(s[0], s[1], s[2])
        if (s[1] > 0f && s[2] > 2f * s[1]) {
          text += " *" /* the last frame took over twice the average */
        }
        fpsView?.text = text
      }
      hitchView?.takeIf { hitchesOn }?.let { v ->
        val hitches = nativeGetHleHitches()
        if (hitches.isNotEmpty()) {
          v.text = hitches
          if (v.visibility != View.VISIBLE) v.visibility = View.VISIBLE
        } else if (v.visibility != View.GONE) {
          v.visibility = View.GONE
        }
      }
      fpsHandler.postDelayed(this, FPS_INTERVAL_MS)
    }
  }

  /* A game that can't run here: the GPU driver lacks what the HLE GPU needs,
   * or the HLE GPU does not support the game. Say which and go back, instead
   * of a black screen. */
  private var fatalShown = false
  private val fatalRunnable = object : Runnable {
    override fun run() {
      val why = nativeGetFatal()
      if (why != null && !fatalShown) {
        fatalShown = true
        val kind = nativeGetFatalKind()
        val driver = kind == FATAL_DRIVER
        /* another driver can be chosen on Adreno only (Turnip); elsewhere the system's */
        val custom = GpuDriverHelper.supportsCustomDriverLoading()
        AlertDialog.Builder(this@NativeCoreActivity)
          .setTitle(
            when (kind) {
              FATAL_DRIVER -> R.string.native_fatal_title
              FATAL_STATE -> R.string.state_load_failed_title
              FATAL_GPU_LOST -> R.string.native_gpu_lost_title
              FATAL_BOOT -> R.string.native_boot_failed_title
              else -> R.string.native_incompatible_title
            }
          )
          .setMessage(
            when {
              driver -> getString(if (custom) R.string.native_fatal_gpu else R.string.native_fatal_gpu_system, why)
              kind == FATAL_STATE -> why
              kind == FATAL_GPU_LOST ->
                getString(if (custom) R.string.native_gpu_lost else R.string.native_gpu_lost_system, why)
              kind == FATAL_BOOT -> getString(R.string.native_boot_failed, why)
              else -> getString(R.string.native_incompatible)
            }
          )
          .setCancelable(false)
          .setPositiveButton(android.R.string.ok) { _, _ -> exitGame() }
          .show()
        return
      }
      /* The title quick-rebooted into another of its XBEs (XLaunchNewImage:
       * Morrowind's New Game starts morrowind.xbe): the core lives for one
       * run, so it starts again in a new process with "relaunch". */
      if (nativeGetRelaunch()) {
        relaunchCore()
        return
      }
      /* The title left (to the dashboard, a reboot, a shutdown): the game
       * closes, as on a console. */
      if (nativeGetQuit()) {
        Log.i(TAG, "the title left: closing the game")
        exitGame()
        return
      }
      nativeTakeTwinConsole()?.let(::showTwinConsole)
      fpsHandler.postDelayed(this, FPS_INTERVAL_MS)
    }
  }

  /**
   * Another console on the network has this one's identity (the app's hard
   * disk copied from one device to the other: the same serial number and MAC
   * address), and neither sees the other's system link games. Said once a
   * run, with the way out: a new identity for this console (its HD key kept,
   * so its saves stay valid), from the game's start, at once.
   */
  private var twinShown = false

  private fun showTwinConsole(address: String) {
    if (twinShown) return
    twinShown = true
    Log.i(TAG, "network: the console at $address has this one's identity")
    showMenuDialog(
      AlertDialog.Builder(this)
        .setTitle(R.string.network_twin_title)
        .setMessage(getString(R.string.network_twin_message, address))
        .setPositiveButton(R.string.network_twin_renew) { _, _ -> renewIdentity() }
        .setNegativeButton(R.string.network_twin_later, null)
    )
  }

  /** The core gives the console a new identity as it next loads the EEPROM (native/kernel/ex.c eeprom_renew). */
  private fun renewIdentity() {
    val marker = File(hddDir(), "eeprom.renew")
    if (runCatching { marker.writeText("") }.isFailure) {
      Toast.makeText(this, R.string.network_twin_failed, Toast.LENGTH_LONG).show()
      return
    }
    Log.i(TAG, "network: a new identity asked; the game starts again")
    restartGame()
  }

  /** The emulated hard disk's folder (the core's <hdd>). */
  private fun hddDir() = File(filesDir, "native/hdd")

  /** The game from its start again, in a new core process (RelaunchActivity, no frame held). */
  private fun restartGame() {
    if (relaunchStarted) return
    relaunchStarted = true
    RelaunchActivity.heldFrame(this).delete()
    startActivity(
      Intent(this, RelaunchActivity::class.java)
        .putExtra(EXTRA_DVD_URI, discUri?.toString() ?: intent.getStringExtra(EXTRA_DVD_URI))
        .putExtra(EXTRA_RESTART, true)
        .putExtra(
          LauncherActivity.EXTRA_LAUNCHED_EXTERNALLY,
          intent.getBooleanExtra(LauncherActivity.EXTRA_LAUNCHED_EXTERNALLY, false),
        )
        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK or Intent.FLAG_ACTIVITY_NO_ANIMATION),
      RelaunchActivity.noTransition(this).toBundle(),
    )
    finish()
    RelaunchActivity.noCloseTransition(this)
    android.os.Process.killProcess(android.os.Process.myPid())
  }

  override fun getLibraries(): Array<String> = arrayOf("SDL3", "hakuxcore")

  override fun getArguments(): Array<String> {
    val args = mutableListOf(discArg, hddDir().absolutePath)
    val state = intent.getStringExtra(EXTRA_LOADSTATE)
    if (state != null) {
      args += "loadstate=$state"
    } else if (intent.getBooleanExtra(EXTRA_RELAUNCH, false)) {
      args += "relaunch"
    }
    runCatching { packageManager.getPackageInfo(packageName, 0).versionName }.getOrNull()?.let {
      args += "version=$it"
    }
    /* The app's settings, under this game's overrides. */
    val settings = RunSettings(AppPrefs(this), gameOverrides())
    /* Every frame presented at once, no vblank wait: on by default. */
    if (settings.uncapped) {
      args += "uncap"
    }
    /* The decoder waits at each frame's flip for the previous frame's GPU
     * work: the game slows to the render pace instead of simulating ahead
     * of the screen. On by default. */
    args += "pace=${if (settings.framePace) 1 else 0}"
    /* The HLE GPU's render scale, 1-4x the title's resolution. */
    args += "scale=${settings.renderScale}"
    /* The picture's shape: auto (the game's), 4:3, 16:9 or stretch. */
    args += "aspect=${settings.aspectRatio}"
    /* Anti-aliasing: FXAA over each frame, and how a picture smaller than
     * the screen is scaled up (a larger one is averaged down). */
    args += "fxaa=${if (settings.fxaa) 1 else 0}"
    args += "upscale=${settings.upscaleFilter}"
    /* The console's video settings (widescreen, 480p/720p/1080i, the cable)
     * and its language. */
    args += "video=%x".format(settings.videoFlags())
    args += "avpack=${settings.avPack}"
    args += "lang=${settings.xboxLanguage()}"
    /* The console's clock (the phone's time, or as set in the settings) and
     * the phone's time zone. */
    AppPrefs(this).clockOffset?.let { args += "clock=$it" }
    args += "tz=${TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 60_000}"
    /* Shaders compiled off the render thread (draws needing one skipped
     * meanwhile), and the ones cached built before the game starts. */
    args += "asyncpipe=${if (settings.asyncShaders) 1 else 0}"
    args += "prebuild=${if (settings.prebuildShaders) 1 else 0}"
    /* The console's network (native/net): system link with the consoles on
     * this network (their broadcasts reach it under the multicast lock), or
     * through an XLink Kai engine; off, no cable. */
    val prefs = AppPrefs(this)
    when (prefs.networkMode) {
      AppPrefs.NET_LAN -> {
        args += "net=lan"
        Network.broadcastAddress(this)?.let { args += "lanbcast=$it" }
        prefs.lanPeers.replace(" ", "").takeIf { it.isNotEmpty() }?.let { args += "lanpeers=$it" }
      }
      AppPrefs.NET_KAI -> prefs.kaiEngine.trim().takeIf { it.isNotEmpty() }?.let {
        args += "net=kai"
        args += "kai=$it"
      }
    }
    /* The game's mods (their files and XBE patches) and cheats (mods/):
     * the manifest the core reads as the disc opens. */
    modsArg()?.let { args += it }
    /* The players with a controller plugged in at boot: player 1 and those a
     * connected controller plays as (later ones: syncPads). */
    plugged = Controllers.load(this).padMask(Controllers.connected())
    args += "pads=%x".format(plugged)
    /* The custom Vulkan driver installed in the settings (Turnip), loaded
     * by the core through adrenotools; none: the system driver. */
    GpuDriverHelper.init(this)
    val driverLib = GpuDriverHelper.getInstalledDriverLibrary()
    if (!driverLib.isNullOrEmpty() && GpuDriverHelper.supportsCustomDriverLoading()) {
      args += "vkdriver=${GpuDriverHelper.driverInstallDir}|$driverLib|${GpuDriverHelper.hookLibDir}"
    }
    return args.toTypedArray()
  }

  override fun onCreate(savedInstanceState: Bundle?) {
    /* the in-game menus (framework dialogs) light or dark, as the app's theme is */
    if (AppTheme.isDark(this)) {
      setTheme(R.style.Theme_HakuX_Game_Dark)
    }
    discArg = openDisc()
    Logs.startGame(this, gameName(), when {
      intent.getBooleanExtra(EXTRA_RELAUNCH, false) -> "relaunched into another XBE"
      intent.hasExtra(EXTRA_LOADSTATE) -> "loading a savestate"
      else -> "started"
    })
    super.onCreate(savedInstanceState)
    if (AppPrefs(this).networkMode == AppPrefs.NET_LAN) {
      multicastLock = Network.multicastLock(this)
    }
    setupOnScreenController()
    setupFpsOverlay()
    setupShaderBuildLine()
    if (intent.getBooleanExtra(EXTRA_RELAUNCH, false) || intent.hasExtra(EXTRA_LOADSTATE)) {
      showHeldFrame(intent.getStringExtra(EXTRA_LOADSTATE))
    }
    router = PadRouter(
      this,
      send = { p, o -> nativeSetPad(p, o.buttons, o.packedAnalog(), o.lx, o.ly, o.rx, o.ry) },
      rumbleOf = { p -> nativeGetRumble(p) },
    ).also { it.reassign() }
    (getSystemService(Context.INPUT_SERVICE) as InputManager).registerInputDeviceListener(this, null)
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      onBackInvokedDispatcher.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT) { onBack() }
    }
  }

  /**
   * The emulation stops while the activity is paused (the device suspends,
   * another app in front) or the menu is open, and goes on where it stopped
   * when neither is (nativeSetPaused).
   */
  private var activityPaused = false
  private var menuDialogs = 0
  private var nativePaused: Boolean? = null

  private fun syncPause() {
    val want = activityPaused || menuDialogs > 0
    if (nativePaused != want) {
      nativePaused = want
      nativeSetPaused(want)
    }
  }

  /**
   * A dialog of the menu (the menu, Controllers, a player's controller): the
   * emulation stops while one is open. The next one opens before the one
   * chosen from closes, so the game stays stopped from one to the next.
   */
  private fun showMenuDialog(b: AlertDialog.Builder): AlertDialog {
    menuDialogs++
    syncPause()
    b.setOnDismissListener {
      menuDialogs--
      syncPause()
    }
    return b.show()
  }

  /** Savestates (push-buffer mode): the menu's Save state and Load state. */
  private val saveStates by lazy {
    SaveStates(
      this,
      gameKey(),
      discUri?.toString() ?: intent.getStringExtra(EXTRA_DVD_URI),
      ::showMenuDialog,
      ::captureFrame,
    )
  }

  /** debug: debug.hakux.state=save:<slot> / load:<slot>, as the menu's (scripted runs). */
  private val stateRequestRunnable = object : Runnable {
    override fun run() {
      nativeTakeStateRequest()?.let { req ->
        val slot = req.substringAfter(':').toIntOrNull()
        if (slot != null && slot in 1..SaveStateStore.SLOTS && nativeStatesAvailable()) {
          when (req.substringBefore(':')) {
            "save" -> saveStates.save(slot)
            "load" -> if (saveStates.state(slot).exists()) saveStates.load(slot)
          }
        }
      }
      fpsHandler.postDelayed(this, STATE_POLL_MS)
    }
  }

  /** The frame on screen, copied from the game's surface (null: it couldn't be). */
  private fun captureFrame(done: (Bitmap?) -> Unit) {
    val surface = mSurface
    if (surface == null || surface.width <= 0 || surface.height <= 0) {
      done(null)
      return
    }
    val frame = Bitmap.createBitmap(surface.width, surface.height, Bitmap.Config.ARGB_8888)
    try {
      PixelCopy.request(surface, frame, { result -> done(if (result == PixelCopy.SUCCESS) frame else null) }, fpsHandler)
    } catch (e: IllegalArgumentException) {
      done(null)
    }
  }

  /** Starts the core again into the savestate at path (SaveStates' Load state). */
  fun restartInto(path: String) {
    if (relaunchStarted) return
    relaunchStarted = true
    Log.i(TAG, "savestate: starting the core again into $path")
    val uri = discUri?.toString() ?: intent.getStringExtra(EXTRA_DVD_URI)
    startActivity(
      Intent(this, RelaunchActivity::class.java)
        .putExtra(EXTRA_DVD_URI, uri)
        .putExtra(EXTRA_LOADSTATE, path)
        .putExtra(
          LauncherActivity.EXTRA_LAUNCHED_EXTERNALLY,
          intent.getBooleanExtra(LauncherActivity.EXTRA_LAUNCHED_EXTERNALLY, false),
        )
        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK or Intent.FLAG_ACTIVITY_NO_ANIMATION),
      RelaunchActivity.noTransition(this).toBundle(),
    )
    finish()
    RelaunchActivity.noCloseTransition(this)
    android.os.Process.killProcess(android.os.Process.myPid())
  }

  override fun onResume() {
    super.onResume()
    activityPaused = false
    syncPause() /* where it stopped (onPause), unless the menu is open */
    hideSystemBars()
    val prefs = getSharedPreferences("x1box_prefs", Context.MODE_PRIVATE)
    fpsOn = prefs.getBoolean("show_fps", true)
    hitchesOn = BuildConfig.DEBUG && prefs.getBoolean(PREF_SHOW_HITCHES, true)
    fpsView?.visibility = if (fpsOn) View.VISIBLE else View.GONE
    if (!hitchesOn) hitchView?.visibility = View.GONE
    fpsHandler.removeCallbacks(fpsRunnable)
    if (fpsOn || hitchesOn) {
      fpsHandler.postDelayed(fpsRunnable, FPS_INTERVAL_MS)
    }
    fpsHandler.removeCallbacks(fatalRunnable)
    if (!fatalShown) {
      fpsHandler.postDelayed(fatalRunnable, FPS_INTERVAL_MS)
    }
    fpsHandler.removeCallbacks(stateRequestRunnable)
    if (BuildConfig.DEBUG) {
      fpsHandler.postDelayed(stateRequestRunnable, STATE_POLL_MS)
    }
    router?.let {
      it.reload()
      fpsHandler.removeCallbacks(rumbleRunnable)
      fpsHandler.postDelayed(rumbleRunnable, RUMBLE_INTERVAL_MS)
    }
  }

  /**
   * The status and navigation bars stay hidden (a swipe shows them for a
   * while). SDL hides them only once its window is fullscreen; started again
   * after a relaunch, the activity got focus before that and the status bar
   * stayed over the top of the game (and the FPS line).
   */
  override fun onWindowFocusChanged(hasFocus: Boolean) {
    super.onWindowFocusChanged(hasFocus)
    if (hasFocus) hideSystemBars()
  }

  /** SDL's window style stays full screen: its windowed start showed the status bar for a moment. */
  override fun keepFullscreen() = true

  private fun hideSystemBars() {
    if (Build.VERSION.SDK_INT >= 30) {
      window.insetsController?.let {
        it.hide(WindowInsets.Type.systemBars())
        it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
      }
    }
  }

  override fun onPause() {
    fpsHandler.removeCallbacks(fpsRunnable)
    fpsHandler.removeCallbacks(fatalRunnable)
    fpsHandler.removeCallbacks(rumbleRunnable)
    router?.stopRumble()
    activityPaused = true
    syncPause() /* the emulation stops (the device suspends, another app in front) */
    super.onPause()
  }

  override fun onDestroy() {
    fpsHandler.removeCallbacks(fpsRunnable)
    fpsHandler.removeCallbacks(fatalRunnable)
    fpsHandler.removeCallbacks(rumbleRunnable)
    loadingDialog?.dismiss()
    (getSystemService(Context.INPUT_SERVICE) as InputManager).unregisterInputDeviceListener(this)
    multicastLock?.release()
    super.onDestroy()
  }

  /**
   * "Building shaders n / N" in the middle of the start screen while the core
   * builds the game's cached shaders, before the game runs. Polled until the
   * build is over (or 30 s without one).
   */
  private fun setupShaderBuildLine() {
    val line = TextView(this).apply {
      visibility = View.GONE
      setTextColor(Color.WHITE)
      setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
      setShadowLayer(2f, 1f, 1f, Color.BLACK)
      setPadding(24, 12, 24, 12)
      setBackgroundColor(Color.argb(140, 0, 0, 0))
    }
    val params = RelativeLayout.LayoutParams(
      RelativeLayout.LayoutParams.WRAP_CONTENT,
      RelativeLayout.LayoutParams.WRAP_CONTENT,
    ).apply { addRule(RelativeLayout.CENTER_IN_PARENT) }
    mLayout?.addView(line, params)
    val started = SystemClock.uptimeMillis()
    var seen = false
    fpsHandler.post(object : Runnable {
      override fun run() {
        val p = nativeGetShaderBuild()
        val total = p?.getOrNull(1) ?: 0
        val loading = loadingDialog         /* a savestate's load: the progress goes in its dialog */
        if (total > 0) {
          seen = true
          val progress = getString(R.string.shader_build_progress, p!![0], total)
          if (loading != null) {
            loading.say(progress)
          } else {
            line.text = progress
            line.visibility = View.VISIBLE
          }
        } else {
          line.visibility = View.GONE
          loading?.say(null)
          if (seen || SystemClock.uptimeMillis() - started > 30_000) {
            return
          }
        }
        fpsHandler.postDelayed(this, 200)
      }
    })
  }

  /**
   * The title's frame rate and pacing, top left ("show_fps"), and under it,
   * in debug builds, the HLE GPU's stutter and hang monitor ("show_hitches"):
   * counts, the ongoing or last hang and the last stutter, with their causes.
   * Each has its own switch; the monitor takes the top with the FPS off.
   */
  private fun setupFpsOverlay() {
    val fps = TextView(this).apply {
      id = View.generateViewId()
      text = "FPS: --"
      setTextColor(Color.WHITE)
      setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f)
      typeface = Typeface.MONOSPACE
      setShadowLayer(2f, 1f, 1f, Color.BLACK)
      setPadding(16, 8, 16, 8)
      setBackgroundColor(Color.argb(100, 0, 0, 0))
      maxLines = 1
    }
    val params = RelativeLayout.LayoutParams(
      RelativeLayout.LayoutParams.WRAP_CONTENT,
      RelativeLayout.LayoutParams.WRAP_CONTENT,
    ).apply {
      addRule(RelativeLayout.ALIGN_PARENT_TOP)
      addRule(RelativeLayout.ALIGN_PARENT_START)
    }
    mLayout?.addView(fps, params)
    fpsView = fps

    if (!BuildConfig.DEBUG) {
      return
    }
    val hitch = TextView(this).apply {
      text = ""
      visibility = View.GONE
      setTextColor(Color.WHITE)
      setTextSize(TypedValue.COMPLEX_UNIT_SP, 10f)
      typeface = Typeface.MONOSPACE
      setShadowLayer(2f, 1f, 1f, Color.BLACK)
      setPadding(16, 6, 16, 6)
      setBackgroundColor(Color.argb(100, 0, 0, 0))
      maxLines = 3
    }
    val hitchParams = RelativeLayout.LayoutParams(
      RelativeLayout.LayoutParams.WRAP_CONTENT,
      RelativeLayout.LayoutParams.WRAP_CONTENT,
    ).apply {
      addRule(RelativeLayout.BELOW, fps.id)
      addRule(RelativeLayout.ALIGN_PARENT_START)
      alignWithParent = true            /* the top when the FPS counter is gone */
      topMargin = 4
    }
    mLayout?.addView(hitch, hitchParams)
    hitchView = hitch
  }

  /** The run's cheats in the manifest's order (nativeSetCheat's index), and the game's key; empty: none. */
  @Volatile private var runCheats: List<CheatInfo> = emptyList()
  @Volatile private var runKey = ""

  /** "mods=<manifest>" when the disc's title has mods or cheats (SDL's thread: may extract a mod's files). */
  private fun modsArg(): String? {
    if (discUri == null) return null
    val key = gameKey()
    if (!ModCatalog.has(this, key)) return null
    return runCatching { ModRun.write(this, key) }
      .onFailure { Log.e(TAG, "mods: ${it.message}") }
      .getOrNull()
      ?.let { (file, cheats) ->
        runKey = key
        runCheats = cheats
        "mods=${file.absolutePath}"
      }
  }

  /** The menu's Cheats: each on or off at once (from the next vblank), and so kept for the game. */
  private fun showCheats() {
    val cheats = runCheats
    val choices = ModChoices(this, runKey)
    val checked = BooleanArray(cheats.size) { choices.cheat(cheats[it].name) }
    showMenuDialog(
      AlertDialog.Builder(this)
        .setTitle(R.string.overlay_menu_cheats)
        .setMultiChoiceItems(cheats.map { it.name }.toTypedArray(), checked) { _, i, on ->
          nativeSetCheat(i, on)
          choices.setCheat(cheats[i].name, on)
        }
        .setPositiveButton(android.R.string.ok, null)
    )
  }

  /** The game's content URI, opened in this process; the core reads it by descriptor. */
  /** The running game's overrides: by its disc's title ID and version, else its path. */
  private fun gameOverrides(): GameOverrides? = discUri?.let { GameOverrides(this, gameKey()) }

  /**
   * The game's key for its per-game settings and its savestates (the library's
   * too): the disc opened and its XBE read once (it was three times before boot).
   */
  private fun gameKey(): String {
    gameKeyMemo?.let { (uri, key) -> if (uri == discUri) return key }
    val (titleId, version) = discUri?.let { uri -> XboxDisc.open(this, uri) { it.titleId to it.version } } ?: (0L to 0L)
    val path = getSharedPreferences(AppPrefs.NAME, Context.MODE_PRIVATE).getString("current_game_relative_path", null)
    val key = GameOverrides.gameKey(titleId, version, path)
    gameKeyMemo = discUri to key
    return key
  }

  private var gameKeyMemo: Pair<Uri?, String>? = null

  private var discUri: Uri? = null

  /** The disc image's name without its extension (the game's log is named by it). */
  private fun gameName(): String =
    discUri?.let { DocumentFile.fromSingleUri(this, it)?.name }
      ?.replace(Regex("(\\.xiso)?\\.iso$", RegexOption.IGNORE_CASE), "") ?: "game"

  private fun openDisc(): String {
    val uri = intent.getStringExtra(EXTRA_DVD_URI)
      ?: getSharedPreferences("x1box_prefs", Context.MODE_PRIVATE).getString("dvdUri", null)
    discUri = uri?.takeIf { it.isNotBlank() }?.let(Uri::parse)
    if (uri.isNullOrBlank()) {
      Log.e(TAG, "no game to run")
      return ""
    }
    return try {
      val pfd = contentResolver.openFileDescriptor(Uri.parse(uri), "r")
      "fd:${pfd!!.detachFd()}"
    } catch (e: Exception) {
      Log.e(TAG, "can't open $uri: ${e.message}")
      ""
    }
  }

  private fun setupOnScreenController() {
    val overlay = OnScreenController(this).apply {
      layoutParams = ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
    }
    overlay.setControllerListener(OverlayBridge())
    overlay.onMenuButtonTapped = { showMenu() }
    overlay.drawGate = overlayPacer::mayDraw
    mLayout?.addView(overlay)
    overlayPacer.attach(overlay)
    onScreenController = overlay
    updateControllerVisibility()
  }

  /**
   * All the controls while no physical gamepad is attached; with one, none,
   * not even the jewel (the swipe up and back open the menu). The menu shows
   * or hides them whatever is attached, for the rest of the session; hidden
   * without a gamepad, the jewel stays.
   */
  private fun updateControllerVisibility() {
    val hasPad = hasGamepad()
    val shown = controlsShown ?: !hasPad
    onScreenController?.minimal = !shown
    onScreenController?.jewelHidden = hasPad && !shown
  }

  private fun hasGamepad() = Controllers.connected().isNotEmpty()

  /** The physical controllers to their players (null: the legacy SDL input). */
  private var router: PadRouter? = null
  private val rumbleRunnable = object : Runnable {
    override fun run() {
      router?.pollRumble()
      fpsHandler.postDelayed(this, RUMBLE_INTERVAL_MS)
    }
  }

  /** The user's choice from the menu; null: by whether a gamepad is attached. */
  private var controlsShown: Boolean? = null
  private var editorBar: View? = null

  /**
   * The jewel, the swipe up and the back key: edit, show or hide the controls,
   * or leave the game. The game stops while it is open; Resume, a tap outside
   * it or back closes it and the game goes on (showMenuDialog).
   */
  private fun showMenu() {
    if (editorBar != null) return
    val shown = controlsShown ?: !hasGamepad()
    /* savestates: push-buffer mode only (native/state) */
    val states = nativeStatesAvailable()
    val items = mutableListOf<Pair<String, () -> Unit>>(
      getString(R.string.overlay_menu_edit) to ::editControls,
      getString(if (shown) R.string.overlay_menu_hide else R.string.overlay_menu_show) to {
        controlsShown = !shown
        updateControllerVisibility()
      },
      getString(R.string.overlay_menu_controllers) to ::showPlayers,
    )
    if (states) {
      items += getString(R.string.overlay_menu_save_state) to { saveStates.showSlots(save = true) }
      items += getString(R.string.overlay_menu_load_state) to { saveStates.showSlots(save = false) }
    }
    if (runCheats.isNotEmpty()) {
      items += getString(R.string.overlay_menu_cheats) to ::showCheats
    }
    val exit = if (launchedExternally) R.string.overlay_menu_exit_frontend else R.string.overlay_menu_exit
    items += getString(exit) to ::exitGame
    showMenuDialog(
      AlertDialog.Builder(this)
        .setItems(items.map { it.first }.toTypedArray()) { _, which -> items[which].second() }
        .setNegativeButton(R.string.overlay_menu_resume, null)
    )
  }

  /** The players the core has a controller plugged in for (bit n: player n + 1). */
  private var plugged = 1

  /**
   * The players' controllers plugged in and pulled out as the connected ones
   * and the controller settings change: player 1 and each player a
   * connected controller plays as (the router's assignment).
   */
  private fun syncPads() {
    val r = router ?: return
    var mask = 1
    for (p in r.assignment.values) mask = mask or (1 shl p)
    if (mask != plugged) {
      plugged = mask
      nativeSetPads(mask)
    }
  }

  /**
   * The menu's Controllers: which controller plays as which player, changed
   * at once (and kept, as the settings' Players), its controller plugged in
   * or pulled out at once as well.
   */
  private fun showPlayers() {
    val r = router ?: return
    val config = Controllers.load(this)
    val devices = Controllers.connected()
    val byId = devices.associateBy { it.id }
    val names = (0 until Controllers.PLAYERS).map { p ->
      val s = config.slots[p]
      val dev = r.assignment.entries.firstOrNull { it.value == p }?.let { byId[it.key]?.name }
      val what = when (s.mode) {
        PlayerSlot.OFF -> getString(R.string.controllers_off)
        PlayerSlot.DEVICE -> dev ?: getString(R.string.controllers_not_connected, s.name)
        else -> dev?.let { getString(R.string.controllers_auto_is, it) } ?: getString(R.string.controllers_auto)
      }
      getString(R.string.controllers_player, p + 1) + ": " + what
    }
    showMenuDialog(
      AlertDialog.Builder(this)
        .setTitle(R.string.overlay_menu_controllers)
        .setItems(names.toTypedArray()) { _, p -> pickController(p, config, devices) }
        .setNegativeButton(android.R.string.cancel, null)
    )
  }

  private fun pickController(player: Int, config: Controllers.Config, devices: List<InputDevice>) {
    val options = mutableListOf(PlayerSlot() to getString(R.string.controllers_auto))
    for (d in devices) options += PlayerSlot(PlayerSlot.DEVICE, Controllers.unitKey(d), d.name ?: "") to (d.name ?: "?")
    if (player > 0) options += PlayerSlot(PlayerSlot.OFF) to getString(R.string.controllers_off)
    showMenuDialog(AlertDialog.Builder(this)
      .setTitle(getString(R.string.controllers_player, player + 1))
      .setItems(options.map { it.second }.toTypedArray()) { _, i ->
        val slot = options[i].first
        /* a controller chosen for this player leaves any other it played as */
        val slots = config.slots.mapIndexed { p, s ->
          when {
            p == player -> slot
            slot.mode == PlayerSlot.DEVICE && s.mode == PlayerSlot.DEVICE && s.key == slot.key -> PlayerSlot()
            else -> s
          }
        }
        val next = config.copy(slots = slots)
        Controllers.save(this, next)
        router?.reload()
        syncPads()
      }
      .setNegativeButton(android.R.string.cancel, null))
  }

  /** The layout editor over the running game (input to the game pauses meanwhile). */
  private fun editControls() {
    val overlay = onScreenController ?: return
    overlay.minimal = false
    overlay.editing = true
    val bar = OverlayEditorBar(this, overlay) { closeEditor() }
    val params = RelativeLayout.LayoutParams(
      RelativeLayout.LayoutParams.WRAP_CONTENT,
      RelativeLayout.LayoutParams.WRAP_CONTENT,
    ).apply {
      addRule(RelativeLayout.ALIGN_PARENT_TOP)
      addRule(RelativeLayout.CENTER_HORIZONTAL)
    }
    mLayout?.addView(bar, params)
    editorBar = bar
  }

  /** A frontend (ES-DE...) launched the game: there's no library to go back to. */
  private val launchedExternally
    get() = intent.getBooleanExtra(LauncherActivity.EXTRA_LAUNCHED_EXTERNALLY, false)

  /**
   * The game closes: back to the frontend that launched it (Android shows
   * what was under the game, its own task or this one), or to the library.
   * The core keeps its state in this process for one run, so the process
   * goes with the game.
   */
  private fun exitGame() {
    if (!launchedExternally) {
      startActivity(
        Intent(this, LauncherActivity::class.java)
          .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
      )
    }
    finish()
    android.os.Process.killProcess(android.os.Process.myPid())
  }

  /**
   * The same disc again in a new core process, booting the XBE the title
   * launched (the core saved its launch data page). RelaunchActivity, in the
   * main process, starts it once this process is gone, showing the frame on
   * screen now (copied from the surface) until the new core shows its own.
   */
  private var relaunching = false

  private fun relaunchCore() {
    if (relaunching) return
    relaunching = true
    Log.i(TAG, "relaunch: the title launched another XBE; starting the core again")
    val surface = mSurface
    if (surface != null && surface.width > 0 && surface.height > 0) {
      val frame = Bitmap.createBitmap(surface.width, surface.height, Bitmap.Config.ARGB_8888)
      val timeout = Runnable { startRelaunch() }
      try {
        PixelCopy.request(surface, frame, { result ->
          fpsHandler.removeCallbacks(timeout)
          if (result != PixelCopy.SUCCESS) {
            startRelaunch()
          } else {
            Thread {
              val file = RelaunchActivity.heldFrame(this)
              runCatching { file.outputStream().use { frame.compress(Bitmap.CompressFormat.JPEG, 92, it) } }
              runOnUiThread { startRelaunch() }
            }.start()
          }
        }, fpsHandler)
        fpsHandler.postDelayed(timeout, RELAUNCH_COPY_MS)
        return
      } catch (e: IllegalArgumentException) {
        Log.w(TAG, "relaunch: the frame can't be copied: ${e.message}")
      }
    }
    startRelaunch()
  }

  private var relaunchStarted = false

  private fun startRelaunch() {
    if (relaunchStarted) return
    relaunchStarted = true
    val uri = discUri?.toString() ?: intent.getStringExtra(EXTRA_DVD_URI)
    startActivity(
      Intent(this, RelaunchActivity::class.java)
        .putExtra(EXTRA_DVD_URI, uri)
        .putExtra(
          LauncherActivity.EXTRA_LAUNCHED_EXTERNALLY,
          intent.getBooleanExtra(LauncherActivity.EXTRA_LAUNCHED_EXTERNALLY, false),
        )
        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK or Intent.FLAG_ACTIVITY_NO_ANIMATION),
      RelaunchActivity.noTransition(this).toBundle(),
    )
    finish()
    RelaunchActivity.noCloseTransition(this)
    android.os.Process.killProcess(android.os.Process.myPid())
  }

  /**
   * After a relaunch: the frame the game left on screen, and over it for a
   * savestate's load its loading dialog, until this core shows a frame of its
   * own. Before that the core starts, builds its cached shaders and loads the
   * state, the game at 0 fps.
   */
  private var heldView: ImageView? = null
  private var loadingDialog: LoadingDialog? = null
  /** The Wi-Fi multicast lock, while the game runs on the LAN (data/Network.kt). */
  private var multicastLock: WifiManager.MulticastLock? = null
  private var heldSince = 0L
  private val heldRunnable = object : Runnable {
    override fun run() {
      if (heldView == null && loadingDialog == null) {
        return
      }
      if (nativeGetFrameShown() || SystemClock.uptimeMillis() - heldSince > HELD_MAX_MS) {
        heldView?.let { mLayout?.removeView(it) }
        loadingDialog?.dismiss()
        heldView = null
        loadingDialog = null
        RelaunchActivity.heldFrame(this@NativeCoreActivity).delete()
        return
      }
      fpsHandler.postDelayed(this, HELD_POLL_MS)
    }
  }

  /** state: the savestate being loaded, or null (the title's own reboot). */
  private fun showHeldFrame(state: String?) {
    val layout = mLayout ?: return
    BitmapFactory.decodeFile(RelaunchActivity.heldFrame(this).path)?.let { frame ->
      val v = ImageView(this).apply {
        scaleType = ImageView.ScaleType.FIT_XY
        setImageBitmap(frame)
        layoutParams = ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
      }
      layout.addView(v, minOf(1, layout.childCount))   /* over the game's surface, under the controls */
      heldView = v
    }
    if (state != null) {
      loadingDialog = LoadingDialog(this, state).also { it.show() }
    }
    if (heldView != null || loadingDialog != null) {
      heldSince = SystemClock.uptimeMillis()
      fpsHandler.postDelayed(heldRunnable, HELD_POLL_MS)
    }
  }

  private fun closeEditor() {
    onScreenController?.editing = false
    editorBar?.let { mLayout?.removeView(it) }
    editorBar = null
    updateControllerVisibility()
  }

  /* Back opens the menu instead of leaving the game; while editing, it cancels the edit. */
  private fun onBack() {
    if (editorBar != null) {
      onScreenController?.reloadLayout()
      closeEditor()
    } else {
      showMenu()
    }
  }

  @Deprecated("Android 12 and older; newer ones call the OnBackInvokedCallback.")
  override fun onBackPressed() = onBack()

  /*
   * The system's back (the navigation button, the back swipe, a keyboard's
   * key, a handheld's Back key: the RP5's is its built-in controller's) arrives
   * as KEYCODE_BACK before any callback: SDL took it, and its native side's
   * manualBackButton called Activity.onBackPressed past ours, closing the game.
   * It opens the menu here first. A controller's back key its mapping binds
   * stays the game's (the Xbox Back button of a controller whose select sends
   * it: Controllers.defaults).
   */
  override fun dispatchKeyEvent(event: KeyEvent): Boolean {
    if (event.keyCode == KeyEvent.KEYCODE_BACK && router?.bindsKey(event) != true) {
      if (event.action == KeyEvent.ACTION_UP && !event.isCanceled) {
        onBack()
      }
      return true
    }
    /* A controller's keys go to its player through its mapping, before SDL
     * (which reads none with the router); its guide button, unbound, opens
     * the menu. */
    if (editorBar == null && router?.onKey(event) == true) {
      return true
    }
    if (event.keyCode == KeyEvent.KEYCODE_BUTTON_MODE && Controllers.isGameController(event.device)) {
      if (event.action == KeyEvent.ACTION_UP && !event.isCanceled) {
        showMenu()
      }
      return true
    }
    return super.dispatchKeyEvent(event)
  }

  /* A controller's sticks, triggers and hat, likewise. */
  override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
    if (editorBar == null && router?.onMotion(event) == true) {
      return true
    }
    return super.dispatchGenericMotionEvent(event)
  }

  override fun onInputDeviceAdded(deviceId: Int) = onControllersChanged()
  override fun onInputDeviceRemoved(deviceId: Int) = onControllersChanged()
  override fun onInputDeviceChanged(deviceId: Int) = onControllersChanged()

  private fun onControllersChanged() {
    router?.reassign()
    syncPads()
    updateControllerVisibility()
  }

  /** The on-screen controller's whole state, sent to the core on every change. */
  private class OverlayBridge : OnScreenController.ControllerListener {
    private var buttons = 0
    private var analog = 0
    private var lt = 0f
    private var rt = 0f
    private var lx = 0f
    private var ly = 0f
    private var rx = 0f
    private var ry = 0f

    override fun onButtonPressed(button: OnScreenController.Button) = set(button, true)
    override fun onButtonReleased(button: OnScreenController.Button) = set(button, false)

    override fun onStickMoved(stick: OnScreenController.Stick, x: Float, y: Float) {
      if (stick == OnScreenController.Stick.LEFT) { lx = x; ly = y } else { rx = x; ry = y }
      send()
    }

    override fun onStickPressed(stick: OnScreenController.Stick) =
      bit(if (stick == OnScreenController.Stick.LEFT) PAD_LTHUMB else PAD_RTHUMB, true)

    override fun onStickReleased(stick: OnScreenController.Stick) =
      bit(if (stick == OnScreenController.Stick.LEFT) PAD_LTHUMB else PAD_RTHUMB, false)

    private fun set(button: OnScreenController.Button, down: Boolean) {
      when (button) {
        OnScreenController.Button.A -> analogBit(0, down)
        OnScreenController.Button.B -> analogBit(1, down)
        OnScreenController.Button.X -> analogBit(2, down)
        OnScreenController.Button.Y -> analogBit(3, down)
        OnScreenController.Button.BLACK -> analogBit(4, down)
        OnScreenController.Button.WHITE -> analogBit(5, down)
        OnScreenController.Button.LEFT_TRIGGER -> { lt = if (down) 1f else 0f; send() }
        OnScreenController.Button.RIGHT_TRIGGER -> { rt = if (down) 1f else 0f; send() }
        OnScreenController.Button.DPAD_UP -> bit(PAD_UP, down)
        OnScreenController.Button.DPAD_DOWN -> bit(PAD_DOWN, down)
        OnScreenController.Button.DPAD_LEFT -> bit(PAD_LEFT, down)
        OnScreenController.Button.DPAD_RIGHT -> bit(PAD_RIGHT, down)
        OnScreenController.Button.START -> bit(PAD_START, down)
        OnScreenController.Button.BACK -> bit(PAD_BACK, down)
        OnScreenController.Button.LEFT_STICK_BUTTON -> bit(PAD_LTHUMB, down)
        OnScreenController.Button.RIGHT_STICK_BUTTON -> bit(PAD_RTHUMB, down)
      }
    }

    private fun analogBit(i: Int, down: Boolean) {
      analog = if (down) analog or (1 shl i) else analog and (1 shl i).inv()
      send()
    }

    private fun bit(mask: Int, down: Boolean) {
      buttons = if (down) buttons or mask else buttons and mask.inv()
      send()
    }

    private fun send() = nativeSetOverlayPad(buttons, analog, lx, ly, rx, ry, lt, rt)
  }

  companion object {
    private const val TAG = "hakuX-native"
    const val EXTRA_DVD_URI = "dvdUri"
    /** Boot the XBE of the saved launch data page (a title's XLaunchNewImage). */
    const val EXTRA_RELAUNCH = "relaunch"
    /** Boot into the savestate at this path (SaveStates' Load state). */
    const val EXTRA_LOADSTATE = "loadstate"
    /** RelaunchActivity: the game started again from the start (no launch data, no savestate). */
    const val EXTRA_RESTART = "restart"

    /* native/dev/usb.h NC_PAD_* */
    private const val PAD_UP = 0x01
    private const val PAD_DOWN = 0x02
    private const val PAD_LEFT = 0x04
    private const val PAD_RIGHT = 0x08
    private const val PAD_START = 0x10
    private const val PAD_BACK = 0x20
    private const val PAD_LTHUMB = 0x40
    private const val PAD_RTHUMB = 0x80

    private const val FPS_INTERVAL_MS = 500L
    /** The FPS and stutter lines' update held back (OverlayPacer): tried again this soon. */
    private const val LINES_RETRY_MS = 50L
    private const val STATE_POLL_MS = 500L
    private const val RUMBLE_INTERVAL_MS = 50L
    /** The relaunch's frame copy: how long it may take before the restart goes on without it. */
    private const val RELAUNCH_COPY_MS = 1000L
    /** The held frame after a relaunch: checked this often, kept at most this long. */
    private const val HELD_POLL_MS = 30L
    private const val HELD_MAX_MS = 20000L
    const val PREF_UNCAP = "uncap_fps"
    const val PREF_FRAME_PACE = "frame_pace"
    const val PREF_ASYNC_SHADERS = "async_shaders"
    const val PREF_PREBUILD_SHADERS = "prebuild_shaders"
    const val PREF_SHOW_HITCHES = "show_hitches"
    const val PREF_SCALE = "render_scale"
    const val DEFAULT_SCALE = 1
    const val PREF_ASPECT = "aspect_ratio"
    const val ASPECT_AUTO = "auto"
    const val PREF_FXAA = "fxaa"
    const val PREF_UPSCALE = "upscale_filter"
    const val UPSCALE_BILINEAR = "bilinear"
    const val UPSCALE_SGSR = "sgsr"

    /** fps, average frame time (ms), last frame time (ms), 1 if push-buffer decoding renders. */
    @JvmStatic
    external fun nativeGetFrameStats(): FloatArray?

    /** Why the game can't run on this device (the GPU driver, or the game), or null. */
    @JvmStatic
    external fun nativeGetFatal(): String?

    /** Whose fault that is: FATAL_DRIVER (1), the game (2), a savestate (3),
     * the driver losing the GPU while the game ran (4), or 0. */
    @JvmStatic
    external fun nativeGetFatalKind(): Int

    /** nativeGetFatalKind (native/hle/hle_native.h NC_FATAL_*). */
    private const val FATAL_DRIVER = 1
    private const val FATAL_STATE = 3
    private const val FATAL_GPU_LOST = 4
    private const val FATAL_BOOT = 5

    /** The core stopped to boot another of the title's XBEs. */
    @JvmStatic
    external fun nativeGetRelaunch(): Boolean

    /** The core stopped as the title left (to the dashboard, a reboot, a shutdown). */
    @JvmStatic
    external fun nativeGetQuit(): Boolean

    /** The players with a controller plugged in (bit n: player n + 1); the core plugs and pulls them. */
    @JvmStatic
    external fun nativeSetPads(mask: Int)

    /** A console on the network with this one's identity: its address, once each; null when no new one. */
    @JvmStatic
    external fun nativeTakeTwinConsole(): String?

    /** A title frame has been on screen. */
    @JvmStatic
    external fun nativeGetFrameShown(): Boolean

    /** The HLE GPU's stutter and hang monitor lines ("": nothing to show). */
    @JvmStatic
    external fun nativeGetHleHitches(): String

    /** How long the GPU has been on the game's work without running out of it, in ms (0: out now). */
    @JvmStatic
    external fun nativeGpuBusyMs(): Int

    /** A moment of idle GPU for a window frame waiting behind the game's work (OverlayPacer). */
    @JvmStatic
    external fun nativeGpuGap()

    /** The cached shaders' build before the game: {built, to build}, {0, 0} when none runs. */
    @JvmStatic
    external fun nativeGetShaderBuild(): IntArray?

    @JvmStatic
    external fun nativeSetOverlayPad(
      buttons: Int, analogMask: Int, lx: Float, ly: Float, rx: Float, ry: Float, lt: Float, rt: Float,
    )

    /** A player's Duke from its physical controller (PadRouter): NC_PAD_* buttons, analog bytes, sticks. */
    @JvmStatic
    external fun nativeSetPad(player: Int, buttons: Int, analog: Long, lx: Int, ly: Int, rx: Int, ry: Int)

    /** The rumble the title last sent to a player: left << 16 | right. */
    @JvmStatic
    external fun nativeGetRumble(player: Int): Int

    /** The emulation paused (onPause) or resumed (onResume). */
    @JvmStatic
    external fun nativeSetPaused(paused: Boolean)

    /** Saves the running game to path (not on the UI thread): null, or why it couldn't. */
    @JvmStatic
    external fun nativeSaveState(path: String): String?

    /** Whether the savestate at path can load into this game: null, or why not. */
    @JvmStatic
    external fun nativeStateCheck(path: String): String?

    /** debug: a new debug.hakux.state request ("save:<slot>", "load:<slot>"), or null. */
    @JvmStatic
    external fun nativeTakeStateRequest(): String?

    /** Savestates work for the running game (push-buffer mode). */
    @JvmStatic
    external fun nativeStatesAvailable(): Boolean

    /** A cheat of the run's manifest (its index there) on or off. */
    @JvmStatic
    external fun nativeSetCheat(index: Int, on: Boolean)
  }
}
