package com.rfandango.xemuandroid

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
import android.widget.Button
import android.widget.RelativeLayout
import android.widget.TextView
import android.widget.Toast
import android.view.KeyEvent
import org.libsdl.app.SDLActivity

class MainActivity : SDLActivity(), InputManager.InputDeviceListener {
  private var onScreenController: OnScreenController? = null
  private var controllerBridge: ControllerInputBridge? = null
  private var isControllerVisible = false
  private var inputManager: InputManager? = null
  private var hasPhysicalController = false
  private var pauseMenuOverlay: PauseMenuOverlay? = null

  private val prefs by lazy { getSharedPreferences("x1box_prefs", MODE_PRIVATE) }
  private var fpsTextView: TextView? = null
  private val fpsHandler = Handler(Looper.getMainLooper())
  private val fpsUpdateInterval = 1000L
  private var driverInfoStr = ""

  private val fpsRunnable = object : Runnable {
    override fun run() {
      val currentFps = nativeGetFps()
      if (driverInfoStr.isEmpty() || driverInfoStr.contains("initializing")) {
        try {
          val info = nativeGetDriverInfo()
          if (!info.contains("initializing")) {
            driverInfoStr = info
          }
        } catch (_: Exception) {}
      }
      val pacing = try { nativeGetFramePacing() } catch (_: Exception) { "" }
      val shaderStats = try { nativeGetShaderStats() } catch (_: Exception) { "" }
      val sb = StringBuilder("FPS: $currentFps")
      if (driverInfoStr.isNotEmpty()) sb.append(" | $driverInfoStr")
      if (pacing.isNotEmpty()) sb.append("\n$pacing")
      if (shaderStats.isNotEmpty()) sb.append("\n$shaderStats")
      fpsTextView?.text = sb.toString()
      fpsHandler.postDelayed(this, fpsUpdateInterval)
    }
  }

  private external fun nativeGetFps(): Int
  private external fun nativeGetDriverInfo(): String
  private external fun nativeGetFramePacing(): String
  private external fun nativeGetShaderStats(): String
  private external fun nativeCaptureFrame(): Boolean
  private external fun nativeDumpRenderTarget(): Unit
  private external fun nativeDumpDiagFrame(): Unit
  private external fun nativeExitEmulation(): Unit

  override fun loadLibraries() {
    super.loadLibraries()
    initializeGpuDriver()
  }

  private fun initializeGpuDriver() {
    GpuDriverHelper.init(this)
    if (GpuDriverHelper.supportsCustomDriverLoading()) {
      val driverLib = GpuDriverHelper.getInstalledDriverLibrary()
      if (driverLib != null) {
        android.util.Log.i("MainActivity", "GPU driver: loading custom driver=$driverLib")
        GpuDriverHelper.initializeDriver(driverLib)
      } else {
        android.util.Log.i("MainActivity", "GPU driver: no custom driver installed, using system default")
      }
    } else {
      android.util.Log.i("MainActivity", "GPU driver: custom loading not supported on this device")
    }
  }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    SDLActivity.nativeSetenv("SDL_ANDROID_TRAP_BACK_BUTTON", "1")
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
      maxLines = 4
    }
    val params = RelativeLayout.LayoutParams(
      RelativeLayout.LayoutParams.WRAP_CONTENT,
      RelativeLayout.LayoutParams.WRAP_CONTENT
    ).apply {
      addRule(RelativeLayout.ALIGN_PARENT_TOP)
      addRule(RelativeLayout.ALIGN_PARENT_START)
    }
    mLayout?.addView(fpsTextView, params)

    val captureBtn = Button(this).apply {
      text = "RDoc"
      setTextColor(Color.WHITE)
      setTextSize(TypedValue.COMPLEX_UNIT_SP, 10f)
      setBackgroundColor(Color.argb(160, 180, 0, 0))
      setPadding(16, 4, 16, 4)
      minHeight = 0
      minimumHeight = 0
      setOnClickListener {
        val ok = try { nativeCaptureFrame() } catch (_: Exception) { false }
        val msg = if (ok) "RenderDoc: capturing next frame" else "RenderDoc not connected"
        Toast.makeText(this@MainActivity, msg, Toast.LENGTH_SHORT).show()
      }
      setOnLongClickListener {
        try { nativeDumpDiagFrame() } catch (_: Exception) {}
        Toast.makeText(this@MainActivity, "Diag capture: next frame", Toast.LENGTH_SHORT).show()
        true
      }
    }
    val captureParams = RelativeLayout.LayoutParams(
      RelativeLayout.LayoutParams.WRAP_CONTENT,
      RelativeLayout.LayoutParams.WRAP_CONTENT
    ).apply {
      addRule(RelativeLayout.ALIGN_PARENT_TOP)
      addRule(RelativeLayout.ALIGN_PARENT_END)
      topMargin = 8
      marginEnd = 8
    }
    mLayout?.addView(captureBtn, captureParams)
  }

  private fun setupPauseMenu() {
    pauseMenuOverlay = PauseMenuOverlay(this).apply {
      onExitEmulation = {
        nativeExitEmulation()
        val intent = Intent(this@MainActivity, GameLibraryActivity::class.java).apply {
          flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK
        }
        startActivity(intent)
        finish()
      }
      onDismiss = {
        togglePauseMenu()
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
      overlay.dismiss()
    } else {
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

    // Add to layout
    mLayout?.addView(onScreenController)

    // Check for existing controllers and show/hide accordingly
    updateControllerVisibility()
  }

  override fun onResume() {
    super.onResume()
    
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
    
    val sources = device.sources
    
    // Check if device is a gamepad or joystick
    return ((sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD) ||
           ((sources and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)
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
