package com.rfandango.haku_x.data.hdd

import java.io.File
import java.io.IOException
import java.io.InputStream
import java.util.zip.ZipInputStream

/**
 * The Xbox dashboard on the hard disk's C: drive (Partition2): xboxdash.xbe and its folders
 * (xboxdashdata.<version>, xodash with Xbox Live's xonlinedash.xbe, fonts). The user's own files: hakuX
 * has none. The core boots it with no disc (NativeCoreActivity, URI) and when a title asks for one of
 * its pages (Xbox Live's account sign-up, native/kernel/ex.c); Insignia's accounts are made there.
 */
object Dashboard {
  /** NativeCoreActivity's disc for the dashboard: none, the core booting C:\xboxdash.xbe. */
  const val URI = "hakux:dashboard"
  const val XBE = "xboxdash.xbe"

  fun cDrive(root: File) = XboxHdd.folder(root, 2)

  fun installed(root: File) = cDrive(root).listFiles()?.any { it.isFile && it.name.equals(XBE, ignoreCase = true) } == true

  /**
   * The dashboard's files from a zip (open() reads it from the start, twice) into C:, over what's
   * there: the files beside its xboxdash.xbe and the folders under it, from the zip's root or a
   * folder inside. Names are matched in any case, as the core matches them (an XODash already there
   * takes xodash's files).
   */
  fun install(open: () -> InputStream, root: File, progress: XboxHdd.Progress): XboxHdd.Result {
    // where xboxdash.xbe is, and how much there is under it
    var prefix: String? = null
    val entries = ArrayList<Pair<String, Long>>()
    ZipInputStream(open()).use { zip ->
      generateSequence { zip.nextEntry }.forEach { e ->
        val name = e.name.replace('\\', '/')
        entries += name to e.size.coerceAtLeast(0)
        if (!e.isDirectory && name.substringAfterLast('/').equals(XBE, ignoreCase = true)) {
          val p = name.substring(0, name.length - XBE.length)
          if (prefix == null || p.length < prefix!!.length) prefix = p
        }
      }
    }
    val base = prefix ?: throw IOException("no $XBE in this zip: not the dashboard's files")
    val total = entries.filter { it.first.startsWith(base, ignoreCase = true) }.sumOf { it.second }.coerceAtLeast(1)
    val c = cDrive(root).apply { mkdirs() }
    var files = 0
    var bytes = 0L
    val skipped = ArrayList<String>()
    val buf = ByteArray(1 shl 16)
    ZipInputStream(open()).use { zip ->
      generateSequence { zip.nextEntry }.forEach { e ->
        val name = e.name.replace('\\', '/')
        if (!name.startsWith(base, ignoreCase = true)) return@forEach
        val parts = name.substring(base.length).split('/').filter { it.isNotEmpty() }
        if (parts.isEmpty()) return@forEach
        if (parts.any { it == "." || it == ".." }) {
          skipped += name
          return@forEach
        }
        if (e.isDirectory) {
          resolve(c, parts, dir = true)
          return@forEach
        }
        val out = resolve(c, parts, dir = false)
        progress.update(bytes, total, parts.joinToString("\\"))
        out.outputStream().use { o ->
          while (true) {
            val n = zip.read(buf)
            if (n < 0) break
            o.write(buf, 0, n)
            bytes += n
          }
        }
        if (e.time > 0) out.setLastModified(e.time)
        files++
      }
    }
    progress.update(total, total, "")
    return XboxHdd.Result(files, bytes, skipped)
  }

  /** The path parts under top, each an existing name in any case or made: the folders made, a file (dir false) not. */
  private fun resolve(top: File, parts: List<String>, dir: Boolean): File {
    var at = top
    parts.forEachIndexed { i, p ->
      val last = i == parts.size - 1
      val found = at.listFiles()?.firstOrNull { it.name.equals(p, ignoreCase = true) }
      at = found ?: File(at, p)
      if ((!last || dir) && !at.isDirectory) {
        if (at.exists()) at.delete()
        at.mkdirs()
      }
    }
    return at
  }
}
