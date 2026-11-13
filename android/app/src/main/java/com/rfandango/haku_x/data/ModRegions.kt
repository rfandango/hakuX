package com.rfandango.haku_x.data

import java.util.Locale

/**
 * The regions a disc is made for (the certificate's XBE_REGION_ flags, XboxDisc.regions) and the
 * ones a mod lists in its "region:" line (mods/README.md): NTSC, NTSC-J and PAL, as the library
 * shows them.
 */
object ModRegions {
  const val NTSC = 1
  const val NTSC_J = 2
  const val PAL = 4

  /** A "region:" line that names none of the three: a bit no disc has, so the mod fits no disc. */
  private const val NONE = 8

  /** The flags a "region:" line lists: names (NTSC, NTSC-J, PAL) apart by commas, "+" or spaces. */
  fun parse(text: String): Int {
    var flags = 0
    for (name in text.split(',', '+', ' ', '\t')) {
      flags = flags or when (name.trim().uppercase(Locale.ROOT)) {
        "NTSC" -> NTSC
        "NTSC-J" -> NTSC_J
        "PAL" -> PAL
        else -> 0
      }
    }
    return if (flags == 0) NONE else flags
  }

  /** Whether a mod that lists the regions `mod` (0: no "region:" line, every disc) fits a disc made for `disc` (0: not known). */
  fun fits(mod: Int, disc: Int) = mod == 0 || (mod and disc) != 0
}
