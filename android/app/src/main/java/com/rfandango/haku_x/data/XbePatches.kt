package com.rfandango.haku_x.data

/** One section of default.xbe as the disc has it: where it loads, where its bytes are in the file, and them. */
class XbeSection(val va: Long, val raw: Long, val data: ByteArray)

/**
 * default.xbe as the disc has it: the headers' part of the file (the core reads it into the header
 * page, at base) and each section's bytes. It turns the file positions and the byte searches of a
 * mod's "offset" and "search" lines (mods/README.md) into the addresses the core patches at.
 */
class XbeImage(val base: Long, val headerEnd: Long, sections: List<XbeSection>) {
  private val sections = sections.sortedBy { it.raw }

  /** Where the file's byte at `offset` is in memory, and how many bytes of its part (the headers, or a section) lie from there on; null: it is in no part. */
  private fun locate(offset: Long): Pair<Long, Long>? {
    if (offset < headerEnd) return (base + offset) to (headerEnd - offset)
    for (s in sections) {
      if (offset >= s.raw && offset < s.raw + s.data.size) return (s.va + (offset - s.raw)) to (s.raw + s.data.size - offset)
    }
    return null
  }

  /**
   * `bytes` written at `offset` of the file, as writes at addresses: one for each part of the file they
   * cover (a patch of the core lies in the headers or in one section). Null when a byte of them is in
   * no part: a patch made for another build of the game.
   */
  fun addressRuns(offset: Long, bytes: ByteArray): List<Pair<Long, ByteArray>>? {
    val out = ArrayList<Pair<Long, ByteArray>>()
    var i = 0
    while (i < bytes.size) {
      val (va, room) = locate(offset + i) ?: return null
      val n = minOf(room, (bytes.size - i).toLong()).toInt()
      out.add(va to bytes.copyOfRange(i, i + n))
      i += n
    }
    return out
  }

  /** The address of the first place (in file order) that `find` occurs in the sections' bytes, or null. */
  fun find(find: ByteArray): Long? {
    if (find.isEmpty()) return null
    for (s in sections) {
      val at = indexOf(s.data, find)
      if (at >= 0) return s.va + at
    }
    return null
  }

  private fun indexOf(data: ByteArray, find: ByteArray): Int {
    val first = find[0]
    val last = data.size - find.size
    var i = 0
    while (i <= last) {
      if (data[i] == first) {
        var k = 1
        while (k < find.size && data[i + k] == find[k]) k++
        if (k == find.size) return i
      }
      i++
    }
    return -1
  }
}

/**
 * A line of a mod's [default.xbe] section that says where to patch by the file, not by an address
 * (mods/README.md), as the .JMP patches of Jay's Magic Patches do:
 *
 *   offset 0x713B: D0 02 00 00     writes the bytes at that position of the XBE file
 *   search 80 02 -> 00 05          writes the second bytes over the first place the first ones occur
 *
 * The core only knows patches at an address (0xADDRESS: ...), so ModRun turns each into some.
 */
sealed class XbePatch {
  class Offset(val offset: Long, val bytes: ByteArray) : XbePatch()
  class Search(val find: ByteArray, val repl: ByteArray) : XbePatch()

  /**
   * The core's patch lines for this patch on this XBE; none when it doesn't fit it: an offset the
   * file doesn't have, bytes that are not there.
   */
  fun resolve(image: XbeImage): List<String> = when (this) {
    is Offset -> image.addressRuns(offset, bytes).orEmpty().flatMap { (va, run) -> writes(va, run) }
    is Search -> image.find(find)?.let { changes(it, find, repl) }.orEmpty()
  }

  companion object {
    private const val MAX_BYTES = 4096

    /**
     * The core reads a line of its manifest in 1023 characters, and a patch of 256 bytes at most:
     * a run of bytes is cut here (the line is about 100 characters and 6 for a byte).
     */
    private const val MAX_RUN = 64

    private val OFFSET = Regex("""offset\s+(?:0x)?([0-9a-f]{1,8})\s*:\s*([0-9a-f\s]+)""", RegexOption.IGNORE_CASE)
    private val SEARCH = Regex("""search\s+([0-9a-f\s]+?)\s*->\s*([0-9a-f\s]+)""", RegexOption.IGNORE_CASE)

    /** The patch a line says, or null when it isn't an offset or search line, or is not well made. */
    fun parse(line: String): XbePatch? {
      val t = line.substringBefore('#').trim()
      OFFSET.matchEntire(t)?.let { m ->
        val bytes = bytes(m.groupValues[2]) ?: return null
        return Offset(m.groupValues[1].toLong(16), bytes)
      }
      SEARCH.matchEntire(t)?.let { m ->
        val find = bytes(m.groupValues[1]) ?: return null
        val repl = bytes(m.groupValues[2]) ?: return null
        return if (find.size == repl.size) Search(find, repl) else null
      }
      return null
    }

    /** Hex pairs, spaces anywhere: the bytes, or null when there are none, a half byte, or too many. */
    private fun bytes(text: String): ByteArray? {
      val s = text.filter { !it.isWhitespace() }
      if (s.isEmpty() || s.length % 2 != 0 || s.length / 2 > MAX_BYTES) return null
      return ByteArray(s.length / 2) { ((Character.digit(s[it * 2], 16) shl 4) or Character.digit(s[it * 2 + 1], 16)).toByte() }
    }

    /** Bytes as the patch lines write them: "DB 44 24 68". */
    fun hex(bytes: ByteArray): String = bytes.joinToString(" ") { "%02X".format(it.toInt() and 0xFF) }

    /** Patch lines for `bytes` at an address, MAX_RUN of them to a line. */
    private fun writes(va: Long, bytes: ByteArray): List<String> =
      (bytes.indices step MAX_RUN).map { i ->
        "0x%08X: %s".format(va + i, hex(bytes.copyOfRange(i, minOf(i + MAX_RUN, bytes.size))))
      }

    /** A patch line each for the runs of bytes that `neu` changes of `old` at an address: the core checks the old ones. */
    private fun changes(va: Long, old: ByteArray, neu: ByteArray): List<String> {
      val lines = ArrayList<String>()
      var i = 0
      while (i < old.size) {
        if (old[i] == neu[i]) {
          i++
          continue
        }
        var end = i
        while (end < old.size && old[end] != neu[end] && end - i < MAX_RUN) end++
        lines.add("0x%08X: %s -> %s".format(va + i, hex(old.copyOfRange(i, end)), hex(neu.copyOfRange(i, end))))
        i = end
      }
      return lines
    }
  }
}
