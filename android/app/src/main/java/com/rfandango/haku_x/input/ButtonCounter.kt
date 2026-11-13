package com.rfandango.haku_x.input

/**
 * How many things hold each button down. A button on the screen, a custom
 * button mapped to it, a double tap holding it and a turbo pulse can all want
 * the same pad button at once: it goes down when the first of them takes it
 * and comes up only when the last lets go, so one letting go does not drop it
 * for the others.
 *
 * Buttons are numbered (an enum's ordinals); onChange is told each one that
 * goes down (pressed) or up.
 */
class ButtonCounter(size: Int, private val onChange: (index: Int, pressed: Boolean) -> Unit) {
  private val counts = IntArray(size)

  fun acquire(index: Int) {
    if (counts[index]++ == 0) onChange(index, true)
  }

  fun release(index: Int) {
    if (counts[index] > 0 && --counts[index] == 0) onChange(index, false)
  }

  /** Everything lets go (the controller was reset): every button still down comes up. */
  fun releaseAll() {
    for (i in counts.indices) {
      if (counts[i] > 0) {
        counts[i] = 0
        onChange(i, false)
      }
    }
  }

  fun count(index: Int) = counts[index]
}
