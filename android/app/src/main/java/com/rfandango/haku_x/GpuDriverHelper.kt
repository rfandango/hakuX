package com.rfandango.haku_x

import android.content.Context
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.provider.OpenableColumns
import android.util.Log
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.util.zip.ZipFile

/**
 * Custom Vulkan drivers (adrenotools packages: a ZIP with meta.json and the driver library, such as
 * Turnip's releases). Packages are kept in driverStorageDir; the one in use is unpacked into
 * driverInstallDir, which the native core loads at game start (NativeCoreActivity's vkdriver), with
 * the name of its package beside it (PACKAGE_MARKER). An empty driverInstallDir: the system driver.
 */
object GpuDriverHelper {
  private const val TAG = "GpuDriverHelper"
  private const val META_JSON = "meta.json"
  /** In driverInstallDir: the file name of the package the driver came from. */
  private const val PACKAGE_MARKER = ".hakux_package"

  private lateinit var appContext: Context

  val driverInstallDir: String get() = appContext.filesDir.absolutePath + "/gpu_driver/"
  val driverStorageDir: String get() = appContext.getExternalFilesDir(null)!!.absolutePath + "/gpu_drivers/"
  val hookLibDir: String get() = appContext.applicationInfo.nativeLibraryDir + "/"

  fun init(context: Context) {
    appContext = context.applicationContext
    File(driverInstallDir).mkdirs()
    File(driverStorageDir).mkdirs()
  }

  fun supportsCustomDriverLoading(): Boolean {
    return File("/dev/kgsl-3d0").exists() || File("/dev/mali0").exists()
  }

  /**
   * A Turnip driver is worth recommending: the phone can load one (Qualcomm), it runs on its own
   * driver, and that is older than Vulkan 1.3 (games then run on the core's compatibility path,
   * vk/compat.c).
   */
  fun turnipRecommended(context: Context): Boolean {
    init(context)
    return supportsCustomDriverLoading() && getActiveDriver() == null &&
      !context.packageManager.hasSystemFeature(PackageManager.FEATURE_VULKAN_HARDWARE_VERSION, VULKAN_1_3)
  }

  /** VK_API_VERSION_1_3, as FEATURE_VULKAN_HARDWARE_VERSION's versions are */
  private const val VULKAN_1_3 = 0x00403000

  fun initializeDriver(customDriverName: String? = null) {
    nativeInitializeDriver(hookLibDir, driverInstallDir, customDriverName)
  }

  /** Why a package wasn't added (addDriver). */
  enum class AddError { UNREADABLE, NOT_A_DRIVER, NO_LIBRARY, API, UNPACK }

  /** addDriver's outcome: the package added (and in use), or why not. */
  data class AddResult(val driver: DriverMetadata? = null, val error: AddError? = null)

  /**
   * The package at uri copied into the store (under its own file name: a package of the same name is
   * replaced, an update of it) and put in use. Checked first: a meta.json, the library it names, the
   * Android version it needs.
   */
  fun addDriver(context: Context, uri: Uri): AddResult {
    init(context)
    val tmpFile = File(driverStorageDir, "driver_tmp.zip")
    try {
      context.contentResolver.openInputStream(uri)?.use { input ->
        FileOutputStream(tmpFile).use { output -> input.copyTo(output) }
      } ?: return AddResult(error = AddError.UNREADABLE)
    } catch (e: IOException) {
      Log.e(TAG, "Failed to copy driver URI", e)
      tmpFile.delete()
      return AddResult(error = AddError.UNREADABLE)
    }
    val meta = readMetadata(tmpFile)
    val error = when {
      meta == null -> AddError.NOT_A_DRIVER
      meta.libraryName.isNullOrEmpty() || !hasEntry(tmpFile, meta.libraryName) -> AddError.NO_LIBRARY
      meta.minApi > Build.VERSION.SDK_INT -> AddError.API
      else -> null
    }
    if (meta == null || error != null) {
      Log.e(TAG, "Not a usable driver package: $error")
      tmpFile.delete()
      return AddResult(meta, error)
    }
    val stored = File(driverStorageDir, packageFileName(context, uri, meta))
    stored.delete()
    if (!tmpFile.renameTo(stored)) {
      tmpFile.delete()
      return AddResult(meta, AddError.UNREADABLE)
    }
    val added = meta.copy(path = stored.absolutePath)
    return if (useDriver(stored)) AddResult(added) else AddResult(added, AddError.UNPACK)
  }

  /** A stored package's file name: the document's own (safe characters only), else the driver's name. */
  private fun packageFileName(context: Context, uri: Uri, meta: DriverMetadata): String {
    val shown = runCatching {
      context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
        if (it.moveToFirst()) it.getString(0) else null
      }
    }.getOrNull()
    val base = (shown ?: meta.name ?: "driver").removeSuffix(".zip").replace(Regex("[^A-Za-z0-9._-]+"), "_")
    return base.ifEmpty { "driver" } + ".zip"
  }

  private fun hasEntry(zipFile: File, name: String): Boolean =
    runCatching { ZipFile(zipFile).use { it.getEntry(name) != null } }.getOrDefault(false)

  /**
   * The stored package put in use: unpacked beside the one in use, which it replaces only once it is
   * whole (a package that fails to unpack leaves the driver in use as it was).
   */
  fun useDriver(driverZip: File): Boolean {
    val installDir = File(driverInstallDir)
    val newDir = File(installDir.parentFile, installDir.name + ".new")
    newDir.deleteRecursively()
    newDir.mkdirs()
    val root = newDir.canonicalPath + File.separator
    try {
      ZipFile(driverZip).use { zip ->
        zip.entries().asSequence().forEach { entry ->
          val outFile = File(newDir, entry.name)
          if (!outFile.canonicalPath.startsWith(root)) {
            throw IOException("entry outside the package: ${entry.name}")
          }
          if (entry.isDirectory) {
            outFile.mkdirs()
          } else {
            outFile.parentFile?.mkdirs()
            zip.getInputStream(entry).use { input ->
              FileOutputStream(outFile).use { output -> input.copyTo(output) }
            }
          }
        }
      }
      File(newDir, PACKAGE_MARKER).writeText(driverZip.name)
    } catch (e: Exception) {
      Log.e(TAG, "Failed to extract driver", e)
      newDir.deleteRecursively()
      return false
    }
    installDir.deleteRecursively()
    if (!newDir.renameTo(installDir)) {
      Log.e(TAG, "Failed to move the driver into place")
      newDir.deleteRecursively()
      installDir.mkdirs()
      return false
    }
    return true
  }

  /** The system driver in use: driverInstallDir emptied. */
  fun useSystemDriver() {
    File(driverInstallDir).deleteRecursively()
    File(driverInstallDir).mkdirs()
  }

  /** A stored package deleted; the one in use too: then the system driver is. True if it was in use. */
  fun deleteDriver(driverZip: File): Boolean {
    val inUse = isInUse(driverZip)
    driverZip.delete()
    if (inUse) {
      useSystemDriver()
    }
    return inUse
  }

  /** The driver in use (its meta.json), or null: the system driver. */
  fun getActiveDriver(): DriverMetadata? {
    val metaFile = File(driverInstallDir, META_JSON)
    if (!metaFile.exists()) return null
    return try {
      parseMetadata(JSONObject(metaFile.readText()), null)
    } catch (e: Exception) {
      null
    }
  }

  fun getInstalledDriverName(): String? = getActiveDriver()?.name

  fun getInstalledDriverLibrary(): String? = getActiveDriver()?.libraryName

  /**
   * The stored package is the driver in use: by the package name the install keeps, else (a driver
   * put in use before it did, or brought over from the previous release) by the driver's name.
   */
  fun isInUse(driverZip: File): Boolean {
    val marker = File(driverInstallDir, PACKAGE_MARKER)
    if (marker.exists()) {
      return runCatching { marker.readText() }.getOrNull() == driverZip.name
    }
    val active = getActiveDriver()?.name ?: return false
    return readMetadata(driverZip)?.name == active
  }

  fun getAvailableDrivers(): List<DriverMetadata> {
    val dir = File(driverStorageDir)
    if (!dir.exists()) return emptyList()
    return dir.listFiles()
      ?.filter { it.extension == "zip" && it.name != "driver_tmp.zip" }
      ?.mapNotNull { readMetadata(it)?.copy(path = it.absolutePath) }
      ?.sortedBy { it.name?.lowercase() }
      ?: emptyList()
  }

  /** A package's meta.json (the first .json if none is called that), or null. */
  fun readMetadata(zipFile: File): DriverMetadata? {
    if (!zipFile.exists()) return null
    try {
      ZipFile(zipFile).use { zip ->
        val jsons = zip.entries().asSequence().filter { !it.isDirectory && it.name.lowercase().endsWith(".json") }.toList()
        val entry = jsons.firstOrNull { File(it.name).name.equals(META_JSON, ignoreCase = true) } ?: jsons.firstOrNull()
          ?: return null
        zip.getInputStream(entry).use { input ->
          return parseMetadata(JSONObject(input.bufferedReader().readText()), zipFile.absolutePath)
        }
      }
    } catch (e: Exception) {
      Log.e(TAG, "Failed to read driver metadata from ${zipFile.name}", e)
    }
    return null
  }

  private fun parseMetadata(json: JSONObject, path: String?) = DriverMetadata(
    name = json.optString("name").ifEmpty { null },
    description = json.optString("description").ifEmpty { null },
    author = json.optString("author").ifEmpty { null },
    vendor = json.optString("vendor").ifEmpty { null },
    driverVersion = json.optString("driverVersion").ifEmpty { null },
    packageVersion = json.optString("packageVersion").ifEmpty { null },
    libraryName = json.optString("libraryName").ifEmpty { null },
    minApi = json.optInt("minApi", 0),
    path = path,
  )

  private external fun nativeInitializeDriver(
    hookLibDir: String?,
    customDriverDir: String?,
    customDriverName: String?
  )

  data class DriverMetadata(
    val name: String? = null,
    val description: String? = null,
    val author: String? = null,
    val vendor: String? = null,
    val driverVersion: String? = null,
    val packageVersion: String? = null,
    val libraryName: String? = null,
    val minApi: Int = 0,
    val path: String? = null
  )
}
