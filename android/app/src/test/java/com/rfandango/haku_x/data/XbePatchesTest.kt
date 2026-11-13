package com.rfandango.haku_x.data

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class XbePatchesTest {
  private fun bytes(vararg b: Int) = ByteArray(b.size) { b[it].toByte() }

  /**
   * An XBE of 0x10000 as base, headers to 0x1000, .text of 0x20 bytes at file 0x1000 loading at
   * 0x11000, .data of 0x10 at file 0x1020 loading at 0x20000 (the sections' bytes follow each other
   * in the file, not their addresses).
   */
  private val image = XbeImage(
    base = 0x10000,
    headerEnd = 0x1000,
    sections = listOf(
      XbeSection(0x20000, 0x1020, ByteArray(0x10) { (0xA0 + it).toByte() }),
      XbeSection(0x11000, 0x1000, ByteArray(0x20) { it.toByte() }),
    ),
  )

  @Test
  fun parsesOffsetLines() {
    val p = XbePatch.parse("offset 0x713B: D0 02 00 00 68") as XbePatch.Offset
    assertEquals(0x713BL, p.offset)
    assertArrayEquals(bytes(0xD0, 0x02, 0x00, 0x00, 0x68), p.bytes)
    assertEquals(0x24AB08L, (XbePatch.parse("OFFSET 24ab08:d002") as XbePatch.Offset).offset)
    assertEquals(0x1L, (XbePatch.parse("offset 0x1: 90   # a comment") as XbePatch.Offset).offset)
  }

  @Test
  fun parsesSearchLines() {
    val p = XbePatch.parse("search DB44 85C9 -> DB44 9090") as XbePatch.Search
    assertArrayEquals(bytes(0xDB, 0x44, 0x85, 0xC9), p.find)
    assertArrayEquals(bytes(0xDB, 0x44, 0x90, 0x90), p.repl)
    assertNotNull(XbePatch.parse("search 8002->0005"))
  }

  @Test
  fun refusesLinesThatAreNotWellMade() {
    assertNull(XbePatch.parse("0x0011A2B4: 90 90"))          // an address line is the core's own
    assertNull(XbePatch.parse("find 8002 -> 0005"))           // the earlier find lines are gone
    assertNull(XbePatch.parse("search 8002 -> 000500"))       // not the same length
    assertNull(XbePatch.parse("search 80 0 -> 00 05"))        // half a byte
    assertNull(XbePatch.parse("search ?? 02 -> 00 05"))       // no wildcards
    assertNull(XbePatch.parse("offset 0x10:"))                // no bytes
    assertNull(XbePatch.parse("offset 0x123456789: 90"))      // more than 32 bits
    assertNull(XbePatch.parse("offset 0x10: " + "90".repeat(4097)))
  }

  @Test
  fun offsetInTheHeadersGoesToTheBaseAddress() {
    val runs = image.addressRuns(0x124, bytes(0x01))!!
    assertEquals(1, runs.size)
    assertEquals(0x10124L, runs[0].first)
  }

  @Test
  fun offsetInASectionGoesToItsAddress() {
    val runs = image.addressRuns(0x1004, bytes(1, 2, 3))!!
    assertEquals(listOf(0x11004L), runs.map { it.first })
    val data = image.addressRuns(0x1028, bytes(1))!!
    assertEquals(0x20008L, data[0].first)
  }

  @Test
  fun offsetAcrossTwoSectionsIsCutWhereTheirAddressesJump() {
    // the last 2 bytes of .text and the first 2 of .data are neighbours in the file, not in memory
    val runs = image.addressRuns(0x101E, bytes(1, 2, 3, 4))!!
    assertEquals(listOf(0x1101EL, 0x20000L), runs.map { it.first })
    assertEquals(listOf(2, 2), runs.map { it.second.size })
  }

  @Test
  fun offsetAcrossTheHeadersAndTheFirstSectionIsCutAtTheHeadersEnd() {
    val runs = image.addressRuns(0xFFF, bytes(1, 2))!!
    assertEquals(listOf(0x10FFFL, 0x11000L), runs.map { it.first })
  }

  @Test
  fun offsetOutsideTheFileIsRefusedWhole() {
    assertNull(image.addressRuns(0x1030, bytes(1)))           // past the last section
    assertNull(image.addressRuns(0x102F, bytes(1, 2)))        // half of it out
  }

  @Test
  fun searchFindsTheFirstPlaceInFileOrder() {
    // .text comes first in the file though .data loads higher
    assertEquals(0x11005L, image.find(bytes(5, 6, 7)))
    assertEquals(0x20003L, image.find(bytes(0xA3, 0xA4)))
    assertNull(image.find(bytes(0xFF, 0xFF)))
    assertNull(image.find(ByteArray(0)))
  }

  @Test
  fun searchLineWritesOnlyTheBytesItChanges() {
    val p = XbePatch.parse("search 05 06 07 08 -> 05 FF 07 EE")!!
    assertEquals(listOf("0x00011006: 06 -> FF", "0x00011008: 08 -> EE"), p.resolve(image))
  }

  @Test
  fun searchThatIsNotThereGivesNoLines() {
    assertTrue(XbePatch.parse("search FF FE -> 00 00")!!.resolve(image).isEmpty())
    assertTrue(XbePatch.parse("offset 0x5000: 90")!!.resolve(image).isEmpty())
  }

  @Test
  fun longRunsAreCutToShortManifestLines() {
    val big = XbeImage(0x10000, 0x1000, listOf(XbeSection(0x11000, 0x1000, ByteArray(4096) { 1 })))
    val offset = XbePatch.Offset(0x1000, ByteArray(300) { 7 }).resolve(big)
    assertEquals(5, offset.size)
    assertEquals("0x00011000: " + "07 ".repeat(63) + "07", offset[0])
    val search = XbePatch.Search(ByteArray(200) { 1 }, ByteArray(200) { 2 }).resolve(big)
    assertEquals(4, search.size)
    // the core reads a manifest line in 1023 characters, "patch<TAB>xbe<TAB>mod<TAB>" before it
    for (line in offset + search) assertTrue(line.length < 500)
    assertEquals(0x11000L + 64, search[1].substringBefore(':').removePrefix("0x").toLong(16))
  }
}
