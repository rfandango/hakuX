package com.rfandango.haku_x

import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.hardware.input.InputManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.TypedValue
import android.view.GestureDetector
import android.view.InputDevice
import android.view.MotionEvent
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.widget.FrameLayout
import android.widget.RelativeLayout
import android.widget.TextView
import android.view.KeyEvent
import org.libsdl.app.SDLActivity

class MainActivity : SDLActivity(), InputManager.InputDeviceListener {

  companion object {
    /**
     * Set when an external frontend, rather than the in-app library, chose the
     * game.  Exiting then returns to that frontend instead of the library.
     */
    const val EXTRA_FROM_FRONTEND = "from_frontend"
  }

  private var onScreenController: OnScreenController? = null
  private var controllerBridge: ControllerInputBridge? = null
  private var isControllerVisible = false
  private var inputManager: InputManager? = null
  private var hasPhysicalController = false
  private var pauseMenuOverlay: PauseMenuOverlay? = null
  private var suspendedByLifecycle = false

  private val prefs by lazy { getSharedPreferences("x1box_prefs", MODE_PRIVATE) }
  private var fpsTextView: TextView? = null
  private val fpsHandler = Handler(Looper.getMainLooper())
  private val fpsUpdateInterval = 1000L
  private val fpsRunnable = object : Runnable {
    override fun run() {
      fpsTextView?.text = "FPS: ${nativeGetFps()}"
      fpsHandler.postDelayed(this, fpsUpdateInterval)
    }
  }

  private external fun nativeGetFps(): Int
  private external fun nativePauseEmulation(): Unit
  private external fun nativeResumeEmulation(): Unit
  private external fun nativeExitEmulation(): Unit
  private external fun nativeDumpDiagFrames(numFrames: Int): Unit

  override fun loadLibraries() {
    super.loadLibraries()
    initializeGpuDriver()
  }

  private fun initializeGpuDriver() {
    GpuDriverHelper.init(this)
    if (!GpuDriverHelper.supportsCustomDriverLoading()) {
      android.util.Log.i("MainActivity", "GPU driver: custom loading not supported on this device")
      return
    }

    // Check per-game override: "system" forces system driver, "custom" or null uses installed
    val driverOverride = getSharedPreferences("x1box_prefs", MODE_PRIVATE)
      .getString(PerGameSettingsManager.runtimeKey("gpu_driver"), null)

    if (driverOverride == "system") {
      android.util.Log.i("MainActivity", "GPU driver: per-game override forces system driver")
      return
    }

    val driverLib = GpuDriverHelper.getInstalledDriverLibrary()
    if (driverLib != null) {
      android.util.Log.i("MainActivity", "GPU driver: loading custom driver=$driverLib")
      GpuDriverHelper.initializeDriver(driverLib)
    } else {
      android.util.Log.i("MainActivity", "GPU driver: no custom driver installed, using system default")
    }
  }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    val prefs = getSharedPreferences("x1box_prefs", MODE_PRIVATE)
    // Per-game overrides take precedence over global settings
    val rendererPref = prefs.getString(PerGameSettingsManager.runtimeKey("renderer"), null)
      ?: prefs.getString("renderer", "vulkan") ?: "vulkan"
    SDLActivity.nativeSetenv("XEMU_RENDERER", rendererPref)
    SDLActivity.nativeSetenv("SDL_ANDROID_TRAP_BACK_BUTTON", "1")

    // Texture settings: per-game override takes precedence over global
    val texDumpEnabled = prefs.getString(PerGameSettingsManager.runtimeKey("texture_dump_enabled"), null)
      ?.let { it == "true" }
      ?: prefs.getBoolean("texture_dump_enabled", false)
    val texReplaceEnabled = false // feature hidden for now

    // Texture dump: pass settings via env vars for native init
    if (texDumpEnabled) {
      SDLActivity.nativeSetenv("XEMU_TEXTURE_DUMP", "1")
      val dumpPath = resolveTextureDumpRealPath(prefs)
      if (dumpPath != null) {
        SDLActivity.nativeSetenv("XEMU_TEXTURE_DUMP_PATH", dumpPath)
      }
    }

    // Texture replacement: use cache dir prepared by GameLibraryActivity
    if (texReplaceEnabled) {
      flushShaderCaches()
      val cacheDir = prefs.getString("textureReplaceCachePath", null)
      if (cacheDir != null && java.io.File(cacheDir).isDirectory) {
        SDLActivity.nativeSetenv("XEMU_TEXTURE_REPLACE", "1")
        SDLActivity.nativeSetenv("XEMU_TEXTURE_REPLACE_PATH", cacheDir)
      }
    }
    setupOnScreenController()
    setupFpsOverlay()
    setupPauseMenu()
    setupEdgeSwipe()
    setupControllerDetection()
    hideSystemUI()
  }

  override fun onWindowFocusChanged(hasFocus: Boolean) {
    super.onWindowFocusChanged(hasFocus)
    if (hasFocus) {
      hideSystemUI()
    }
  }

  private fun resolveTextureDumpRealPath(prefs: android.content.SharedPreferences): String? {
    val uriStr = prefs.getString("textureDumpFolderUri", null) ?: return null
    val uri = android.net.Uri.parse(uriStr)
    val docId = android.provider.DocumentsContract.getTreeDocumentId(uri)
    if (docId.startsWith("primary:")) {
      val relPath = docId.removePrefix("primary:")
      val realPath = android.os.Environment.getExternalStorageDirectory().absolutePath + "/" + relPath
      val dir = java.io.File(realPath)
      if (dir.isDirectory || dir.mkdirs()) return realPath
    }
    val fallback = java.io.File(getExternalFilesDir(null), "texture_dump")
    fallback.mkdirs()
    return fallback.absolutePath
  }

  // Texture decode progress dialog - called from native loader thread via JNI
  private var decodeDialog: android.app.AlertDialog? = null
  private var decodeProgressBar: android.widget.ProgressBar? = null
  private var decodeMessageView: android.widget.TextView? = null
  private val uiHandler = Handler(Looper.getMainLooper())

  @Suppress("unused") // Called from native code via JNI
  fun onTextureDecodeProgress(current: Int, total: Int) {
    uiHandler.post {
      if (current == 0 && total > 0) {
        // First call: show dialog
        val pad = (24 * resources.displayMetrics.density).toInt()
        val layout = android.widget.LinearLayout(this).apply {
          orientation = android.widget.LinearLayout.VERTICAL
          setPadding(pad, pad, pad, pad)
        }
        val msg = android.widget.TextView(this).apply {
          text = "Decoding texture 0 / $total..."
          setTextColor(android.graphics.Color.WHITE)
        }
        val bar = android.widget.ProgressBar(this, null,
          android.R.attr.progressBarStyleHorizontal).apply {
          max = total
          progress = 0
          val lp = android.widget.LinearLayout.LayoutParams(
            android.widget.LinearLayout.LayoutParams.MATCH_PARENT,
            android.widget.LinearLayout.LayoutParams.WRAP_CONTENT)
          lp.topMargin = pad / 2
          layoutParams = lp
        }
        layout.addView(msg)
        layout.addView(bar)
        decodeProgressBar = bar
        decodeMessageView = msg

        decodeDialog = android.app.AlertDialog.Builder(this)
          .setTitle("Processing Textures")
          .setView(layout)
          .setCancelable(false)
          .create()
        decodeDialog?.show()
      } else if (current >= total) {
        // Done: dismiss
        decodeDialog?.dismiss()
        decodeDialog = null
        decodeProgressBar = null
        decodeMessageView = null
      } else {
        // Update progress
        decodeProgressBar?.progress = current
        decodeMessageView?.text = "Decoding texture $current / $total..."
      }
    }
  }

  // Shader warmup progress dialog - called from native renderer init via JNI
  private var warmupDialog: android.app.AlertDialog? = null
  private var warmupProgressBar: android.widget.ProgressBar? = null
  private var warmupMessageView: android.widget.TextView? = null

  @Suppress("unused") // Called from native code via JNI
  fun onShaderWarmupProgress(current: Int, total: Int) {
    uiHandler.post {
      if (current == 0 && total > 0) {
        val pad = (24 * resources.displayMetrics.density).toInt()
        val layout = android.widget.LinearLayout(this).apply {
          orientation = android.widget.LinearLayout.VERTICAL
          setPadding(pad, pad, pad, pad)
        }
        val msg = android.widget.TextView(this).apply {
          text = "Compiling shader 0 / $total..."
          setTextColor(android.graphics.Color.WHITE)
        }
        val bar = android.widget.ProgressBar(this, null,
          android.R.attr.progressBarStyleHorizontal).apply {
          max = total
          progress = 0
          val lp = android.widget.LinearLayout.LayoutParams(
            android.widget.LinearLayout.LayoutParams.MATCH_PARENT,
            android.widget.LinearLayout.LayoutParams.WRAP_CONTENT)
          lp.topMargin = pad / 2
          layoutParams = lp
        }
        layout.addView(msg)
        layout.addView(bar)
        warmupProgressBar = bar
        warmupMessageView = msg

        warmupDialog = android.app.AlertDialog.Builder(this)
          .setTitle("Loading Shaders")
          .setView(layout)
          .setCancelable(false)
          .create()
        warmupDialog?.show()
      } else if (current >= total) {
        warmupDialog?.dismiss()
        warmupDialog = null
        warmupProgressBar = null
        warmupMessageView = null
      } else {
        warmupProgressBar?.progress = current
        warmupMessageView?.text = "Compiling shader $current / $total..."
      }
    }
  }

  private fun flushShaderCaches() {
    val baseDir = filesDir
    val spvDir = java.io.File(baseDir, "spv_cache")
    if (spvDir.isDirectory) {
      spvDir.listFiles()?.forEach { it.delete() }
      spvDir.delete()
    }
    java.io.File(baseDir, "vk_pipeline_cache.bin").delete()
    java.io.File(baseDir, "shader_module_keys.bin").delete()
    android.util.Log.i("hakuX-texreplace", "Flushed shader caches for texture replacement")
  }

  private fun hideSystemUI() {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
      // Android 11 (API 30) and above
      window.setDecorFitsSystemWindows(false)
      window.insetsController?.let { controller ->
        controller.hide(WindowInsets.Type.systemBars())
        controller.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
      }
    } else {
      // Android 10 and below
      @Suppress("DEPRECATION")
      window.decorView.systemUiVisibility = (
        View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
        or View.SYSTEM_UI_FLAG_FULLSCREEN
        or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
        or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
        or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
      )
    }
  }

  private fun setupFpsOverlay() {
    fpsTextView = TextView(this).apply {
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
      RelativeLayout.LayoutParams.WRAP_CONTENT
    ).apply {
      addRule(RelativeLayout.ALIGN_PARENT_TOP)
      addRule(RelativeLayout.ALIGN_PARENT_START)
    }
    mLayout?.addView(fpsTextView, params)
  }

  private fun setupPauseMenu() {
    pauseMenuOverlay = PauseMenuOverlay(this).apply {
      onExitEmulation = {
        if (intent?.getBooleanExtra(EXTRA_FROM_FRONTEND, false) == true) {
          // An external frontend started this game, so it, not the library, is
          // where the player expects to end up.  Dropping the task rather than
          // just finishing leaves nothing of ours behind to return to, so the
          // frontend comes back to the foreground.
          finishAndRemoveTask()
        } else {
          // Launch the library activity FIRST so the intent is queued
          // before we do any blocking work or kill the process.
          val intent = Intent(this@MainActivity, GameLibraryActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK
          }
          startActivity(intent)
          finish()
        }
        // nativeExitEmulation blocks until the display loop exits and
        // RCU callbacks are drained, so QEMU threads are quiescent
        // before we kill the process.
        Thread {
          nativeExitEmulation()
          android.os.Process.killProcess(android.os.Process.myPid())
        }.start()
      }
      onDismiss = {
        togglePauseMenu()
      }
      onDiagCapture = { numFrames ->
        nativeDumpDiagFrames(numFrames)
      }
      isDebugToolsEnabled = {
        prefs.getBoolean("debug_tools", false)
      }
    }
    val overlayParams = RelativeLayout.LayoutParams(
      RelativeLayout.LayoutParams.MATCH_PARENT,
      RelativeLayout.LayoutParams.MATCH_PARENT
    )
    mLayout?.addView(pauseMenuOverlay, overlayParams)
  }

  private fun setupEdgeSwipe() {
    val edgeWidth = TypedValue.applyDimension(
      TypedValue.COMPLEX_UNIT_DIP, 24f, resources.displayMetrics
    ).toInt()

    val edgeView = View(this)
    val edgeParams = RelativeLayout.LayoutParams(edgeWidth, RelativeLayout.LayoutParams.MATCH_PARENT).apply {
      addRule(RelativeLayout.ALIGN_PARENT_START)
    }

    val gestureDetector = GestureDetector(this, object : GestureDetector.SimpleOnGestureListener() {
      override fun onDown(e: MotionEvent): Boolean = true

      override fun onFling(e1: MotionEvent?, e2: MotionEvent, velocityX: Float, velocityY: Float): Boolean {
        if (e1 != null && velocityX > 500f && (e2.x - e1.x) > 80f) {
          togglePauseMenu()
          return true
        }
        return false
      }
    })

    edgeView.setOnTouchListener { _, event -> gestureDetector.onTouchEvent(event) }
    mLayout?.addView(edgeView, edgeParams)
  }

  override fun dispatchKeyEvent(event: KeyEvent?): Boolean {
    if (event?.keyCode == KeyEvent.KEYCODE_BACK && event.action == KeyEvent.ACTION_DOWN) {
      togglePauseMenu()
      return true
    }
    if (event?.keyCode == KeyEvent.KEYCODE_BACK) {
      return true
    }
    return super.dispatchKeyEvent(event)
  }

  private fun togglePauseMenu() {
    val overlay = pauseMenuOverlay ?: return
    if (overlay.isShowing()) {
      nativeResumeEmulation()
      overlay.dismiss()
    } else {
      nativePauseEmulation()
      overlay.show()
    }
  }

  private fun setupOnScreenController() {
    // Create on-screen controller
    onScreenController = OnScreenController(this).apply {
      layoutParams = FrameLayout.LayoutParams(
        FrameLayout.LayoutParams.MATCH_PARENT,
        FrameLayout.LayoutParams.MATCH_PARENT
      )
    }

    // Create input bridge
    controllerBridge = ControllerInputBridge()
    onScreenController?.setControllerListener(controllerBridge!!)
    onScreenController?.onMenuButtonTapped = { togglePauseMenu() }

    // Add to layout
    mLayout?.addView(onScreenController)

    // Check for existing controllers and show/hide accordingly
    updateControllerVisibility()
  }

  override fun onResume() {
    super.onResume()
    if (suspendedByLifecycle) {
      nativeResumeEmulation()
      suspendedByLifecycle = false
    }

    mLayout?.postDelayed({
      registerVirtualController()
    }, 1000)

    val showFps = prefs.getBoolean("show_fps", true)
    fpsTextView?.visibility = if (showFps) View.VISIBLE else View.GONE
    if (showFps) {
      fpsHandler.postDelayed(fpsRunnable, fpsUpdateInterval)
    } else {
      fpsHandler.removeCallbacks(fpsRunnable)
    }
  }

  override fun onPause() {
    fpsHandler.removeCallbacks(fpsRunnable)
    suspendedByLifecycle = true
    nativePauseEmulation()
    super.onPause()
  }

  private fun registerVirtualController() {
    try {
      // Register the virtual on-screen controller as a joystick device
      // Device ID: -2, Name: "On-Screen Controller"
      org.libsdl.app.SDLControllerManager.nativeAddJoystick(
        -2, // device_id
        "On-Screen Controller", // name
        "Virtual touchscreen controller", // desc
        0x045e, // vendor_id (Microsoft)
        0x028e, // product_id (Xbox 360 Controller)
        false, // is_accelerometer
        0xFFFF, // button_mask (all buttons)
        6, // naxes (left X/Y, right X/Y, left trigger, right trigger)
        0x3F, // axis_mask (6 axes)
        0, // nhats
        0  // nballs
      )
      android.util.Log.d("MainActivity", "Virtual controller registered successfully")
    } catch (e: Exception) {
      android.util.Log.e("MainActivity", "Failed to register virtual controller: ${e.message}")
    }
  }

  private fun setupControllerDetection() {
    inputManager = getSystemService(Context.INPUT_SERVICE) as InputManager
    inputManager?.registerInputDeviceListener(this, null)
    
    // Check for already connected controllers
    checkForPhysicalControllers()
  }

  private fun checkForPhysicalControllers() {
    val deviceIds = inputManager?.inputDeviceIds ?: return
    hasPhysicalController = deviceIds.any { deviceId ->
      val device = inputManager?.getInputDevice(deviceId)
      isGameController(device)
    }
    updateControllerVisibility()
  }

  private fun isGameController(device: InputDevice?): Boolean {
    if (device == null) return false

    // Exclude virtual and built-in devices (sensors, accelerometers, etc.)
    if (device.isVirtual) return false
    val name = device.name?.lowercase() ?: ""
    if (name.contains("accelerometer") || name.contains("gyroscope") ||
        name.contains("sensor") || name.contains("gpio")) {
      return false
    }

    val sources = device.sources
    // Must have gamepad or joystick source AND have real axes/buttons
    val isGamepad = (sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
    val isJoystick = (sources and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
    if (!isGamepad && !isJoystick) return false

    // Require at least one motion axis to filter out keyboard-only "gamepads"
    val motionRanges = device.motionRanges
    return motionRanges != null && motionRanges.isNotEmpty()
  }

  private fun updateControllerVisibility() {
    // Show on-screen controller only if no physical controller is connected
    val shouldShow = !hasPhysicalController
    
    if (shouldShow != isControllerVisible) {
      isControllerVisible = shouldShow
      onScreenController?.visibility = if (shouldShow) View.VISIBLE else View.GONE
    }
  }

  // InputDeviceListener callbacks
  override fun onInputDeviceAdded(deviceId: Int) {
    val device = inputManager?.getInputDevice(deviceId)
    if (isGameController(device)) {
      hasPhysicalController = true
      updateControllerVisibility()
    }
  }

  override fun onInputDeviceRemoved(deviceId: Int) {
    // Recheck all devices to see if any controllers remain
    checkForPhysicalControllers()
  }

  override fun onInputDeviceChanged(deviceId: Int) {
    // Recheck all devices in case configuration changed
    checkForPhysicalControllers()
  }

  override fun onDestroy() {
    fpsHandler.removeCallbacks(fpsRunnable)

    // Unregister virtual controller
    try {
      org.libsdl.app.SDLControllerManager.nativeRemoveJoystick(-2)
    } catch (e: Exception) {
      android.util.Log.e("MainActivity", "Failed to unregister virtual controller: ${e.message}")
    }
    
    inputManager?.unregisterInputDeviceListener(this)
    super.onDestroy()
  }

  // Manual control methods (for settings/preferences)
  fun toggleOnScreenController() {
    isControllerVisible = !isControllerVisible
    onScreenController?.visibility = if (isControllerVisible) View.VISIBLE else View.GONE
  }

  fun showOnScreenController() {
    isControllerVisible = true
    onScreenController?.visibility = View.VISIBLE
  }

  fun hideOnScreenController() {
    isControllerVisible = false
    onScreenController?.visibility = View.GONE
  }

  fun forceUpdateControllerVisibility() {
    checkForPhysicalControllers()
  }

  override fun getLibraries(): Array<String> = arrayOf(
    "SDL2",
    "xemu",
  )
}
