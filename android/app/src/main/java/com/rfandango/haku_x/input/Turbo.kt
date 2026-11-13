package com.rfandango.haku_x.input

/** Auto-repeat: a held button pressed over and over. */
object Turbo {
  /**
   * Whether an auto-repeating button is down elapsedMs after it was first
   * held: down for the first half of every period, up for the second, so
   * the first press is at once.
   */
  fun pressed(elapsedMs: Long, periodMs: Int): Boolean {
    if (periodMs < 2) return true
    val e = if (elapsedMs < 0L) 0L else elapsedMs
    return e % periodMs.toLong() < (periodMs / 2).toLong()
  }
}
