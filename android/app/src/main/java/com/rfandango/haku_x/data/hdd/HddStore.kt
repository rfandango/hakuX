package com.rfandango.haku_x.data.hdd

import android.content.Context
import android.graphics.Bitmap
import com.rfandango.haku_x.data.DataRoot
import com.rfandango.haku_x.data.XprImage
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * What the native core keeps of the Xbox's hard disk (files/native/hdd, see
 * XboxHdd) and beside it, for the hard disk screen: the game saves (E:\UDATA:
 * a folder per title with TitleMeta.xbx and TitleImage.xbx, a folder per save
 * with SaveMeta.xbx and SaveImage.xbx), the titles' data (E:\TDATA), the
 * cache drives (X, Y, Z) and the savestates.
 */
class HddStore(context: Context) {
  val native = DataRoot.native(context)
  val root = File(native, "hdd")
  private val states = File(native, "states")

  class Save(val dir: File, val name: String, val size: Long, val modified: Long) {
    val icon get() = File(dir, "SaveImage.xbx").takeIf { it.isFile }
  }

  class Title(val id: String, val dir: File, val name: String?, val size: Long, val saves: List<Save>) {
    val icon get() = File(dir, "TitleImage.xbx").takeIf { it.isFile }
  }

  class Cache(val part: Int, val drive: String, val size: Long, val files: Int, val formatted: Boolean)

  class States(val dir: File, val key: String, val slots: Int, val size: Long)

  class Usage(val saves: Long, val titleData: Long, val system: Long, val caches: Long, val states: Long)

  /** A folder's name in any case, as the core matches names (io.c host_path). */
  private fun child(dir: File, name: String) =
    dir.listFiles()?.firstOrNull { it.name.equals(name, ignoreCase = true) }

  /** E:\UDATA's titles with their saves, the largest first. */
  fun saves(): List<Title> {
    val udata = child(XboxHdd.folder(root, 1), "UDATA") ?: return emptyList()
    return (udata.listFiles() ?: emptyArray()).filter { it.isDirectory }.map { t ->
      val saves = (t.listFiles() ?: emptyArray()).filter { it.isDirectory }.map { s ->
        Save(s, meta(child(s, "SaveMeta.xbx"), "Name") ?: s.name, sizeOf(s), s.lastModified())
      }.sortedByDescending { it.modified }
      Title(t.name.uppercase(), t, meta(child(t, "TitleMeta.xbx"), "TitleName"), sizeOf(t), saves)
    }.sortedByDescending { it.size }
  }

  /** E:\TDATA's titles (settings, downloaded content), the largest first. The kernel makes each title's T:
   *  folder at launch, so the ones with no files aren't listed. */
  fun titleData(): List<Title> {
    val tdata = child(XboxHdd.folder(root, 1), "TDATA") ?: return emptyList()
    val names = saves().associate { it.id to it.name }
    return (tdata.listFiles() ?: emptyArray()).filter { it.isDirectory && it.walkTopDown().any { f -> f.isFile } }.map { t ->
      Title(t.name.uppercase(), t, meta(child(t, "TitleMeta.xbx"), "TitleName") ?: names[t.name.uppercase()],
            sizeOf(t), emptyList())
    }.sortedByDescending { it.size }
  }

  fun caches(): List<Cache> = listOf(3 to "X", 4 to "Y", 5 to "Z").map { (p, d) ->
    val dir = XboxHdd.folder(root, p)
    val files = dir.walkTopDown().filter { it.isFile }.toList()
    Cache(p, d, files.sumOf { it.length() }, files.size,
          XboxHdd.rawHead(root, p).isFile || files.isNotEmpty())
  }

  /** A cache drive emptied and unformatted: the next game that wants it formats it. */
  fun clearCache(part: Int) {
    require(part in 3..5)
    XboxHdd.folder(root, part).let { it.deleteRecursively(); it.mkdirs() }
    XboxHdd.rawHead(root, part).delete()
  }

  fun delete(dir: File) {
    require(dir.canonicalPath.startsWith(native.canonicalPath + File.separator)) { "outside the native core's storage" }
    dir.deleteRecursively()
  }

  /** The savestates kept per game (SaveStateStore's folders). */
  fun states(): List<States> = (states.listFiles() ?: emptyArray()).filter { it.isDirectory }.map {
    States(it, it.name, it.listFiles()?.count { f -> f.name.endsWith(".hxs") } ?: 0, sizeOf(it))
  }.filter { it.slots > 0 || it.size > 0 }.sortedByDescending { it.size }

  /** Copies of the disk earlier test builds left beside the one in use (hdd-*): nothing reads them. */
  fun deleteLeftovers() {
    native.listFiles()?.filter { it.isDirectory && it.name.startsWith("hdd-") }?.forEach { it.deleteRecursively() }
  }

  fun usage(): Usage {
    val sv = child(XboxHdd.folder(root, 1), "UDATA")?.let(::sizeOf) ?: 0L
    val td = child(XboxHdd.folder(root, 1), "TDATA")?.let(::sizeOf) ?: 0L
    val e = sizeOf(XboxHdd.folder(root, 1))
    return Usage(
      saves = sv, titleData = td,
      system = e - sv - td + sizeOf(XboxHdd.folder(root, 2)),
      caches = (3..5).sumOf { sizeOf(XboxHdd.folder(root, it)) },
      states = sizeOf(states),
    )
  }

  companion object {
    fun sizeOf(f: File): Long = if (f.isFile) f.length() else f.walkTopDown().filter { it.isFile }.sumOf { it.length() }

    /** A key's value in an .xbx metadata file (UTF-16 "Key=Value" lines). */
    fun meta(f: File?, key: String): String? {
      if (f == null || !f.isFile || f.length() > 65536) return null
      return meta(f.readBytes(), key)
    }

    fun meta(b: ByteArray, key: String): String? {
      val text = if (b.size >= 2 && b[0] == 0xFF.toByte() && b[1] == 0xFE.toByte()) String(b, 2, b.size - 2, Charsets.UTF_16LE)
      else String(b, Charsets.UTF_16LE)
      return text.lineSequence().map { it.trim('\uFEFF', ' ', '\r', '\u0000') }
        .firstOrNull { it.startsWith("$key=", ignoreCase = true) }
        ?.substringAfter('=')?.trim()?.takeIf { it.isNotEmpty() }
    }

    /** An .xbx image (XPR0) decoded, or null. */
    fun icon(f: File?): Bitmap? {
      if (f == null || !f.isFile || f.length() > (1 shl 20)) return null
      return runCatching { XprImage.decode(ByteBuffer.wrap(f.readBytes()).order(ByteOrder.LITTLE_ENDIAN)) }.getOrNull()
    }
  }
}
