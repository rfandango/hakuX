package com.rfandango.haku_x.data

import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.DocumentsContract
import com.rfandango.haku_x.data.hdd.XboxHdd
import java.io.File
import java.io.IOException

/**
 * Where the core's data lives: the folder holding hdd (the Xbox's hard disk), states (the savestates),
 * hle (the shader caches, the core's <hdd>/../hle) and mods.txt. The app's own storage (files/native)
 * unless the player picked another: the app's folder on an SD card, or any folder of the phone's
 * storage (all files access, as the core opens its files by path, not through a document tree).
 *
 * The choice is a file (files/data_root) both processes read afresh, as Controllers: a game's process
 * must see a change made in the launcher's. A folder picked that can't be reached now (all files access
 * taken back, its SD card out) stays the choice: the launcher asks before a game runs anywhere else
 * (LauncherActivity), as saves made elsewhere wouldn't be where the player's are.
 */
object DataRoot {
  private const val FILE = "data_root"
  /** A move's copy, renamed into place once whole. */
  private const val MOVING = ".hakux-moving"
  /** What of a data folder is hakuX's: all a move copies and deletes. A picked folder may hold the player's own files too. */
  private val OWNED = listOf("hdd", "states", "hle", "mods.txt", ".nomedia")

  /** The app's own storage: where the data is unless another folder was picked. */
  fun internal(context: Context) = File(context.filesDir, "native")

  /** The folder picked, usable or not; null: the app's own storage. */
  fun custom(context: Context): File? =
    runCatching { File(context.filesDir, FILE).readText().trim() }.getOrNull()
      ?.takeIf { it.isNotEmpty() }?.let(::File)

  /** The core's data folder: the one picked, reachable or not (unavailable says), else the app's own storage. */
  fun native(context: Context): File = custom(context) ?: internal(context)

  /** A folder was picked but can't be used now: all files access taken back, or its SD card out. */
  fun unavailable(context: Context): Boolean = custom(context)?.let { !usable(context, it) } ?: false

  /** Back to the app's own storage, the folder picked left as it is (the player's choice when it can't be reached). */
  fun useInternal(context: Context) = save(context, null)

  /** The app's folder on an SD card (no permission needed), null without one. */
  fun sdCard(context: Context): File? =
    context.getExternalFilesDirs(null).drop(1).firstOrNull { it != null }?.let { File(it, "native") }

  /** On the app's own folders of the phone's storage, where no permission is needed. */
  private fun appOwned(context: Context, dir: File): Boolean {
    val path = dir.absolutePath + File.separator
    return context.getExternalFilesDirs(null).filterNotNull().any { path.startsWith(it.absolutePath + File.separator) }
  }

  /** Any folder of the phone's storage can be opened by path (Android 11 on: all files access). */
  fun allFilesAccess(context: Context): Boolean =
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) Environment.isExternalStorageManager()
    else context.checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE) ==
      android.content.pm.PackageManager.PERMISSION_GRANTED

  private fun usable(context: Context, dir: File): Boolean =
    (appOwned(context, dir) || allFilesAccess(context)) && dir.isDirectory && dir.canWrite()

  /**
   * A picked document tree's folder as a path: "primary:Games/hakuX" is /storage/emulated/0/Games/hakuX,
   * "1234-5678:hakuX" the SD card's /storage/1234-5678/hakuX. Null for a tree that isn't a folder of
   * the phone's storage (a cloud drive's), or a storage's top: the data goes in a folder of its own.
   */
  fun pathOf(tree: Uri): File? {
    if (tree.authority != "com.android.externalstorage.documents") return null
    val id = runCatching { DocumentsContract.getTreeDocumentId(tree) }.getOrNull() ?: return null
    val (volume, rel) = id.split(":", limit = 2).takeIf { it.size == 2 } ?: return null
    if (rel.trim('/').isEmpty()) return null
    val base = if (volume == "primary") Environment.getExternalStorageDirectory() else File("/storage/$volume")
    return File(base, rel)
  }

  /**
   * dir can take the core's files: one written and deleted (the core writes with fopen, not the Java
   * API). A new name each time: a fixed one left behind, or two checks at once, made a writable
   * folder look read-only (ARMSX2 validateSystemDirWritable).
   */
  fun writable(dir: File): Boolean = runCatching {
    (dir.isDirectory || dir.mkdirs()) && File.createTempFile(".hakux-write-probe-", ".tmp", dir).delete()
  }.getOrDefault(false)

  /** dir holds a hakuX hard disk or savestates already: switching to it uses that data instead of copying over it. */
  fun hasData(dir: File) = File(dir, "hdd").isDirectory || File(dir, "states").isDirectory

  /** One of a and b is in the other (or they're the same): a move between them would copy a folder into itself. */
  fun nested(a: File, b: File): Boolean {
    val x = a.canonicalPath + File.separator
    val y = b.canonicalPath + File.separator
    return x.startsWith(y) || y.startsWith(x)
  }

  /**
   * The folders the core makes itself with mkdir (app_main.c make_hdd), made here first through the
   * Java API; and .nomedia, so the savestates' pictures stay out of the phone's gallery.
   */
  fun prepare(native: File) {
    val hdd = File(native, "hdd")
    for (i in 0..5) File(hdd, "Partition$i").mkdirs()
    runCatching { File(native, ".nomedia").createNewFile() }
  }

  /**
   * The data moved to target (null: the app's own storage), and the choice saved. hakuX's part of the
   * folder (OWNED) is copied beside target's (MOVING), checked, put in place, and only then deleted
   * where it was. A target with a hard disk of its own (hasData) is used as it is, and a data folder
   * that can't be reached now is left as it is: then nothing is copied or deleted.
   */
  fun move(context: Context, target: File?, progress: XboxHdd.Progress) {
    val from = native(context)
    val to = target ?: internal(context)
    if (from.canonicalPath == to.canonicalPath) {
      save(context, target)
      return
    }
    if (nested(from, to)) throw IOException("$to and $from are one inside the other")
    if (!writable(to)) throw IOException("can't write to $to")
    if (hasData(to) || unavailable(context) || !from.isDirectory) {
      save(context, target)
      prepare(to)
      return
    }
    val tmp = File(to, MOVING)
    tmp.deleteRecursively()
    val placed = ArrayList<File>()
    try {
      val entries = OWNED.map { File(from, it) }.filter { it.exists() }
      // empty folders too: the core made them (TDATA\<title>) and a game may count on them
      for (e in entries) e.walkTopDown().filter { it.isDirectory }.forEach { File(tmp, it.relativeTo(from).path).mkdirs() }
      val files = entries.flatMap { e -> e.walkTopDown().filter { it.isFile }.toList() }
      val total = files.sumOf { it.length() }
      var done = 0L
      var said = 0L
      val buf = ByteArray(1 shl 16)
      for (f in files) {
        val rel = f.relativeTo(from).path
        val dst = File(tmp, rel)
        dst.parentFile!!.mkdirs()
        f.inputStream().use { i ->
          dst.outputStream().use { o ->
            while (true) {
              val n = i.read(buf)
              if (n < 0) break
              o.write(buf, 0, n)
              done += n
              // said at most every 4 MB: each one redraws the progress
              if (done - said >= (4 shl 20)) {
                said = done
                progress.update(done, total, rel)
              }
            }
          }
        }
        progress.update(done, total, rel)
        dst.setLastModified(f.lastModified())
      }
      for (f in files) {
        if (File(tmp, f.relativeTo(from).path).length() != f.length()) throw IOException("${f.relativeTo(from)} wasn't copied whole")
      }
      for (e in entries) {
        val dst = File(to, e.name)
        dst.deleteRecursively()       /* an older hakuX folder's shader caches or mods.txt (hasData: no hard disk, no states) */
        if (!File(tmp, e.name).renameTo(dst)) throw IOException("can't put ${e.name} in place in $to")
        placed += dst
      }
    } catch (t: Throwable) {
      // nothing half-copied left to look like a hard disk of its own: the data is still all where it was
      tmp.deleteRecursively()
      for (p in placed) p.deleteRecursively()
      throw t
    }
    tmp.deleteRecursively()
    save(context, target)
    prepare(to)
    // the old copy goes only once the new one is in use, and only hakuX's part of the folder
    for (name in OWNED) File(from, name).deleteRecursively()
  }

  private fun save(context: Context, target: File?) {
    val f = File(context.filesDir, FILE)
    if (target == null) f.delete() else f.writeText(target.absolutePath)
  }
}
