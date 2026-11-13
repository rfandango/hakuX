package com.rfandango.haku_x.input

import kotlin.math.hypot
import kotlin.math.ln
import kotlin.math.max
import kotlin.math.min

/**
 * Swipe to look: a finger's motion as the right stick's deflection.
 *
 * A stick is a velocity command (the game turns the camera at a rate the
 * deflection sets), a finger on a screen is a position. To make the camera
 * follow the finger and stop with it, the stick is held at the finger's
 * velocity: the game integrates that into the finger's own displacement.
 *
 * The finger arrives as timestamped moves.
 *
 * 1. A small circular play (a dead band in space, not in speed) absorbs the
 *    flicker of a resting finger. A speed floor would do that too, but would
 *    cut off the slow swipes that fine aim is made of.
 * 2. The finger's cumulative position is a piecewise-linear function of event
 *    time, and the answer is
 *
 *        velocity(now) = (position(q) - position(q - window)) / window,  q = now - delay
 *
 *    a difference of positions, so what the camera is given adds up to the
 *    finger's displacement however the touch events were batched or timed. The
 *    delay adapts to how stale the newest sample can be (how late the system
 *    delivered it, and how far apart the panel's samples are: a panel reports
 *    a slow finger only when a pixel changes, which can be 100 ms apart). Any
 *    shorter, the newest edge of the window would have to be guessed, which
 *    biases the output low. While it changes, the output is scaled by the rate
 *    q runs at, which keeps the sum exact. A silence longer than SPAN_MAX is a
 *    pause: the move after it is placed in the last PULSE_MS, where the output
 *    can still reach it, instead of being smeared across the pause.
 * 3. Acceleration: the velocity is scaled by a gain of the finger's physical
 *    speed: below 1 when slow (fine aim), 1 at a normal swipe, above when fast.
 * 4. Catch-up: the stick cannot go past full deflection, so a fast flick would
 *    turn the camera no further than the game's top speed allows for as long
 *    as the finger moved. What the demand exceeds it by is kept (the backlog,
 *    capped) and paid back at once after, so a fast flick turns further. A
 *    finger coming down, or a move against it, drops what is still owed: the
 *    camera never fights the finger.
 * 5. The demand goes through a radial curve: it skips the game's own dead zone
 *    (an Xbox title ignores the stick up to about a quarter of its range), then
 *    is exactly linear, so slow motion still turns the camera one to one, and
 *    the direction of the swipe is kept.
 *
 * Plain Kotlin, no Android types: the controller feeds it from the UI thread
 * and reads it there, so it takes no locks.
 */
class TouchLookEngine {
  /** The play's radius (dp): the controller sets it from a number of pixels. */
  var playDp = DEFAULT_PLAY

  private val time = DoubleArray(CAP)
  private val px = DoubleArray(CAP)
  private val py = DoubleArray(CAP)
  private var head = 0
  private var size = 0

  /** The finger's position through the play (dp), and where it really is. */
  private var sumX = 0.0
  private var sumY = 0.0
  private var rawX = 0.0
  private var rawY = 0.0

  /** The last RING samples' delivery lateness and spacing (ms). */
  private val late = DoubleArray(RING)
  private val gap = DoubleArray(RING)
  private var ring = 0
  private var lastEvent = -1.0

  /** The delay q lags now by (ms), and when it was last adapted. */
  private var delay = DELAY_START
  private var lastNow = -1.0

  /** What the stick is still owed (in full-speed seconds), and when it was last paid. */
  private var backX = 0.0
  private var backY = 0.0
  private var lastStep = -1.0

  /** The last sample()'s answer: the stick's deflection, x right and y down, each within -1..1. */
  var outX = 0f
    private set
  var outY = 0f
    private set

  /** position() writes its answer here. */
  private var atX = 0.0
  private var atY = 0.0

  /** Forgets everything (a new game, or the feature turned off). */
  fun clear() {
    head = 0
    size = 0
    sumX = 0.0
    sumY = 0.0
    rawX = 0.0
    rawY = 0.0
    late.fill(0.0)
    gap.fill(0.0)
    ring = 0
    lastEvent = -1.0
    delay = DELAY_START
    lastNow = -1.0
    backX = 0.0
    backY = 0.0
    lastStep = -1.0
    outX = 0f
    outY = 0f
  }

  /**
   * A finger came down at eventMs (the event's time): the position stays
   * where the last one left it, so a finger landing elsewhere is not a jump,
   * and what the camera was still owed is dropped (a touch stops it).
   */
  fun beginStroke(eventMs: Double) {
    push(eventMs, sumX, sumY)
    lastEvent = -1.0 // the next spacing is not measured across strokes
    rawX = sumX // the play starts centred on where the finger came down
    rawY = sumY
    backX = 0.0
    backY = 0.0
  }

  /**
   * The finger moved by (dx, dy) dp at eventMs (the sample's time); arrivalMs
   * is when the system delivered it.
   */
  fun addMove(eventMs: Double, arrivalMs: Double, dx: Double, dy: Double) {
    val newest = if (size > 0) time[at(size - 1)] else eventMs
    var t = max(eventMs, newest)
    rawX += dx
    rawY += dy
    val ex = rawX - sumX
    val ey = rawY - sumY
    val dist = hypot(ex, ey)
    val prevX = sumX
    val prevY = sumY
    if (dist > playDp) {
      val k = 1.0 - playDp / dist
      sumX += ex * k
      sumY += ey * k
    }
    if (size > 0 && t - newest > SPAN_MAX) {
      push(t - PULSE_MS, prevX, prevY)
    }
    t = push(t, sumX, sumY)
    val i = ring % RING
    ring++
    late[i] = max(0.0, arrivalMs - t)
    val g = if (lastEvent >= 0.0) t - lastEvent else 0.0
    gap[i] = if (g > 0.0 && g <= GAP_MAX) g else 0.0
    lastEvent = t
  }

  /** Appends a point (time never goes back); returns the time it was kept at. */
  private fun push(eventMs: Double, x: Double, y: Double): Double {
    var t = eventMs
    if (size > 0) {
      val newest = time[at(size - 1)]
      if (t < newest) t = newest
    }
    if (size == CAP) {
      head = (head + 1) % CAP
      size--
    }
    val i = at(size)
    time[i] = t
    px[i] = x
    py[i] = y
    size++
    return t
  }

  private fun at(k: Int) = (head + k) % CAP

  /** The finger's position at time tau (linear between samples, held before the first and after the last). */
  private fun position(tau: Double) {
    val first = at(0)
    val last = at(size - 1)
    if (tau <= time[first]) {
      atX = px[first]
      atY = py[first]
      return
    }
    if (tau >= time[last]) {
      atX = px[last]
      atY = py[last]
      return
    }
    var lo = 0
    var hi = size - 1 // time[lo] <= tau < time[hi]
    while (hi - lo > 1) {
      val mid = (lo + hi) ushr 1
      if (time[at(mid)] <= tau) lo = mid else hi = mid
    }
    val a = at(lo)
    val b = at(hi)
    val span = time[b] - time[a]
    if (span <= 0.0) {
      atX = px[b]
      atY = py[b]
      return
    }
    val f = (tau - time[a]) / span
    atX = px[a] + (px[b] - px[a]) * f
    atY = py[a] + (py[b] - py[a]) * f
  }

  /** Moves the delay toward what the samples need; returns the rate q runs at (1 - d delay / d now). */
  private fun adapt(now: Double): Double {
    var worstLate = 0.0
    var worstGap = 0.0
    for (i in 0 until RING) {
      worstLate = max(worstLate, late[i])
      worstGap = max(worstGap, gap[i])
    }
    val target = min(max(worstLate + worstGap + 1.0, DELAY_MIN), DELAY_MAX)
    val dt = if (lastNow < 0.0) 0.0 else max(0.0, now - lastNow)
    lastNow = now
    val before = delay
    delay = if (target > delay) min(target, delay + DELAY_RISE * dt) else max(target, delay - DELAY_FALL * dt)
    return if (dt > 0.0) 1.0 - (delay - before) / dt else 1.0
  }

  /**
   * The stick's deflection for time nowMs (uptime ms, the events' clock), into
   * outX and outY. windowMs is the smoothing; sensitivityX and sensitivityY
   * scale the finger's speed (1.0 is the default feel); deadZone is the game's
   * own dead zone to skip, 0..1; accel is the acceleration level, 0 (off) to 3.
   *
   * Returns whether more is to follow (call again soon): false once the finger
   * has been still for the whole window and nothing is owed, the output zero.
   */
  fun sample(
    nowMs: Double,
    windowMs: Double,
    sensitivityX: Double,
    sensitivityY: Double,
    deadZone: Double,
    accel: Int,
  ): Boolean {
    val level = accel.coerceIn(0, ACCEL_LEVELS - 1)
    if (size == 0 && backX == 0.0 && backY == 0.0) {
      outX = 0f
      outY = 0f
      return false
    }
    var vx = 0.0
    var vy = 0.0
    var busy = false
    if (size > 0) {
      val rate = adapt(nowMs)
      val q = nowMs - delay
      // Nothing before q - window is looked at again: drop all but the point that brackets it.
      val from = q - windowMs
      while (size >= 2 && time[at(1)] <= from) {
        head = (head + 1) % CAP
        size--
      }
      if (from < time[at(size - 1)]) {
        position(q)
        val x1 = atX
        val y1 = atY
        position(from)
        // q runs at `rate` times real time while the delay moves: scaling by it keeps the sum exact.
        val scale = 1000.0 / windowMs * rate
        vx = (x1 - atX) * scale
        vy = (y1 - atY) * scale
        busy = true
      }
    }

    // what the finger asks of the stick (1.0 is full deflection, and may be exceeded)
    val gain = accelGain(hypot(vx, vy), level)
    val mx = vx * sensitivityX * gain / V_FULL
    val my = vy * sensitivityY * gain / V_FULL
    val mag = hypot(mx, my)

    // catch-up: what the stick cannot give now is owed, and paid back once it can
    val backMax = BACKLOG_MAX[level]
    val dt = if (lastStep < 0.0) 0.0 else min(max(nowMs - lastStep, 0.0), STEP_MAX) / 1000.0
    lastStep = nowMs
    if (backMax <= 0.0) {
      backX = 0.0
      backY = 0.0
    } else if (backX * mx + backY * my < 0.0 && mag > CANCEL_MIN) {
      backX = 0.0
      backY = 0.0
    }
    val wx = mx + backX / PAY_TAU
    val wy = my + backY / PAY_TAU
    val wm = hypot(wx, wy)
    val dx = if (wm > 1.0) wx / wm else wx
    val dy = if (wm > 1.0) wy / wm else wy
    if (backMax > 0.0) {
      backX += (mx - dx) * dt
      backY += (my - dy) * dt
      val bm = hypot(backX, backY)
      if (bm > backMax) {
        backX *= backMax / bm
        backY *= backMax / bm
      } else if (bm < BACKLOG_EPS && mag < 1.0) {
        backX = 0.0
        backY = 0.0
      }
    }

    // the curve
    val dm = hypot(dx, dy)
    if (dm <= 0.0) {
      outX = 0f
      outY = 0f
    } else {
      val k = shape(dm, deadZone) / dm
      outX = (dx * k).toFloat()
      outY = (dy * k).toFloat()
    }
    return busy || backX != 0.0 || backY != 0.0
  }

  companion object {
    private const val CAP = 256

    /** Finger speed (dp/s, after sensitivity) that is the stick's full deflection. */
    const val V_FULL = 2500.0

    /**
     * Where the dead-zone ramp ends: demands up to this share of V_FULL span
     * 0..the dead zone's edge, so the camera starts to move the moment the
     * finger does (and speeds near zero are continuous, not a jump).
     */
    const val M0 = 0.001

    /** The play's radius (dp) when none is set. */
    const val DEFAULT_PLAY = 1.0

    private const val DELAY_MIN = 4.0
    private const val DELAY_MAX = 130.0
    private const val DELAY_START = 16.0
    private const val DELAY_RISE = 0.25
    private const val DELAY_FALL = 0.15
    private const val RING = 8

    /** A silence longer than this (ms) is a pause, not a spacing between samples. */
    private const val GAP_MAX = 120.0
    private const val SPAN_MAX = 120.0

    /** Where the move after a pause is placed (ms before it): no earlier than the output has already passed. */
    private const val PULSE_MS = 4.0

    const val ACCEL_LEVELS = 4

    /** Per acceleration level (off, low, medium, high): the gain when slow, when fast, and the backlog's cap (full-speed seconds). */
    private val ACC_LO = doubleArrayOf(1.0, 0.85, 0.70, 0.55)
    private val ACC_HI = doubleArrayOf(1.0, 1.5, 2.2, 3.0)
    private val BACKLOG_MAX = doubleArrayOf(0.0, 0.12, 0.22, 0.35)

    /** Finger speeds (dp/s): where the slow gain is fully reached, a normal swipe (gain 1), where the fast gain is. */
    private const val S_LO = 30.0
    private const val S_REF = 600.0
    private const val S_HI = 2200.0

    /** The backlog is paid back with this time constant (s), and dropped below this (full-speed seconds). */
    private const val PAY_TAU = 0.05
    private const val BACKLOG_EPS = 0.006

    /** A move against the backlog drops it once it asks for more than this. */
    private const val CANCEL_MIN = 0.05

    /** The longest step (ms) the backlog is integrated over. */
    private const val STEP_MAX = 50.0

    private fun smooth(x: Double) = x * x * (3.0 - 2.0 * x)

    /** The gain for a finger speed s (dp/s) at an acceleration level: below 1 slow, 1 at a normal swipe, above fast. */
    fun accelGain(s: Double, level: Int): Double {
      if (level <= 0) return 1.0
      val lo = ACC_LO[level]
      val hi = ACC_HI[level]
      if (s >= S_REF) {
        val u = min(max(ln(s / S_REF) / ln(S_HI / S_REF), 0.0), 1.0)
        return 1.0 + (hi - 1.0) * smooth(u)
      }
      if (s <= 0.0) return lo
      val w = min(max(ln(S_REF / s) / ln(S_REF / S_LO), 0.0), 1.0)
      return 1.0 - (1.0 - lo) * smooth(w)
    }

    /**
     * The deflection (0..1) for a demand (m, 1 is full): the game's dead zone
     * skipped, then exactly linear (so the camera turns one to one with the
     * finger), 1 at full. A ramp of M0 up to it keeps tiny speeds continuous.
     */
    fun shape(m: Double, deadZone: Double): Double {
      val m0 = min(M0, deadZone)
      return when {
        m <= 0.0 -> 0.0
        m0 > 0.0 && m <= m0 -> m * (deadZone + (1.0 - deadZone) * m0) / m0
        m < 1.0 -> deadZone + (1.0 - deadZone) * m
        else -> 1.0
      }
    }
  }
}
