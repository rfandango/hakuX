package com.rfandango.haku_x.data

import android.content.Context
import android.graphics.Bitmap
import android.net.Uri
import java.io.FileInputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel
import java.util.Locale

/**
 * An Xbox disc image, read as far as the library needs: default.xbe's
 * certificate and the title image the dashboard shows. The game partition starts at
 * 0 in an XISO, or after the video partition in a full dump (the offsets
 * the core's xdvdfs.c probes).
 */
class XboxDisc private constructor(private val ch: FileChannel, private val partition: Long) {

  /** Where the game partition starts: 0 in an XISO, past the video partition in a full dump. */
  val partitionOffset: Long get() = partition

  /** default.xbe's first byte in the image, or null. */
  private val xbeOffset: Long? by lazy {
    // Volume descriptor at sector 32: magic, root directory sector and size.
    val vd = readAt(ch, partition + VOLUME + MAGIC.length, 8) ?: return@lazy null
    val rootSector = vd.int.toLong() and MASK
    val rootSize = vd.int.toLong() and MASK
    if (rootSize == 0L || rootSize > 256 * 1024) return@lazy null
    val dir = readAt(ch, partition + rootSector * SECTOR, rootSize.toInt()) ?: return@lazy null
    findEntry(dir, "default.xbe")?.let { partition + it * SECTOR }
  }

  /** default.xbe's headers (base address 0x104, size 0x108) and where it starts. */
  private class Xbe(val start: Long, val base: Long, val headers: ByteBuffer)

  private val xbe: Xbe? by lazy {
    val at = xbeOffset ?: return@lazy null
    val h = readAt(ch, at, 0x178) ?: return@lazy null
    if (h.getInt(0) != XBE_MAGIC) return@lazy null
    val size = h.getInt(0x108).toLong() and MASK
    if (size < 0x178 || size > MAX_READ) return@lazy null
    Xbe(at, h.getInt(0x104).toLong() and MASK, readAt(ch, at, size.toInt()) ?: return@lazy null)
  }

  /**
   * The certificate (its address at 0x118): title ID at +8, title name (40
   * UTF-16 characters) at +0x0C, game region flags (XBE_REGION_*) at +0xA0,
   * version at +0xAC.
   */
  private val cert: Int? by lazy { xbe?.let { headerOffset(it, it.headers.getInt(0x118), 0xB0) } }

  val titleId: Long
    get() = cert?.let { xbe!!.headers.getInt(it + 8).toLong() and MASK } ?: 0L

  /**
   * The certificate's version: it tells one title's releases apart, its
   * revisions and its discs (Dead or Alive Ultimate's two share 54430006:
   * versions 5 and 6).
   */
  val version: Long
    get() = cert?.let { xbe!!.headers.getInt(it + 0xAC).toLong() and MASK } ?: 0L

  val titleName: String?
    get() {
      val c = cert ?: return null
      val chars = CharArray(40) { xbe!!.headers.getChar(c + 0x0C + it * 2) }
      return String(chars).substringBefore('\u0000').trim().ifEmpty { null }
    }

  /** XBE_REGION_NA (1), _JAPAN (2), _RESTOFWORLD (4); the manufacturing bit dropped. */
  val regions: Int
    get() = cert?.let { xbe!!.headers.getInt(it + 0xA0) and 0x7 } ?: 0

  /**
   * The keys saves are signed with, by title ID (raw: the kernel derives each with the BIOS's
   * certificate key, SaveResigner.authKey): the title's own (certificate +0xC0), and per alternate
   * title ID (+0x5C, 16 of them) the key for that title's saves (+0xD0, 16 bytes each).
   */
  val saveKeys: Map<Long, ByteArray>
    get() {
      val x = xbe ?: return emptyMap()
      val c = headerOffset(x, x.headers.getInt(0x118), 0x1D0) ?: return emptyMap()
      val h = x.headers
      fun key(at: Int) = ByteArray(16) { h.get(at + it) }
      val out = LinkedHashMap<Long, ByteArray>()
      out[h.getInt(c + 8).toLong() and MASK] = key(c + 0xC0)
      for (i in 0 until 16) {
        val alt = h.getInt(c + 0x5C + i * 4).toLong() and MASK
        if (alt != 0L && alt !in out) out[alt] = key(c + 0xD0 + i * 16)
      }
      return out
    }

  /** The title image ($$XTIMAGE, else the save image $$XSIMAGE), or null. */
  fun titleImage(): Bitmap? =
    (section("\$\$XTIMAGE") ?: section("\$\$XSIMAGE"))?.let(XprImage::decode)

  /** A section's raw bytes by name: section headers at 0x120, 0x11C of them, 0x38 bytes each. */
  private fun section(name: String): ByteBuffer? {
    val x = xbe ?: return null
    val h = x.headers
    val count = h.getInt(0x11C)
    if (count !in 1..1024) return null
    val table = headerOffset(x, h.getInt(0x120), count * 0x38) ?: return null
    for (i in 0 until count) {
      val s = table + i * 0x38
      val nameAt = headerOffset(x, h.getInt(s + 0x14), 1) ?: continue
      if (cString(h, nameAt) != name) continue
      val raw = h.getInt(s + 0x0C).toLong() and MASK
      val size = h.getInt(s + 0x10).toLong() and MASK
      if (size == 0L || size > MAX_READ) return null
      return readAt(ch, x.start + raw, size.toInt())
    }
    return null
  }

  /**
   * default.xbe as the disc has it, for the mods' "offset" and "search" lines (XbePatch): the headers'
   * part of the file (as the core reads it, to the first section's data or sizeof_headers) and each
   * section's bytes with the address it loads at. Null when the XBE can't be read, or is over MAX_IMAGE.
   */
  fun xbeImage(): XbeImage? {
    val x = xbe ?: return null
    val h = x.headers
    val count = h.getInt(0x11C)
    if (count !in 1..1024) return null
    val table = headerOffset(x, h.getInt(0x120), count * 0x38) ?: return null
    val sections = ArrayList<XbeSection>()
    var headerEnd = 0x1000L
    var total = 0L
    for (i in 0 until count) {
      val s = table + i * 0x38
      val va = h.getInt(s + 0x04).toLong() and MASK
      val raw = h.getInt(s + 0x0C).toLong() and MASK
      val size = h.getInt(s + 0x10).toLong() and MASK
      headerEnd = minOf(headerEnd, raw)
      if (size == 0L) continue
      total += size
      if (total > MAX_IMAGE) return null
      sections.add(XbeSection(va, raw, readAt(ch, x.start + raw, size.toInt())?.array() ?: return null))
    }
    return XbeImage(x.base, maxOf(headerEnd, h.getInt(0x108).toLong() and MASK), sections)
  }

  /** A header address (a virtual address in the XBE) as an offset into the headers. */
  private fun headerOffset(x: Xbe, va: Int, len: Int): Int? {
    val off = (va.toLong() and MASK) - x.base
    return if (off >= 0 && off + len <= x.headers.capacity()) off.toInt() else null
  }

  private fun cString(b: ByteBuffer, at: Int): String {
    val sb = StringBuilder()
    var i = at
    while (i < b.capacity() && sb.length < 32) {
      val c = b.get(i++).toInt()
      if (c == 0) break
      sb.append(c.toChar())
    }
    return sb.toString()
  }

  companion object {
    private const val SECTOR = 2048L
    private const val VOLUME = 0x10000L
    private const val MAGIC = "MICROSOFT*XBOX*MEDIA"
    private const val XBE_MAGIC = 0x48454258 // "XBEH"
    private const val MASK = 0xFFFFFFFFL
    private const val MAX_READ = 4L shl 20
    private const val MAX_IMAGE = 48L shl 20
    private val PARTITIONS = longArrayOf(0L, 0x18300000L, 0xFD90000L, 0x2080000L)

    /** Opens the image at uri and runs use on it; null when it isn't an Xbox disc or can't be read. */
    fun <T> open(context: Context, uri: Uri, use: (XboxDisc) -> T): T? = try {
      context.contentResolver.openFileDescriptor(uri, "r")?.use { pfd ->
        FileInputStream(pfd.fileDescriptor).use { fis ->
          val ch = fis.channel
          partitionOf(ch)?.let { use(XboxDisc(ch, it)) }
        }
      }
    } catch (_: Exception) {
      null
    }

    /** The game partition's offset in an image (null: not an Xbox disc). */
    fun partitionOf(ch: FileChannel): Long? = PARTITIONS.firstOrNull { isVolume(ch, it) }

    private fun isVolume(ch: FileChannel, partition: Long): Boolean {
      val b = readAt(ch, partition + VOLUME, MAGIC.length) ?: return false
      return String(ByteArray(MAGIC.length).also { b.get(it) }, Charsets.US_ASCII) == MAGIC
    }

    private fun readAt(ch: FileChannel, pos: Long, n: Int): ByteBuffer? {
      if (n <= 0) return null
      val buf = ByteBuffer.allocate(n).order(ByteOrder.LITTLE_ENDIAN)
      var at = pos
      while (buf.hasRemaining()) {
        val got = ch.read(buf, at)
        if (got <= 0) return null
        at += got
      }
      buf.flip()
      return buf
    }

    /** The start sector of a root-directory entry (an XDVDFS binary tree). */
    private fun findEntry(dir: ByteBuffer, target: String): Long? {
      val wanted = target.lowercase(Locale.ROOT)
      val capacity = dir.capacity()

      fun search(offset: Int, depth: Int): Long? {
        if (depth > 64 || offset < 0 || offset + 14 > capacity) return null
        val left = dir.getShort(offset).toInt() and 0xFFFF
        val right = dir.getShort(offset + 2).toInt() and 0xFFFF
        val sector = dir.getInt(offset + 4).toLong() and MASK
        val nameLen = dir.get(offset + 13).toInt() and 0xFF
        if (offset + 14 + nameLen > capacity) return null
        val name = ByteArray(nameLen) { dir.get(offset + 14 + it) }
        if (String(name, Charsets.US_ASCII).lowercase(Locale.ROOT) == wanted) return sector
        if (left != 0 && left != 0xFFFF) search(left * 4, depth + 1)?.let { return it }
        if (right != 0 && right != 0xFFFF) search(right * 4, depth + 1)?.let { return it }
        return null
      }
      return search(0, 0)
    }
  }
}
