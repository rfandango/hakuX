package com.rfandango.haku_x.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ModRegionsTest {
  @Test
  fun parsesTheNamesTheLibraryShows() {
    assertEquals(ModRegions.NTSC, ModRegions.parse("NTSC"))
    assertEquals(ModRegions.NTSC_J, ModRegions.parse("ntsc-j"))
    assertEquals(ModRegions.NTSC or ModRegions.PAL, ModRegions.parse("NTSC, PAL"))
    assertEquals(7, ModRegions.parse("NTSC, NTSC-J, PAL"))
    assertEquals(7, ModRegions.parse("NTSC + NTSC-J + PAL"))
  }

  @Test
  fun aLineThatNamesNoRegionFitsNoDisc() {
    val none = ModRegions.parse("somewhere")
    assertFalse(ModRegions.fits(none, 7))
    assertFalse(ModRegions.fits(none, 0))
  }

  @Test
  fun aModFitsTheDiscsOfItsRegions() {
    assertTrue(ModRegions.fits(0, 0))                       // no region: any disc, even an unknown one
    assertTrue(ModRegions.fits(ModRegions.NTSC, ModRegions.NTSC))
    assertFalse(ModRegions.fits(ModRegions.NTSC, ModRegions.PAL))
    assertTrue(ModRegions.fits(ModRegions.NTSC or ModRegions.PAL, ModRegions.PAL))
    assertTrue(ModRegions.fits(ModRegions.PAL, ModRegions.NTSC or ModRegions.PAL))   // a region-free disc
    assertFalse(ModRegions.fits(ModRegions.NTSC, 0))        // a disc whose region is not known
  }
}
