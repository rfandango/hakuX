package com.rfandango.haku_x.input

import kotlin.math.hypot

/**
 * Double taps on empty space. A tap is a touch that lifts soon (tapMaxMs)
 * and has hardly moved (slopPx); a touch that comes down within gapMs of a
 * tap's lifting, near it (nearPx) and in the same half of the screen, is the
 * second of a double tap. That one is then held (it is not asked again
 * whether it was a tap: a triple tap is a double and then a tap).
 *
 * Plain Kotlin: the controller tells it about the touches that came down on
 * empty space, their moves and their lifting.
 */
class DoubleTapDetector(
  private val tapMaxMs: Long,
  private val slopPx: Float,
  private val nearPx: Float,
) {
  /** The longest wait (ms) between a tap's lifting and the next touch's coming down. */
  var gapMs = 250L

  private class Touch(val time: Long, val x: Float, val y: Float, val side: Int) {
    var travel = 0f
  }

  private val touches = HashMap<Int, Touch>()
  private var hasTap = false
  private var tapTime = 0L
  private var tapX = 0f
  private var tapY = 0f
  private var tapSide = 0

  /**
   * A touch came down at (x, y) on empty space, in the half of the screen
   * side (0 left, 1 right). armed: a double tap is wanted on that side.
   * Returns whether it is the second tap of a double tap.
   */
  fun down(pointer: Int, timeMs: Long, x: Float, y: Float, side: Int, armed: Boolean): Boolean {
    val wait = timeMs - tapTime
    if (armed && hasTap && tapSide == side && wait >= 0L && wait <= gapMs && hypot(x - tapX, y - tapY) <= nearPx) {
      hasTap = false
      touches.remove(pointer)
      return true
    }
    hasTap = false // a touch that is not the second of a pair makes an earlier tap stale
    touches[pointer] = Touch(timeMs, x, y, side)
    return false
  }

  fun move(pointer: Int, x: Float, y: Float) {
    val t = touches[pointer] ?: return
    val d = hypot(x - t.x, y - t.y)
    if (d > t.travel) t.travel = d
  }

  /** The touch lifted: if it was a tap, a double tap may follow. */
  fun up(pointer: Int, timeMs: Long) {
    val t = touches.remove(pointer) ?: return
    if (timeMs - t.time <= tapMaxMs && t.travel <= slopPx) {
      hasTap = true
      tapTime = timeMs
      tapX = t.x
      tapY = t.y
      tapSide = t.side
    }
  }

  /** Every touch is over (cancelled): nothing is half a double tap. */
  fun cancel() {
    touches.clear()
    hasTap = false
  }
}
