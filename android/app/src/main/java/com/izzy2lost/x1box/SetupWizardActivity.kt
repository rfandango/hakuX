package com.izzy2lost.x1box

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.util.Log
import android.view.View
import android.widget.TextView
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.documentfile.provider.DocumentFile
import com.google.android.material.button.MaterialButton
import java.io.File
import java.io.FileOutputStream
import java.io.IOException

class SetupWizardActivity : AppCompatActivity() {
  private val prefs by lazy { getSharedPreferences("x1box_prefs", MODE_PRIVATE) }

  private lateinit var pageMcpx: View
  private lateinit var pageFlash: View
  private lateinit var pageHdd: View
  private lateinit var pageDisc: View
  private lateinit var pages: List<View>
  private lateinit var mcpxPathText: TextView
  private lateinit var flashPathText: TextView
  private lateinit var hddPathText: TextView
  private lateinit var discPathText: TextView
  private lateinit var btnBack: MaterialButton
  private lateinit var btnNext: MaterialButton
  private lateinit var indicatorMcpx: View
  private lateinit var indicatorFlash: View
  private lateinit var indicatorHdd: View
  private lateinit var indicatorDisc: View
  private lateinit var indicators: List<View>

  private var mcpxUri: Uri? = null
  private var flashUri: Uri? = null
  private var hddUri: Uri? = null
  private var gamesFolderUri: Uri? = null
  private var mcpxPath: String? = null
  private var flashPath: String? = null
  private var hddPath: String? = null
  private var currentStep = 0
  private var isCopying = false

  private val mcpxExts = setOf("bin", "rom", "img")
  private val flashExts = setOf("bin", "rom", "img")
  private val hddExts = setOf("qcow2", "img")

  private val pickMcpx =
    registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
      if (uri != null) {
        if (!isAllowedExtension(uri, mcpxExts)) {
          showExtensionError(mcpxExts)
          return@registerForActivityResult
        }
        persistUriPermission(uri)
        mcpxUri = uri
        prefs.edit().putString("mcpxUri", uri.toString()).apply()
        copyUriAsync(uri, "mcpx.bin") { path ->
          if (path != null) {
            mcpxPath = path
            prefs.edit().putString("mcpxPath", path).apply()
          } else {
            Toast.makeText(this, "Failed to copy MCPX ROM", Toast.LENGTH_LONG).show()
          }
          updateMcpxSelection()
          updateButtons()
        }
      }
    }

  private val pickFlash =
    registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
      if (uri != null) {
        if (!isAllowedExtension(uri, flashExts)) {
          showExtensionError(flashExts)
          return@registerForActivityResult
        }
        persistUriPermission(uri)
        flashUri = uri
        prefs.edit().putString("flashUri", uri.toString()).apply()
        copyUriAsync(uri, "flash.bin") { path ->
          if (path != null) {
            flashPath = path
            prefs.edit().putString("flashPath", path).apply()
          } else {
            Toast.makeText(this, "Failed to copy flash ROM", Toast.LENGTH_LONG).show()
          }
          updateFlashSelection()
          updateButtons()
        }
      }
    }

  private val pickHdd =
    registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
      if (uri != null) {
        if (!isAllowedExtension(uri, hddExts)) {
          showExtensionError(hddExts)
          return@registerForActivityResult
        }
        persistUriPermission(uri)
        hddUri = uri
        prefs.edit().putString("hddUri", uri.toString()).apply()
        copyUriAsync(uri, "hdd.img") { path ->
          if (path != null) {
            hddPath = path
            prefs.edit().putString("hddPath", path).apply()
          } else {
            Toast.makeText(this, "Failed to copy HDD image", Toast.LENGTH_LONG).show()
          }
          updateHddSelection()
          updateButtons()
        }
      }
    }

  private val pickGamesFolder =
    registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
      if (uri != null) {
        persistUriPermission(uri)
        gamesFolderUri = uri
        prefs.edit().putString("gamesFolderUri", uri.toString()).apply()
        updateDiscSelection()
        updateButtons()
      }
    }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)

    mcpxPath = loadLocalPath("mcpxPath")
    flashPath = loadLocalPath("flashPath")
    hddPath = loadLocalPath("hddPath")
    mcpxUri = prefs.getString("mcpxUri", null)?.let(Uri::parse)
    flashUri = prefs.getString("flashUri", null)?.let(Uri::parse)
    hddUri = prefs.getString("hddUri", null)?.let(Uri::parse)
    gamesFolderUri = prefs.getString("gamesFolderUri", null)?.let(Uri::parse)

    val coreReady = isFileReady(mcpxPath) && isFileReady(flashPath) && isFileReady(hddPath)
    val gamesFolderReady = hasGamesFolderReady()
    if (prefs.getBoolean("setup_complete", false) && coreReady && gamesFolderReady) {
      goToLibrary()
      return
    }

    setContentView(R.layout.activity_setup_wizard)

    val setupRoot: View = findViewById(R.id.setup_root)
    val setupCard: View = findViewById(R.id.setup_card)
    setupRoot.post {
      val target = (setupRoot.height * 0.92f).toInt()
      if (target > 0) {
        setupCard.minimumHeight = target
      }
    }

    pageMcpx = findViewById(R.id.page_mcpx)
    pageFlash = findViewById(R.id.page_flash)
    pageHdd = findViewById(R.id.page_hdd)
    pageDisc = findViewById(R.id.page_disc)
    pages = listOf(pageMcpx, pageFlash, pageHdd, pageDisc)
    mcpxPathText = findViewById(R.id.mcpx_path_text)
    flashPathText = findViewById(R.id.flash_path_text)
    hddPathText = findViewById(R.id.hdd_path_text)
    discPathText = findViewById(R.id.disc_path_text)
    btnBack = findViewById(R.id.btn_wizard_back)
    btnNext = findViewById(R.id.btn_wizard_next)
    indicatorMcpx = findViewById(R.id.step_indicator_mcpx)
    indicatorFlash = findViewById(R.id.step_indicator_flash)
    indicatorHdd = findViewById(R.id.step_indicator_hdd)
    indicatorDisc = findViewById(R.id.step_indicator_disc)
    indicators = listOf(indicatorMcpx, indicatorFlash, indicatorHdd, indicatorDisc)

    val btnPickMcpx: MaterialButton = findViewById(R.id.btn_pick_mcpx)
    val btnPickFlash: MaterialButton = findViewById(R.id.btn_pick_flash)
    val btnPickHdd: MaterialButton = findViewById(R.id.btn_pick_hdd)
    val btnPickDisc: MaterialButton = findViewById(R.id.btn_pick_disc)

    btnPickMcpx.setOnClickListener { pickMcpx.launch(arrayOf("application/octet-stream")) }
    btnPickFlash.setOnClickListener { pickFlash.launch(arrayOf("application/octet-stream")) }
    btnPickHdd.setOnClickListener { pickHdd.launch(arrayOf("application/x-qcow2", "application/octet-stream")) }
    btnPickDisc.setOnClickListener { pickGamesFolder.launch(gamesFolderUri) }

    btnBack.setOnClickListener { showStep(currentStep - 1) }
    btnNext.setOnClickListener {
      if (currentStep < pages.size - 1) {
        showStep(currentStep + 1)
      } else {
        finishSetup()
      }
    }

    onBackPressedDispatcher.addCallback(
      this,
      object : OnBackPressedCallback(true) {
        override fun handleOnBackPressed() {
          if (currentStep > 0) {
            showStep(currentStep - 1)
          } else {
            finish()
          }
        }
      }
    )

    updateMcpxSelection()
    updateFlashSelection()
    updateHddSelection()
    updateDiscSelection()

    val startStep = if (coreReady && !gamesFolderReady) pages.size - 1 else 0
    showStep(startStep)
  }

  private fun showStep(step: Int) {
    val lastIndex = (pages.size - 1).coerceAtLeast(0)
    currentStep = step.coerceIn(0, lastIndex)
    pages.forEachIndexed { index, view ->
      view.visibility = if (currentStep == index) View.VISIBLE else View.GONE
    }
    indicators.forEachIndexed { index, view ->
      view.setBackgroundResource(
        if (currentStep == index) {
          R.drawable.setup_wizard_indicator_active
        } else {
          R.drawable.setup_wizard_indicator_inactive
        }
      )
    }

    updateButtons()
  }

  private fun updateButtons() {
    btnBack.visibility = if (currentStep == 0) View.INVISIBLE else View.VISIBLE
    btnNext.text =
      getString(if (currentStep == pages.size - 1) R.string.setup_finish else R.string.setup_next)
    if (isCopying) {
      btnNext.isEnabled = false
      btnBack.isEnabled = false
      return
    }
    btnBack.isEnabled = true
    btnNext.isEnabled =
      when (currentStep) {
        0 -> isFileReady(mcpxPath)
        1 -> isFileReady(flashPath)
        2 -> isFileReady(hddPath)
        else -> hasGamesFolderReady()
      }
  }

  private fun updateMcpxSelection() {
    val value = mcpxPath ?: mcpxUri?.toString() ?: getString(R.string.setup_not_set)
    mcpxPathText.text = getString(R.string.setup_mcpx_value, value)
  }

  private fun updateFlashSelection() {
    val value = flashPath ?: flashUri?.toString() ?: getString(R.string.setup_not_set)
    flashPathText.text = getString(R.string.setup_flash_value, value)
  }

  private fun updateHddSelection() {
    val value = hddPath ?: hddUri?.toString() ?: getString(R.string.setup_not_set)
    hddPathText.text = getString(R.string.setup_hdd_value, value)
  }

  private fun updateDiscSelection() {
    val value = gamesFolderUri?.let { formatTreeLabel(it) } ?: getString(R.string.setup_not_set)
    discPathText.text = getString(R.string.setup_disc_value, value)
  }


  private fun finishSetup() {
    prefs.edit()
      .putBoolean("setup_complete", true)
      .putBoolean("skip_game_picker", false)
      .apply()
    goToLibrary()
  }

  private fun goToLibrary() {
    startActivity(Intent(this, GameLibraryActivity::class.java))
    finish()
  }

  private fun persistUriPermission(uri: Uri) {
    val flags = Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
    try {
      contentResolver.takePersistableUriPermission(uri, flags)
    } catch (_: SecurityException) {
    }
  }

  private fun loadLocalPath(key: String): String? {
    val path = prefs.getString(key, null) ?: return null
    if (!File(path).isFile) {
      prefs.edit().remove(key).apply()
      return null
    }
    return path
  }

  private fun isFileReady(path: String?): Boolean {
    return path != null && File(path).isFile
  }

  private fun hasGamesFolderReady(): Boolean {
    val uri = gamesFolderUri ?: return false
    if (!hasPersistedReadPermission(uri)) {
      return false
    }
    val root = DocumentFile.fromTreeUri(this, uri) ?: return false
    return root.exists() && root.isDirectory
  }

  private fun hasPersistedReadPermission(uri: Uri): Boolean {
    return contentResolver.persistedUriPermissions.any { perm ->
      perm.uri == uri && perm.isReadPermission
    }
  }

  private fun formatTreeLabel(uri: Uri): String {
    val name = DocumentFile.fromTreeUri(this, uri)?.name
    if (!name.isNullOrBlank()) {
      return name
    }
    return uri.toString()
  }

  private fun copyUriAsync(uri: Uri, destName: String, onDone: (String?) -> Unit) {
    if (isCopying) return
    isCopying = true
    updateButtons()
    Toast.makeText(this, "Copying file...", Toast.LENGTH_SHORT).show()
    Thread {
      val path = copyUriToAppStorage(uri, destName)
      runOnUiThread {
        isCopying = false
        onDone(path)
      }
    }.start()
  }

  private fun copyUriToAppStorage(uri: Uri, destName: String): String? {
    val tag = "xemu-android"
    val base = getExternalFilesDir(null) ?: filesDir
    val dir = File(base, "x1box")
    if (!dir.exists() && !dir.mkdirs()) {
      Log.e(tag, "Failed to create ${dir.absolutePath}")
      return null
    }
    val target = File(dir, destName)
    Log.i(tag, "copyUriToAppStorage: $uri -> ${target.absolutePath}")
    return try {
      val bytesCopied = contentResolver.openInputStream(uri)?.use { input ->
        FileOutputStream(target).use { output ->
          input.copyTo(output)
        }
      } ?: return null
      Log.i(tag, "copyUriToAppStorage: done  bytes=$bytesCopied  fileSize=${target.length()}")
      if (bytesCopied == 0L) {
        Log.e(tag, "copyUriToAppStorage: source was empty or unreadable!")
        target.delete()
        return null
      }
      target.absolutePath
    } catch (e: IOException) {
      Log.e(tag, "Copy failed for $destName", e)
      target.delete()
      null
    }
  }

  private fun isAllowedExtension(uri: Uri, allowed: Set<String>): Boolean {
    return isAllowedFile(uri, allowed, emptySet())
  }

  private fun isAllowedFile(uri: Uri, allowedExts: Set<String>, allowedMimes: Set<String>): Boolean {
    val name = contentResolver.query(uri, null, null, null, null)?.use { cursor ->
      val nameIndex = cursor.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME)
      if (nameIndex >= 0 && cursor.moveToFirst()) cursor.getString(nameIndex) else null
    } ?: uri.lastPathSegment
    val lowerName = name?.lowercase().orEmpty()
    if (lowerName.endsWith(".xiso.iso")) return true
    val ext = lowerName.substringAfterLast('.', "")
    if (ext.isNotEmpty() && allowedExts.contains(ext)) return true
    val mime = contentResolver.getType(uri)?.lowercase()
    if (mime != null) {
      if (allowedMimes.contains(mime)) return true
      if (mime.startsWith("application/x-iso")) return true
    }
    return false
  }

  private fun showExtensionError(allowed: Set<String>) {
    val pretty = allowed.sorted().joinToString(separator = ", ") { ".$it" }
    Toast.makeText(this, "Please pick a file with one of: $pretty", Toast.LENGTH_LONG).show()
  }
}
