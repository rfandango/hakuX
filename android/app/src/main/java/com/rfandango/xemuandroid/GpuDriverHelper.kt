package com.rfandango.xemuandroid

import android.content.Context
import android.net.Uri
import android.os.Build
import android.util.Log
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.util.zip.ZipFile

object GpuDriverHelper {
  private const val TAG = "GpuDriverHelper"
  private const val META_JSON = "meta.json"

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
    return File("/dev/kgsl-3d0").exists()
  }

  fun initializeDriver(customDriverName: String? = null) {
    nativeInitializeDriver(hookLibDir, driverInstallDir, customDriverName)
  }

  fun installDriverFromUri(context: Context, uri: Uri): Boolean {
    init(context)

    val tmpFile = File(driverStorageDir, "driver_tmp.zip")
    try {
      context.contentResolver.openInputStream(uri)?.use { input ->
        FileOutputStream(tmpFile).use { output ->
          input.copyTo(output)
        }
      } ?: return false
    } catch (e: IOException) {
      Log.e(TAG, "Failed to copy driver URI", e)
      tmpFile.delete()
      return false
    }

    val metadata = readMetadata(tmpFile)
    if (metadata == null) {
      Log.e(TAG, "Invalid driver ZIP: no meta.json found")
      tmpFile.delete()
      return false
    }

    if (metadata.minApi > Build.VERSION.SDK_INT) {
      Log.e(TAG, "Driver requires API ${metadata.minApi}, device is ${Build.VERSION.SDK_INT}")
      tmpFile.delete()
      return false
    }

    val namedFile = File(driverStorageDir, metadata.name?.replace(" ", "_") + ".zip")
    tmpFile.renameTo(namedFile)

    return installDriver(namedFile)
  }

  fun installDriver(driverZip: File): Boolean {
    val installDir = File(driverInstallDir)
    installDir.deleteRecursively()
    installDir.mkdirs()

    try {
      ZipFile(driverZip).use { zip ->
        zip.entries().asSequence().forEach { entry ->
          if (entry.isDirectory) {
            File(installDir, entry.name).mkdirs()
          } else {
            val outFile = File(installDir, entry.name)
            outFile.parentFile?.mkdirs()
            zip.getInputStream(entry).use { input ->
              FileOutputStream(outFile).use { output ->
                input.copyTo(output)
              }
            }
          }
        }
      }
    } catch (e: Exception) {
      Log.e(TAG, "Failed to extract driver", e)
      return false
    }

    return true
  }

  fun installDefaultDriver() {
    File(driverInstallDir).deleteRecursively()
    File(driverInstallDir).mkdirs()
  }

  fun getInstalledDriverName(): String? {
    val metaFile = File(driverInstallDir, META_JSON)
    if (!metaFile.exists()) return null
    return try {
      val json = JSONObject(metaFile.readText())
      json.optString("name", null)
    } catch (e: Exception) {
      null
    }
  }

  fun getInstalledDriverLibrary(): String? {
    val metaFile = File(driverInstallDir, META_JSON)
    if (!metaFile.exists()) return null
    return try {
      val json = JSONObject(metaFile.readText())
      json.optString("libraryName", null)
    } catch (e: Exception) {
      null
    }
  }

  fun getAvailableDrivers(): List<DriverMetadata> {
    val dir = File(driverStorageDir)
    if (!dir.exists()) return emptyList()
    return dir.listFiles()
      ?.filter { it.extension == "zip" }
      ?.mapNotNull { readMetadata(it)?.copy(path = it.absolutePath) }
      ?.sortedBy { it.name }
      ?: emptyList()
  }

  fun readMetadata(zipFile: File): DriverMetadata? {
    if (!zipFile.exists()) return null
    try {
      ZipFile(zipFile).use { zip ->
        val entries = zip.entries()
        while (entries.hasMoreElements()) {
          val entry = entries.nextElement()
          if (!entry.isDirectory && entry.name.lowercase().endsWith(".json")) {
            zip.getInputStream(entry).use { input ->
              val text = input.bufferedReader().readText()
              val json = JSONObject(text)
              return DriverMetadata(
                name = json.optString("name", null),
                description = json.optString("description", null),
                author = json.optString("author", null),
                libraryName = json.optString("libraryName", null),
                minApi = json.optInt("minApi", 0),
                path = zipFile.absolutePath
              )
            }
          }
        }
      }
    } catch (e: Exception) {
      Log.e(TAG, "Failed to read driver metadata from ${zipFile.name}", e)
    }
    return null
  }

  private external fun nativeInitializeDriver(
    hookLibDir: String?,
    customDriverDir: String?,
    customDriverName: String?
  )

  data class DriverMetadata(
    val name: String? = null,
    val description: String? = null,
    val author: String? = null,
    val libraryName: String? = null,
    val minApi: Int = 0,
    val path: String? = null
  )
}
