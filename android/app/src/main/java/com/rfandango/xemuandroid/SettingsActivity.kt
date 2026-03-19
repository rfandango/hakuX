package com.rfandango.xemuandroid

import android.net.Uri
import android.os.Bundle
import android.os.Environment
import android.view.View
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.materialswitch.MaterialSwitch
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder

class SettingsActivity : AppCompatActivity() {
  private val prefs by lazy { getSharedPreferences("x1box_prefs", MODE_PRIVATE) }

  private lateinit var driverStatusText: TextView
  private lateinit var gpuNotSupportedText: TextView
  private lateinit var btnInstallDriver: MaterialButton
  private lateinit var btnSelectDriver: MaterialButton
  private lateinit var btnResetDriver: MaterialButton
  private lateinit var switchShowFps: MaterialSwitch

  private val pickDriverZip =
    registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
      if (uri != null) {
        installDriverFromUri(uri)
      }
    }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    setContentView(R.layout.activity_settings)

    driverStatusText = findViewById(R.id.settings_gpu_driver_status)
    gpuNotSupportedText = findViewById(R.id.settings_gpu_not_supported)
    btnInstallDriver = findViewById(R.id.btn_install_driver)
    btnSelectDriver = findViewById(R.id.btn_select_driver)
    btnResetDriver = findViewById(R.id.btn_reset_driver)
    switchShowFps = findViewById(R.id.switch_show_fps)

    findViewById<View>(R.id.btn_settings_back).setOnClickListener { finish() }

    GpuDriverHelper.init(this)

    val supportsCustom = GpuDriverHelper.supportsCustomDriverLoading()

    if (!supportsCustom) {
      gpuNotSupportedText.visibility = View.VISIBLE
      btnInstallDriver.isEnabled = false
      btnSelectDriver.isEnabled = false
      btnResetDriver.isEnabled = false
    }

    btnInstallDriver.setOnClickListener {
      pickDriverZip.launch(arrayOf("application/zip", "application/octet-stream"))
    }

    btnSelectDriver.setOnClickListener {
      showDriverSelectionDialog()
    }

    btnResetDriver.setOnClickListener {
      confirmResetDriver()
    }

    switchShowFps.isChecked = prefs.getBoolean("show_fps", true)
    switchShowFps.setOnCheckedChangeListener { _, checked ->
      prefs.edit().putBoolean("show_fps", checked).apply()
    }

    val switchUnlockFps = findViewById<MaterialSwitch>(R.id.switch_unlock_framerate)
    switchUnlockFps.isChecked = prefs.getBoolean("unlock_framerate", true)
    switchUnlockFps.setOnCheckedChangeListener { _, checked ->
      prefs.edit().putBoolean("unlock_framerate", checked).apply()
    }

    val switchFpSafe = findViewById<MaterialSwitch>(R.id.switch_fp_safe)
    try {
      switchFpSafe.isChecked = nativeGetFpSafe()
    } catch (_: Throwable) {
      switchFpSafe.isChecked = prefs.getBoolean("fp_safe", true)
    }
    switchFpSafe.setOnCheckedChangeListener { _, checked ->
      prefs.edit().putBoolean("fp_safe", checked).apply()
      try { nativeSetFpSafe(checked) } catch (_: Throwable) {}
    }

    val switchFpJit = findViewById<MaterialSwitch>(R.id.switch_fp_jit)
    try {
      switchFpJit.isChecked = nativeGetFpJit()
    } catch (_: Throwable) {
      switchFpJit.isChecked = prefs.getBoolean("fp_jit", true)
    }
    switchFpJit.setOnCheckedChangeListener { _, checked ->
      prefs.edit().putBoolean("fp_jit", checked).apply()
      try { nativeSetFpJit(checked) } catch (_: Throwable) {}
    }

    val switchFastFences = findViewById<MaterialSwitch>(R.id.switch_fast_fences)
    try {
      switchFastFences.isChecked = nativeGetFastFences()
    } catch (_: Throwable) {
      switchFastFences.isChecked = prefs.getBoolean("fast_fences", false)
    }
    switchFastFences.setOnCheckedChangeListener { _, checked ->
      prefs.edit().putBoolean("fast_fences", checked).apply()
      try { nativeSetFastFences(checked) } catch (_: Throwable) {}
    }

    val switchDrawReorder = findViewById<MaterialSwitch>(R.id.switch_draw_reorder)
    try {
      switchDrawReorder.isChecked = nativeGetDrawReorder()
    } catch (_: Throwable) {
      switchDrawReorder.isChecked = prefs.getBoolean("draw_reorder", false)
    }
    switchDrawReorder.setOnCheckedChangeListener { _, checked ->
      prefs.edit().putBoolean("draw_reorder", checked).apply()
      try { nativeSetDrawReorder(checked) } catch (_: Throwable) {}
    }

    val switchBindless = findViewById<MaterialSwitch>(R.id.switch_bindless_textures)
    try {
      switchBindless.isChecked = nativeGetBindlessTextures()
    } catch (_: Throwable) {
      switchBindless.isChecked = prefs.getBoolean("bindless_textures", false)
    }
    switchBindless.setOnCheckedChangeListener { _, checked ->
      prefs.edit().putBoolean("bindless_textures", checked).apply()
      try { nativeSetBindlessTextures(checked) } catch (_: Throwable) {}
    }

    val switchAsyncCompile = findViewById<MaterialSwitch>(R.id.switch_async_compile)
    try {
      switchAsyncCompile.isChecked = nativeGetAsyncCompile()
    } catch (_: Throwable) {
      switchAsyncCompile.isChecked = prefs.getBoolean("async_compile", false)
    }
    switchAsyncCompile.setOnCheckedChangeListener { _, checked ->
      prefs.edit().putBoolean("async_compile", checked).apply()
      try { nativeSetAsyncCompile(checked) } catch (_: Throwable) {}
    }

    val switchFrameSkip = findViewById<MaterialSwitch>(R.id.switch_frame_skip)
    try {
      switchFrameSkip.isChecked = nativeGetFrameSkip()
    } catch (_: Throwable) {
      switchFrameSkip.isChecked = prefs.getBoolean("frame_skip", false)
    }
    switchFrameSkip.setOnCheckedChangeListener { _, checked ->
      prefs.edit().putBoolean("frame_skip", checked).apply()
      try { nativeSetFrameSkip(checked) } catch (_: Throwable) {}
    }

    setupSubmitFramesPicker()
    setupTier1ThresholdPicker()

    val switchValidation = findViewById<MaterialSwitch>(R.id.switch_validation_layers)
    switchValidation.isChecked = prefs.getBoolean("validation_layers", false)
    switchValidation.setOnCheckedChangeListener { _, checked ->
      prefs.edit().putBoolean("validation_layers", checked).apply()
    }

    findViewById<MaterialButton>(R.id.btn_clear_cache).setOnClickListener {
      confirmClearCache()
    }

    findViewById<MaterialButton>(R.id.btn_clear_shader_cache).setOnClickListener {
      confirmClearShaderCache()
    }

    findViewById<MaterialButton>(R.id.btn_recreate_hdd).setOnClickListener {
      confirmRecreateHdd()
    }

    setupResolutionScale()
    setupFilteringPicker()
    setupAspectRatioPicker()

    refreshDriverStatus()
  }

  private fun refreshDriverStatus() {
    val name = GpuDriverHelper.getInstalledDriverName()
    if (name != null) {
      driverStatusText.text = getString(R.string.settings_gpu_driver_active, name)
    } else {
      driverStatusText.text = getString(R.string.settings_gpu_driver_system)
    }
  }

  private fun setupResolutionScale() {
    val btn = findViewById<MaterialButton>(R.id.btn_resolution_scale)
    val labels = arrayOf("1x (Native)", "2x", "3x", "4x")
    val values = intArrayOf(1, 2, 3, 4)
    val current = prefs.getInt("surface_scale", 1)
    val idx = values.indexOf(current).coerceAtLeast(0)
    btn.text = labels[idx]
    btn.setOnClickListener {
      val sel = values.indexOf(prefs.getInt("surface_scale", 1)).coerceAtLeast(0)
      MaterialAlertDialogBuilder(this)
        .setTitle(R.string.settings_resolution_scale)
        .setSingleChoiceItems(labels, sel) { dialog, which ->
          prefs.edit().putInt("surface_scale", values[which]).apply()
          btn.text = labels[which]
          dialog.dismiss()
        }
        .setNegativeButton(android.R.string.cancel, null)
        .show()
    }
  }

  private fun setupFilteringPicker() {
    val btn = findViewById<MaterialButton>(R.id.btn_filtering)
    val labels = arrayOf("Nearest", "Linear")
    val keys = arrayOf("nearest", "linear")
    val current = prefs.getString("filtering", "nearest") ?: "nearest"
    val idx = keys.indexOf(current).coerceAtLeast(0)
    btn.text = labels[idx]
    btn.setOnClickListener {
      val cur = prefs.getString("filtering", "nearest") ?: "nearest"
      val sel = keys.indexOf(cur).coerceAtLeast(0)
      MaterialAlertDialogBuilder(this)
        .setTitle(R.string.settings_filtering)
        .setSingleChoiceItems(labels, sel) { dialog, which ->
          prefs.edit().putString("filtering", keys[which]).apply()
          btn.text = labels[which]
          dialog.dismiss()
        }
        .setNegativeButton(android.R.string.cancel, null)
        .show()
    }
  }

  private fun setupAspectRatioPicker() {
    val btn = findViewById<MaterialButton>(R.id.btn_aspect_ratio)
    val labels = arrayOf("Auto", "Native (4:3)", "16:9", "Fit")
    val keys = arrayOf("auto", "native", "16:9", "fit")
    val current = prefs.getString("aspect_ratio", "auto") ?: "auto"
    val idx = keys.indexOf(current).coerceAtLeast(0)
    btn.text = labels[idx]
    btn.setOnClickListener {
      val cur = prefs.getString("aspect_ratio", "auto") ?: "auto"
      val sel = keys.indexOf(cur).coerceAtLeast(0)
      MaterialAlertDialogBuilder(this)
        .setTitle(R.string.settings_aspect_ratio)
        .setSingleChoiceItems(labels, sel) { dialog, which ->
          prefs.edit().putString("aspect_ratio", keys[which]).apply()
          btn.text = labels[which]
          dialog.dismiss()
        }
        .setNegativeButton(android.R.string.cancel, null)
        .show()
    }
  }

  private fun setupSubmitFramesPicker() {
    val btn = findViewById<MaterialButton>(R.id.btn_submit_frames)
    val labels = arrayOf("Single (1)", "Double (2)", "Triple (3)")
    val values = intArrayOf(1, 2, 3)
    var current = try { nativeGetSubmitFrames() } catch (_: Throwable) {
      prefs.getInt("submit_frames", 3)
    }
    val idx = values.indexOf(current).coerceAtLeast(0)
    btn.text = labels[idx]
    btn.setOnClickListener {
      val cur = try { nativeGetSubmitFrames() } catch (_: Throwable) {
        prefs.getInt("submit_frames", 3)
      }
      val sel = values.indexOf(cur).coerceAtLeast(0)
      MaterialAlertDialogBuilder(this)
        .setTitle(R.string.settings_submit_frames)
        .setSingleChoiceItems(labels, sel) { dialog, which ->
          prefs.edit().putInt("submit_frames", values[which]).apply()
          try { nativeSetSubmitFrames(values[which]) } catch (_: Throwable) {}
          btn.text = labels[which]
          dialog.dismiss()
        }
        .setNegativeButton(android.R.string.cancel, null)
        .show()
    }
  }

  private fun setupTier1ThresholdPicker() {
    val btn = findViewById<MaterialButton>(R.id.btn_tier1_threshold)
    val labels = arrayOf("Aggressive (16)", "Early (32)", "Default (64)", "Conservative (128)", "Lazy (256)")
    val values = intArrayOf(16, 32, 64, 128, 256)
    val current = try { nativeGetTier1Threshold() } catch (_: Throwable) {
      prefs.getInt("tier1_threshold", 64)
    }
    val idx = values.indexOf(current).coerceAtLeast(2)
    btn.text = labels[idx]
    btn.setOnClickListener {
      val cur = try { nativeGetTier1Threshold() } catch (_: Throwable) {
        prefs.getInt("tier1_threshold", 64)
      }
      val sel = values.indexOf(cur).coerceAtLeast(2)
      MaterialAlertDialogBuilder(this)
        .setTitle(R.string.settings_tier1_threshold)
        .setSingleChoiceItems(labels, sel) { dialog, which ->
          prefs.edit().putInt("tier1_threshold", values[which]).apply()
          try { nativeSetTier1Threshold(values[which]) } catch (_: Throwable) {}
          btn.text = labels[which]
          dialog.dismiss()
        }
        .setNegativeButton(android.R.string.cancel, null)
        .show()
    }
  }

  private fun installDriverFromUri(uri: Uri) {
    Thread {
      val success = GpuDriverHelper.installDriverFromUri(this, uri)
      runOnUiThread {
        if (success) {
          Toast.makeText(this, getString(R.string.settings_gpu_driver_installed), Toast.LENGTH_SHORT).show()
          refreshDriverStatus()
        } else {
          Toast.makeText(this, getString(R.string.settings_gpu_driver_install_failed), Toast.LENGTH_SHORT).show()
        }
      }
    }.start()
  }

  private fun showDriverSelectionDialog() {
    val drivers = GpuDriverHelper.getAvailableDrivers()
    if (drivers.isEmpty()) {
      Toast.makeText(this, getString(R.string.settings_gpu_driver_none_available), Toast.LENGTH_SHORT).show()
      return
    }

    val labels = drivers.map { driver ->
      buildString {
        append(driver.name ?: "Unknown")
        if (!driver.description.isNullOrBlank()) {
          append("\n")
          append(driver.description)
        }
        if (!driver.author.isNullOrBlank()) {
          append("\nby ")
          append(driver.author)
        }
      }
    }.toTypedArray()

    MaterialAlertDialogBuilder(this)
      .setTitle(R.string.settings_gpu_driver_select_title)
      .setItems(labels) { _, which ->
        val selected = drivers[which]
        if (selected.path != null) {
          val zipFile = java.io.File(selected.path)
          val success = GpuDriverHelper.installDriver(zipFile)
          if (success) {
            Toast.makeText(this, getString(R.string.settings_gpu_driver_installed), Toast.LENGTH_SHORT).show()
            refreshDriverStatus()
          } else {
            Toast.makeText(this, getString(R.string.settings_gpu_driver_install_failed), Toast.LENGTH_SHORT).show()
          }
        }
      }
      .setNegativeButton(android.R.string.cancel, null)
      .show()
  }

  private fun confirmResetDriver() {
    MaterialAlertDialogBuilder(this)
      .setTitle(R.string.settings_gpu_driver_reset_title)
      .setMessage(R.string.settings_gpu_driver_reset_message)
      .setPositiveButton(R.string.settings_gpu_driver_reset) { _, _ ->
        GpuDriverHelper.installDefaultDriver()
        Toast.makeText(this, getString(R.string.settings_gpu_driver_reset_done), Toast.LENGTH_SHORT).show()
        refreshDriverStatus()
      }
      .setNegativeButton(android.R.string.cancel, null)
      .show()
  }

  private fun resolveHddPath(): String? {
    val tag = "XemuHddCache"
    val direct = prefs.getString("hddPath", null)
    Log.i(tag, "resolveHddPath: pref hddPath='${direct ?: "(null)"}'")
    if (!direct.isNullOrEmpty()) {
      val f = File(direct)
      Log.i(tag, "  direct exists=${f.exists()} length=${if (f.exists()) f.length() else -1}")
      if (f.exists()) return direct
    }
    val extDir = getExternalFilesDir(null)
    Log.i(tag, "  extDir=${extDir?.absolutePath ?: "(null)"}")
    if (extDir != null) {
      val extPath = extDir.absolutePath + "/x1box/hdd.img"
      val ef = File(extPath)
      Log.i(tag, "  extPath=$extPath exists=${ef.exists()} length=${if (ef.exists()) ef.length() else -1}")
      if (ef.exists()) return extPath
    }
    val internalPath = filesDir.absolutePath + "/x1box/hdd.img"
    val inf = File(internalPath)
    Log.i(tag, "  internalPath=$internalPath exists=${inf.exists()} length=${if (inf.exists()) inf.length() else -1}")
    if (inf.exists()) return internalPath
    Log.w(tag, "  no HDD image found at any candidate path")
    return null
  }

  private fun confirmClearShaderCache() {
    MaterialAlertDialogBuilder(this)
      .setTitle(R.string.settings_clear_shader_cache_confirm_title)
      .setMessage(R.string.settings_clear_shader_cache_confirm_message)
      .setPositiveButton(R.string.settings_clear_shader_cache) { _, _ ->
        try {
          val tag = "XemuShaderCache"
          val baseDir = filesDir
          Log.i(tag, "clearShaderCache: baseDir=${baseDir.absolutePath}")

          val spvDir = File(baseDir, "spv_cache")
          if (spvDir.isDirectory) {
            val files = spvDir.listFiles()
            Log.i(tag, "  spv_cache: ${files?.size ?: 0} files")
            files?.forEach { it.delete() }
            val rmDir = spvDir.delete()
            Log.i(tag, "  spv_cache dir removed=$rmDir")
          } else {
            Log.i(tag, "  spv_cache: not found")
          }

          val plc = File(baseDir, "vk_pipeline_cache.bin")
          Log.i(tag, "  vk_pipeline_cache.bin exists=${plc.exists()} deleted=${plc.delete()}")
          val smk = File(baseDir, "shader_module_keys.bin")
          Log.i(tag, "  shader_module_keys.bin exists=${smk.exists()} deleted=${smk.delete()}")

          Toast.makeText(this, getString(R.string.settings_clear_shader_cache_success), Toast.LENGTH_SHORT).show()
        } catch (e: Exception) {
          Log.e("XemuShaderCache", "clearShaderCache failed", e)
          Toast.makeText(this, getString(R.string.settings_clear_shader_cache_failed, e.message), Toast.LENGTH_LONG).show()
        }
      }
      .setNegativeButton(android.R.string.cancel, null)
      .show()
  }

  private fun confirmClearCache() {
    val tag = "XemuHddCache"
    val hddPath = resolveHddPath()
    if (hddPath == null) {
      Log.e(tag, "confirmClearCache: no HDD path resolved")
      Toast.makeText(this, getString(R.string.settings_clear_cache_no_hdd), Toast.LENGTH_LONG).show()
      return
    }
    Log.i(tag, "confirmClearCache: resolved path=$hddPath")
    MaterialAlertDialogBuilder(this)
      .setTitle(R.string.settings_clear_cache_confirm_title)
      .setMessage(R.string.settings_clear_cache_confirm_message)
      .setPositiveButton(R.string.settings_clear_cache) { _, _ ->
        try {
          clearHddCachePartitions(hddPath)
          Toast.makeText(this, getString(R.string.settings_clear_cache_success), Toast.LENGTH_SHORT).show()
        } catch (e: Exception) {
          Log.e(tag, "clearHddCachePartitions failed", e)
          Toast.makeText(this, getString(R.string.settings_clear_cache_failed, e.message), Toast.LENGTH_LONG).show()
        }
      }
      .setNegativeButton(android.R.string.cancel, null)
      .show()
  }

  private fun confirmRecreateHdd() {
    val tag = "XemuHddRecreate"
    val sourceUriStr = prefs.getString("hddUri", null)
    if (sourceUriStr.isNullOrEmpty()) {
      Log.e(tag, "confirmRecreateHdd: no source URI in prefs")
      Toast.makeText(this, getString(R.string.settings_recreate_hdd_no_source), Toast.LENGTH_LONG).show()
      return
    }

    MaterialAlertDialogBuilder(this)
      .setTitle(R.string.settings_recreate_hdd_confirm_title)
      .setMessage(R.string.settings_recreate_hdd_confirm_message)
      .setPositiveButton(R.string.settings_recreate_hdd) { _, _ ->
        recreateHddFromSource(sourceUriStr, tag)
      }
      .setNegativeButton(android.R.string.cancel, null)
      .show()
  }

  private fun recreateHddFromSource(sourceUriStr: String, tag: String) {
    Toast.makeText(this, getString(R.string.settings_recreate_hdd_copying), Toast.LENGTH_SHORT).show()

    Thread {
      try {
        val sourceUri = Uri.parse(sourceUriStr)
        val base = getExternalFilesDir(null) ?: filesDir
        val dir = File(base, "x1box")
        if (!dir.exists() && !dir.mkdirs()) {
          throw IOException("Failed to create directory ${dir.absolutePath}")
        }
        val target = File(dir, "hdd.img")

        Log.i(tag, "recreateHdd: source=$sourceUriStr target=${target.absolutePath}")

        contentResolver.openInputStream(sourceUri)?.use { input ->
          FileOutputStream(target).use { output ->
            input.copyTo(output)
          }
        } ?: throw IOException("Unable to open source HDD image")

        val newPath = target.absolutePath
        prefs.edit().putString("hddPath", newPath).apply()
        Log.i(tag, "recreateHdd: done, size=${target.length()}")

        runOnUiThread {
          Toast.makeText(this, getString(R.string.settings_recreate_hdd_success), Toast.LENGTH_SHORT).show()
        }
      } catch (e: Exception) {
        Log.e(tag, "recreateHdd failed", e)
        runOnUiThread {
          Toast.makeText(this, getString(R.string.settings_recreate_hdd_failed, e.message), Toast.LENGTH_LONG).show()
        }
      }
    }.start()
  }

  private external fun nativeGetFpSafe(): Boolean
  private external fun nativeSetFpSafe(enable: Boolean)
  private external fun nativeGetFpJit(): Boolean
  private external fun nativeSetFpJit(enable: Boolean)
  private external fun nativeGetFastFences(): Boolean
  private external fun nativeSetFastFences(enable: Boolean)
  private external fun nativeGetDrawReorder(): Boolean
  private external fun nativeSetDrawReorder(enable: Boolean)
  private external fun nativeGetBindlessTextures(): Boolean
  private external fun nativeSetBindlessTextures(enable: Boolean)
  private external fun nativeGetAsyncCompile(): Boolean
  private external fun nativeSetAsyncCompile(enable: Boolean)
  private external fun nativeGetFrameSkip(): Boolean
  private external fun nativeSetFrameSkip(enable: Boolean)
  private external fun nativeGetSubmitFrames(): Int
  private external fun nativeSetSubmitFrames(count: Int)
  private external fun nativeGetTier1Threshold(): Int
  private external fun nativeSetTier1Threshold(value: Int)

  companion object {
    private const val FATX_SUPERBLOCK_SIZE = 4096
    private const val FATX_MAGIC = 0x46415458 // "FATX" read as big-endian int
    private const val QCOW2_MAGIC = 0x514649fb.toInt() // "QFI\xfb"
    private const val WIPE_SIZE = 1 * 1024 * 1024L

    private val STANDARD_CACHE_OFFSETS = longArrayOf(
      0x00080000L, // X
      0x2EE80000L, // Y
      0x5DC80000L, // Z
    )
  }

  private fun clearHddCachePartitions(hddPath: String) {
    val tag = "XemuHddCache"
    val file = File(hddPath)
    val imageLen = file.length()
    Log.i(tag, "clearHddCachePartitions: path=$hddPath length=$imageLen " +
      "(0x${imageLen.toString(16)}) canWrite=${file.canWrite()}")
    require(file.exists() && imageLen > 0) { "HDD image missing or empty" }

    RandomAccessFile(file, "rw").use { raf ->
      raf.seek(0)
      val fileMagic = raf.readInt()

      if (fileMagic == QCOW2_MAGIC) {
        Log.i(tag, "  format: QCOW2")
        clearCacheQcow2(raf, tag)
      } else {
        Log.i(tag, "  format: raw")
        clearCacheRaw(raf, imageLen, tag)
      }
    }
  }

  private fun clearCacheRaw(raf: RandomAccessFile, imageLen: Long, tag: String) {
    var cleared = 0
    for (offset in STANDARD_CACHE_OFFSETS) {
      if (offset + FATX_SUPERBLOCK_SIZE > imageLen) {
        Log.i(tag, "  raw@0x${offset.toString(16)}: beyond image, skip")
        continue
      }
      raf.seek(offset)
      val sig = raf.readInt()
      if (sig != FATX_MAGIC) {
        Log.i(tag, "  raw@0x${offset.toString(16)}: no FATX (0x${sig.toString(16)}), skip")
        continue
      }
      Log.i(tag, "  raw@0x${offset.toString(16)}: FATX found, reformatting")
      val wipeLen = minOf(WIPE_SIZE, imageLen - offset).toInt()
      raf.seek(offset)
      raf.write(ByteArray(wipeLen))
      writeFatxSuperblock(raf, offset)
      cleared++
    }
    if (cleared == 0) {
      throw IllegalStateException("No cache partitions found in raw image ($imageLen bytes)")
    }
    Log.i(tag, "  raw: $cleared partition(s) cleared")
  }

  private fun clearCacheQcow2(raf: RandomAccessFile, tag: String) {
    raf.seek(4)
    val version = raf.readInt()
    raf.seek(20)
    val clusterBits = raf.readInt()
    val virtualSize = raf.readLong()
    raf.seek(36)
    val l1Size = raf.readInt()
    val l1TableOffset = raf.readLong()
    val clusterSize = 1 shl clusterBits
    val l2Bits = clusterBits - 3

    Log.i(tag, "  qcow2: v$version clusterBits=$clusterBits clusterSize=$clusterSize " +
      "virtualSize=0x${virtualSize.toString(16)} ($virtualSize) " +
      "l1Size=$l1Size l1TableOff=0x${l1TableOffset.toString(16)}")

    var cleared = 0
    for (partOff in STANDARD_CACHE_OFFSETS) {
      if (partOff >= virtualSize) {
        Log.i(tag, "  qcow2@0x${partOff.toString(16)}: beyond virtual disk, skip")
        continue
      }

      val sigPhys = qcow2Resolve(raf, partOff, clusterBits, l2Bits, l1Size, l1TableOffset)
      if (sigPhys < 0) {
        Log.i(tag, "  qcow2@0x${partOff.toString(16)}: unallocated, skip")
        continue
      }
      raf.seek(sigPhys)
      val sig = raf.readInt()
      if (sig != FATX_MAGIC) {
        Log.i(tag, "  qcow2@0x${partOff.toString(16)}: no FATX (0x${sig.toString(16)}), skip")
        continue
      }

      Log.i(tag, "  qcow2@0x${partOff.toString(16)}: FATX found, reformatting")

      val wipeEnd = minOf(partOff + WIPE_SIZE, virtualSize)
      var vOff = partOff
      while (vOff < wipeEnd) {
        val inCluster = (vOff and (clusterSize - 1).toLong()).toInt()
        val chunk = minOf((clusterSize - inCluster).toLong(), wipeEnd - vOff).toInt()
        val phys = qcow2Resolve(raf, vOff, clusterBits, l2Bits, l1Size, l1TableOffset)
        if (phys >= 0) {
          raf.seek(phys)
          raf.write(ByteArray(chunk))
        }
        vOff += chunk
      }

      val sbPhys = qcow2Resolve(raf, partOff, clusterBits, l2Bits, l1Size, l1TableOffset)
      if (sbPhys >= 0) {
        writeFatxSuperblockAt(raf, sbPhys)
        val fatVirt = partOff + FATX_SUPERBLOCK_SIZE
        val fatPhys = qcow2Resolve(raf, fatVirt, clusterBits, l2Bits, l1Size, l1TableOffset)
        if (fatPhys >= 0) {
          raf.seek(fatPhys)
          val fatEntry = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN)
          fatEntry.putInt(0xFFFFFFF8.toInt())
          raf.write(fatEntry.array())
        }
      }
      cleared++
    }

    if (cleared == 0) {
      throw IllegalStateException(
        "No cache partitions found in QCOW2 image (virtualSize=0x${virtualSize.toString(16)})")
    }
    Log.i(tag, "  qcow2: $cleared partition(s) cleared")
  }

  private fun qcow2Resolve(
    raf: RandomAccessFile, virtOff: Long,
    clusterBits: Int, l2Bits: Int, l1Size: Int, l1TableOffset: Long
  ): Long {
    val clusterSize = 1 shl clusterBits
    val l1Idx = (virtOff ushr (clusterBits + l2Bits)).toInt()
    val l2Idx = ((virtOff ushr clusterBits) and ((1 shl l2Bits) - 1).toLong()).toInt()
    val inCluster = (virtOff and (clusterSize - 1).toLong()).toInt()
    if (l1Idx >= l1Size) return -1

    raf.seek(l1TableOffset + l1Idx.toLong() * 8)
    val l1Entry = raf.readLong()
    val l2Off = l1Entry and 0x00fffffffffffe00L
    if (l2Off == 0L) return -1

    raf.seek(l2Off + l2Idx.toLong() * 8)
    val l2Entry = raf.readLong()
    val dataOff = l2Entry and 0x00fffffffffffe00L
    if (dataOff == 0L) return -1

    return dataOff + inCluster
  }

  private fun writeFatxSuperblock(raf: RandomAccessFile, offset: Long) {
    raf.seek(offset)
    writeFatxSuperblockAt(raf, offset)
    if (raf.filePointer + 4 <= raf.length()) {
      val fatEntry = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN)
      fatEntry.putInt(0xFFFFFFF8.toInt())
      raf.write(fatEntry.array())
    }
  }

  private fun writeFatxSuperblockAt(raf: RandomAccessFile, physOffset: Long) {
    val sb = ByteBuffer.allocate(FATX_SUPERBLOCK_SIZE).order(ByteOrder.BIG_ENDIAN)
    sb.putInt(FATX_MAGIC)
    sb.order(ByteOrder.LITTLE_ENDIAN)
    sb.putInt((System.nanoTime() and 0xFFFFFFFFL).toInt())
    sb.putInt(32) // sectors_per_cluster
    sb.putInt(1)  // root_cluster
    sb.putShort(0)
    val remaining = FATX_SUPERBLOCK_SIZE - sb.position()
    for (i in 0 until remaining) sb.put(0xFF.toByte())
    raf.seek(physOffset)
    raf.write(sb.array())
  }
}
