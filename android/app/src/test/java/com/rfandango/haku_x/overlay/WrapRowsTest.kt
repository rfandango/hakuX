package com.rfandango.haku_x.overlay

import org.junit.Assert.assertArrayEquals
import org.junit.Test

class WrapRowsTest {
  /** The editor bar's groups (dp): the selected control, opacity, reset / cancel / done, fold. */
  private val groups = intArrayOf(318, 192, 202, 40)
  private val gap = 13

  @Test
  fun oneRowWhenAllFit() {
    assertArrayEquals(intArrayOf(0, 0, 0, 0), wrapRows(groups, gap, 791))
  }

  @Test
  fun wrapsOn4by3() {
    // 640 dp across (a 1280 x 960 screen in landscape), the bar's padding off
    assertArrayEquals(intArrayOf(0, 0, 1, 1), wrapRows(groups, gap, 622))
  }

  @Test
  fun gapsCount() {
    assertArrayEquals(intArrayOf(0, 0, 0, 1), wrapRows(groups, gap, 790))
  }

  @Test
  fun tooWideGetsItsOwnRow() {
    assertArrayEquals(intArrayOf(0, 1, 2, 2), wrapRows(groups, gap, 300))
  }
}
