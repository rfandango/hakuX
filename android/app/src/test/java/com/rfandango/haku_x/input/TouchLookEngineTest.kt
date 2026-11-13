package com.rfandango.haku_x.input

import com.rfandango.haku_x.data.TouchLookConfig
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.round
import kotlin.math.sign

class TouchLookEngineTest {
  private class Stroke(val startMs: Double, val durationMs: Double, val x: (Double) -> Double, val y: (Double) -> Double)

  private class Event(val arrival: Double, val time: Double, val dx: Double, val dy: Double, val down: Boolean)

  private class Outcome(
    /** The stick summed over time: the turn, in seconds at the game's top speed (a linear game, no dead zone). */
    val turnX: Double,
    /** The turn of a game that skips its own dead zone (gameDeadZone) and is linear above it. */
    val gameTurn: Double,
    val peak: Double,
    val lastNonZeroMs: Double,
    val moreAtEnd: Boolean,
    val minDegrees: Double,
    val maxDegrees: Double,
    val minX: Double,
    val maxX: Double,
  )

  private val still: (Double) -> Double = { 0.0 }

  /** A minimum-jerk reach, the shape of a real finger's flick: 0 at start, total at start + duration. */
  private fun reach(total: Double, start: Double, duration: Double): (Double) -> Double = { t ->
    val s = min(max((t - start) / duration, 0.0), 1.0)
    total * (10 * s * s * s - 15 * s * s * s * s + 6 * s * s * s * s * s)
  }

  /** A finger at a constant speed (dp/s) from time 0. */
  private fun line(speed: Double): (Double) -> Double = { t -> speed * t / 1000.0 }

  /**
   * What the system delivers for strokes. dense: a report every 1/hz (even when nothing moved);
   * otherwise a report only when the pixel the finger is on changes (what most panels do, and
   * the only thing a slow finger gives). Each is delivered lateMs after it happened.
   */
  private fun events(strokes: List<Stroke>, dense: Boolean = true, hz: Double = 120.0, lateMs: Double = 0.5): List<Event> {
    val list = ArrayList<Event>()
    for (s in strokes) {
      list += Event(s.startMs + lateMs, s.startMs, 0.0, 0.0, true)
      var lx = 0.0
      var ly = 0.0
      var lqx = Math.round(s.x(s.startMs) / PX)
      var lqy = Math.round(s.y(s.startMs) / PX)
      val step = if (dense) 1000.0 / hz else 0.25
      var k = 1
      while (true) {
        val t = s.startMs + k * step
        if (t > s.startMs + s.durationMs + 1e-9) break
        if (dense) {
          val x = s.x(t)
          val y = s.y(t)
          val time = round(t)
          list += Event(time + lateMs, time, x - lx, y - ly, false)
          lx = x
          ly = y
        } else {
          val qx = Math.round(s.x(t) / PX)
          val qy = Math.round(s.y(t) / PX)
          if (qx != lqx || qy != lqy) {
            val time = round(t)
            list += Event(time + lateMs, time, (qx - lqx) * PX, (qy - lqy) * PX, false)
            lqx = qx
            lqy = qy
          }
        }
        k++
      }
    }
    list.sortBy { it.arrival }
    return list
  }

  /** The events played into the engine, its stick read every 4 ms until endMs. */
  private fun drive(
    engine: TouchLookEngine,
    events: List<Event>,
    endMs: Double,
    window: Double = 24.0,
    sensX: Double = 1.0,
    sensY: Double = sensX,
    deadZone: Double = 0.0,
    accel: Int = 0,
    playDp: Double = 0.96,
    gameDeadZone: Double = 0.0,
  ): Outcome {
    engine.playDp = playDp
    var i = 0
    var turnX = 0.0
    var game = 0.0
    var peak = 0.0
    var lastNonZero = 0.0
    var more = false
    var minDeg = Double.MAX_VALUE
    var maxDeg = -Double.MAX_VALUE
    var minX = 0.0
    var maxX = 0.0
    var now = 0.0
    while (now <= endMs) {
      while (i < events.size && events[i].arrival <= now) {
        val e = events[i++]
        if (e.down) engine.beginStroke(e.time) else engine.addMove(e.time, e.arrival, e.dx, e.dy)
      }
      more = engine.sample(now, window, sensX, sensY, deadZone, accel)
      val ox = engine.outX.toDouble()
      val oy = engine.outY.toDouble()
      turnX += ox * 0.004
      game += sign(ox) * max(abs(ox) - gameDeadZone, 0.0) / (1 - gameDeadZone) * 0.004
      minX = min(minX, ox)
      maxX = max(maxX, ox)
      val mag = hypot(ox, oy)
      peak = max(peak, mag)
      if (mag > 0.1) {
        val deg = Math.toDegrees(atan2(oy, ox))
        minDeg = min(minDeg, deg)
        maxDeg = max(maxDeg, deg)
      }
      if (mag > 0.0) lastNonZero = now
      now += 4.0
    }
    return Outcome(turnX, game, peak, lastNonZero, more, minDeg, maxDeg, minX, maxX)
  }

  private fun swipe(total: Double, durationMs: Double, accel: Int, sens: Double): Outcome =
    drive(
      TouchLookEngine(),
      events(listOf(Stroke(100.0, durationMs, reach(total, 100.0, durationMs), still))),
      100.0 + durationMs + 2000.0,
      sensX = sens,
      accel = accel,
    )

  // ------------------------------------------------------------------ accuracy

  @Test
  fun anEngineThatSawNothingIsStill() {
    val e = TouchLookEngine()
    assertFalse(e.sample(1000.0, 24.0, 1.0, 1.0, 0.2, 2))
    assertEquals(0f, e.outX, 0f)
    assertEquals(0f, e.outY, 0f)
  }

  @Test
  fun aSteadySwipeReadsAsItsSpeed() {
    // 600 dp/s: 4.8 dp every 8 ms
    val e = TouchLookEngine()
    e.beginStroke(0.0)
    for (t in 8..1000 step 8) e.addMove(t.toDouble(), t + 0.5, 4.8, 0.0)
    assertTrue(e.sample(1000.5, 24.0, 1.0, 1.0, 0.0, 0))
    assertEquals(600.0 / TouchLookEngine.V_FULL, e.outX.toDouble(), 0.003)
    assertEquals(0.0, e.outY.toDouble(), 1e-6)
  }

  @Test
  fun theCameraLandsWhereTheFingerDid() {
    // a 300 dp flick in 160 ms (sensitivity halved so it doesn't reach full deflection), acceleration off
    val r = swipe(300.0, 160.0, accel = 0, sens = 0.5)
    assertEquals(300.0, r.turnX * TouchLookEngine.V_FULL / 0.5, 4.5)
    assertTrue(r.peak < 1.0)
  }

  @Test
  fun thatHoldsForEveryPanelRateAndSmoothing() {
    for (hz in listOf(60.0, 120.0, 240.0, 480.0)) {
      for (w in listOf(8.0, 16.0, 24.0, 40.0)) {
        val r = drive(
          TouchLookEngine(),
          events(listOf(Stroke(100.0, 200.0, reach(120.0, 100.0, 200.0), still)), hz = hz),
          2000.0,
          window = w,
        )
        assertEquals("$hz Hz, window $w ms", 120.0, r.turnX * TouchLookEngine.V_FULL, 120.0 * 0.03)
      }
    }
  }

  // ---------------------------------------------------------------- fine aim

  @Test
  fun aSlowSwipeStillTurnsTheCamera() {
    // 1.5 to 3 mm/s, the panel reporting only when a pixel changes (one step every 20-80 ms).
    // The game's dead zone is what the setting says, sensitivity 150%: the camera gets nearly all of it.
    for (speed in listOf(20.0, 10.0, 5.0)) {
      val distance = 30.0
      val duration = distance / speed * 1000.0
      val r = drive(
        TouchLookEngine(),
        events(listOf(Stroke(0.0, duration, line(speed), still)), dense = false),
        duration + 1500.0,
        sensX = 1.5,
        deadZone = 0.25,
        gameDeadZone = 0.25,
      )
      val ideal = distance * 1.5 / TouchLookEngine.V_FULL
      assertTrue("$speed dp/s: ${r.gameTurn / ideal}", r.gameTurn / ideal > 0.92)
      assertTrue("$speed dp/s: ${r.gameTurn / ideal}", r.gameTurn / ideal < 1.02)
    }
  }

  @Test
  fun aRestingFingerWhoseReadingFlickersMovesNothing() {
    // 2 px back and forth every 8 ms for 3 s, at a high sensitivity
    val flicker: (Double) -> Double = { t -> PX * 2 * (if ((t / 8.0).toInt() % 2 == 1) 1.0 else 0.0) }
    val r = drive(
      TouchLookEngine(),
      events(listOf(Stroke(50.0, 3000.0, flicker, still)), dense = false),
      3500.0,
      sensX = 4.0,
      sensY = 6.0,
      deadZone = 0.25,
    )
    assertEquals(0.0, r.peak, 1e-9)
  }

  // ------------------------------------------------------------- stopping

  @Test
  fun theCameraStopsWithTheFinger() {
    // a normal swipe, acceleration on: nothing is owed afterwards, the stick is zero again within the window and the delay
    val r = swipe(120.0, 300.0, accel = 2, sens = 2.0)
    assertTrue("still moving at ${r.lastNonZeroMs} ms", r.lastNonZeroMs - 400.0 < 70.0)
    assertFalse(r.moreAtEnd)
  }

  @Test
  fun aFingerLandingStopsWhatIsStillTurning() {
    // a fast flick would turn the camera on for ~150 ms; a finger coming down 40 ms after it stops that at once
    val flick = Stroke(100.0, 60.0, reach(80.0, 100.0, 60.0), still)
    val rest = Stroke(200.0, 700.0, still, still)
    val r = drive(TouchLookEngine(), events(listOf(flick, rest)), 3000.0, sensX = 2.0, accel = 2)
    assertTrue("still turning at ${r.lastNonZeroMs} ms", r.lastNonZeroMs <= 212.0)
  }

  // ---------------------------------------------------------- acceleration

  @Test
  fun aNormalSwipeIsUnchangedAndSlowerOnesTurnLess() {
    // acceleration medium against off, at the same sensitivity
    fun ratio(total: Double, ms: Double) = swipe(total, ms, 2, 2.0).turnX / swipe(total, ms, 0, 2.0).turnX
    assertEquals("normal (6 cm/s)", 1.0, ratio(120.0, 300.0), 0.08)
    val slow = ratio(30.0, 600.0)
    assertTrue("slow aim: $slow", slow > 0.70 && slow < 0.84)
    val quick = ratio(150.0, 200.0)
    assertTrue("quick swipe: $quick", quick > 1.35 && quick < 1.65)
  }

  @Test
  fun aFastFlickTurnsFurtherAndThenStops() {
    val off = swipe(80.0, 60.0, 0, 2.0)
    val low = swipe(80.0, 60.0, 1, 2.0)
    val medium = swipe(80.0, 60.0, 2, 2.0)
    val high = swipe(80.0, 60.0, 3, 2.0)
    val gain = medium.turnX / off.turnX
    assertTrue("medium: $gain", gain > 2.2 && gain < 2.7)
    assertTrue(low.turnX < medium.turnX && medium.turnX < high.turnX)
    // it keeps turning for a moment (the finger stopped at 160 ms), but only a moment, and then it is over
    assertTrue("still turning at ${medium.lastNonZeroMs} ms", medium.lastNonZeroMs - 160.0 < 250.0)
    assertFalse(medium.moreAtEnd)
  }

  @Test
  fun neverTheOppositeWayForAMonotonicSwipe() {
    for (level in 0..3) {
      for ((total, ms) in listOf(300.0 to 100.0, 120.0 to 300.0, 60.0 to 800.0, -200.0 to 150.0)) {
        val r = swipe(total, ms, level, 3.0)
        if (total > 0) assertTrue("level $level, $total dp", r.minX >= -1e-12)
        if (total < 0) assertTrue("level $level, $total dp", r.maxX <= 1e-12)
      }
    }
  }

  @Test
  fun theGainIsOneAtANormalSwipeAndBendsEitherWay() {
    assertEquals(1.0, TouchLookEngine.accelGain(123.0, 0), 0.0)
    assertEquals(1.0, TouchLookEngine.accelGain(600.0, 2), 1e-9)
    assertEquals(0.70, TouchLookEngine.accelGain(30.0, 2), 1e-9)
    assertEquals(0.70, TouchLookEngine.accelGain(0.0, 2), 1e-9)
    assertEquals(2.2, TouchLookEngine.accelGain(2200.0, 2), 1e-9)
    assertEquals(2.2, TouchLookEngine.accelGain(9000.0, 2), 1e-9)
    var last = 0.0
    var s = 1.0
    while (s < 4000.0) {
      val g = TouchLookEngine.accelGain(s, 3)
      assertTrue("not rising at $s dp/s", g >= last)
      last = g
      s *= 1.05
    }
  }

  // --------------------------------------------------------- axes and curve

  @Test
  fun verticalSensitivityIsItsOwn() {
    // 600 dp/s along y with y at 200% and x at 100%, then along x
    val ey = TouchLookEngine()
    ey.beginStroke(0.0)
    for (t in 8..1000 step 8) ey.addMove(t.toDouble(), t + 0.5, 0.0, 4.8)
    ey.sample(1000.5, 24.0, 1.0, 2.0, 0.0, 0)
    val ex = TouchLookEngine()
    ex.beginStroke(0.0)
    for (t in 8..1000 step 8) ex.addMove(t.toDouble(), t + 0.5, 4.8, 0.0)
    ex.sample(1000.5, 24.0, 1.0, 2.0, 0.0, 0)
    assertEquals(0.48f, ey.outY, 0.003f)
    assertEquals(0.24f, ex.outX, 0.003f)
  }

  @Test
  fun aDiagonalSwipeKeepsItsDirection() {
    val a = Math.toRadians(30.0)
    val stroke = Stroke(100.0, 300.0, reach(300.0 * Math.cos(a), 100.0, 300.0), reach(300.0 * Math.sin(a), 100.0, 300.0))
    val r = drive(TouchLookEngine(), events(listOf(stroke)), 2000.0, deadZone = 0.25)
    assertEquals(30.0, r.minDegrees, 0.5)
    assertEquals(30.0, r.maxDegrees, 0.5)
  }

  @Test
  fun aFingerLandingElsewhereIsNoJump() {
    // the second finger comes down 10 ms after the first lifted (the engine keeps only moves, not places)
    val one = drive(TouchLookEngine(), events(listOf(Stroke(100.0, 200.0, reach(100.0, 100.0, 200.0), still))), 2000.0)
    val two = drive(
      TouchLookEngine(),
      events(
        listOf(
          Stroke(100.0, 200.0, reach(100.0, 100.0, 200.0), still),
          Stroke(310.0, 200.0, reach(100.0, 310.0, 200.0), still),
        ),
      ),
      2000.0,
    )
    assertEquals(200.0, two.turnX * TouchLookEngine.V_FULL, 3.0)
    assertTrue(two.peak <= one.peak * 1.05)
  }

  @Test
  fun clearForgetsTheFinger() {
    val e = TouchLookEngine()
    e.beginStroke(0.0)
    e.addMove(8.0, 8.5, 4.8, 0.0)
    e.clear()
    assertFalse(e.sample(20.0, 24.0, 1.0, 1.0, 0.2, 2))
    assertEquals(0f, e.outX, 0f)
  }

  @Test
  fun theCurveSkipsTheGamesDeadZoneThenIsLinear() {
    fun shape(m: Double, deadZone: Double) = TouchLookEngine.shape(m, deadZone)
    assertEquals(0.0, shape(0.0, 0.25), 0.0)
    // just above the ramp the stick is at the game's dead zone's edge: it only just starts to turn
    assertEquals(0.25 + 0.75 * TouchLookEngine.M0, shape(TouchLookEngine.M0, 0.25), 1e-12)
    // then exactly linear, one to one with the finger, and full at full
    assertEquals(0.55, shape(0.4, 0.25), 1e-12)
    assertEquals(1.0, shape(1.0, 0.25), 1e-12)
    assertEquals(1.0, shape(3.0, 0.25), 0.0)
    // no dead zone to skip: the curve is the identity
    assertEquals(0.4, shape(0.4, 0.0), 1e-12)
    var last = 0.0
    var m = 0.0
    while (m <= 1.3) {
      val v = shape(m, 0.25)
      assertTrue("not rising at $m", v >= last)
      last = v
      m += 0.0005
    }
  }

  // ------------------------------------------- buttons, turbo, double taps, mappings

  @Test
  fun aButtonStaysDownUntilEverythingHoldingItLetsGo() {
    val log = ArrayList<String>()
    val c = ButtonCounter(4) { i, down -> log += "$i" + if (down) "+" else "-" }
    c.acquire(1)
    c.acquire(1) // a second holder: no second press
    c.release(1)
    c.release(1)
    c.release(1) // more letting go than holding: nothing
    c.acquire(2)
    c.acquire(3)
    c.releaseAll()
    c.releaseAll()
    assertEquals(listOf("1+", "1-", "2+", "3+", "2-", "3-"), log)
  }

  @Test
  fun turboIsDownForTheFirstHalfOfEveryPeriod() {
    assertTrue(Turbo.pressed(0, 100))
    assertTrue(Turbo.pressed(49, 100))
    assertFalse(Turbo.pressed(50, 100))
    assertFalse(Turbo.pressed(99, 100))
    assertTrue(Turbo.pressed(100, 100))
    assertTrue(Turbo.pressed(149, 100))
    assertFalse(Turbo.pressed(150, 100))
    assertTrue(Turbo.pressed(-5, 100))
    assertTrue(Turbo.pressed(7, 1))
  }

  /** A tap at (500, 500) on the right half lifting at upMs (having moved moveDp), then a touch at (x, y) at downMs. */
  private fun secondTap(
    upMs: Long,
    downMs: Long,
    x: Float = 510f,
    y: Float = 505f,
    side: Int = 1,
    armed: Boolean = true,
    moved: Float = 0f,
  ): Boolean {
    val d = DoubleTapDetector(220L, 30f, 300f)
    d.down(0, 0L, 500f, 500f, 1, true)
    if (moved > 0f) d.move(0, 500f + moved, 500f)
    d.up(0, upMs)
    return d.down(1, downMs, x, y, side, armed)
  }

  @Test
  fun aTouchSoonAfterATapIsADoubleTap() {
    assertTrue(secondTap(upMs = 80L, downMs = 250L))
    assertTrue("exactly at the interval", secondTap(upMs = 80L, downMs = 330L))
    assertFalse("a moment too late", secondTap(upMs = 80L, downMs = 331L))
  }

  @Test
  fun aSlowOrMovingTouchIsNotATap() {
    assertFalse("a touch that lasted 400 ms", secondTap(upMs = 400L, downMs = 500L))
    assertFalse("a swipe", secondTap(upMs = 80L, downMs = 250L, moved = 40f))
    assertTrue("a little wobble is still a tap", secondTap(upMs = 80L, downMs = 250L, moved = 20f))
  }

  @Test
  fun aDoubleTapIsOnOneHalfNearAndWanted() {
    assertFalse("the other half", secondTap(upMs = 80L, downMs = 250L, side = 0))
    assertFalse("too far away", secondTap(upMs = 80L, downMs = 250L, x = 900f, y = 500f))
    assertFalse("nothing mapped on that side", secondTap(upMs = 80L, downMs = 250L, armed = false))
  }

  @Test
  fun aDoubleTapIsOneTouchesThenANewOne() {
    val d = DoubleTapDetector(220L, 30f, 300f)
    d.down(0, 0L, 500f, 500f, 1, true)
    d.up(0, 80L)
    assertTrue(d.down(1, 250L, 505f, 500f, 1, true))
    d.up(1, 300L) // the held second tap lifting is not a first tap...
    assertFalse(d.down(2, 400L, 505f, 500f, 1, true))
    d.up(2, 450L) // ...but a touch after it that is a tap can start another double tap
    assertTrue(d.down(3, 600L, 505f, 500f, 1, true))
    val e = DoubleTapDetector(220L, 30f, 300f)
    e.down(0, 0L, 500f, 500f, 1, true)
    e.up(0, 80L)
    e.cancel()
    assertFalse("cancelled in between", e.down(1, 250L, 505f, 500f, 1, true))
    val f = DoubleTapDetector(220L, 30f, 300f)
    f.down(0, 0L, 500f, 500f, 1, true)
    f.up(0, 80L)
    f.down(1, 120L, 900f, 100f, 0, true) // a touch elsewhere makes the tap stale
    f.up(1, 150L)
    assertFalse(f.down(2, 260L, 505f, 500f, 1, true))
  }

  @Test
  fun aMappingKeepsTwoValidButtons() {
    assertEquals(listOf("A", "RIGHT_TRIGGER"), TouchLookConfig.parseTargets("A, RIGHT_TRIGGER ,B"))
    assertEquals(listOf("A"), TouchLookConfig.parseTargets("A,A,nonsense,"))
    assertEquals(emptyList<String>(), TouchLookConfig.parseTargets(""))
    assertEquals("X", TouchLookConfig.formatTargets(listOf("", "X")))
    assertEquals("A", TouchLookConfig.formatTargets(listOf("A", "A")))
    val c = TouchLookConfig().withTargets(2, listOf("B", "LEFT_TRIGGER"))
    assertEquals(listOf("B", "LEFT_TRIGGER"), c.targets(2))
    assertEquals(emptyList<String>(), c.targets(0))
    assertEquals(emptyList<String>(), c.targets(4))
  }

  @Test
  fun turboIsSetPerSource() {
    val c = TouchLookConfig().withTurbo(1, true).withTurbo(4, true)
    assertTrue(c.hasTurbo(1) && c.hasTurbo(4))
    assertFalse(c.hasTurbo(0) || c.hasTurbo(2) || c.hasTurbo(3))
    val d = c.withTurbo(1, false)
    assertFalse(d.hasTurbo(1))
    assertTrue(d.hasTurbo(4))
  }

  private companion object {
    /** One pixel in dp (a screen density of 2.6). */
    const val PX = 1.0 / 2.6
  }
}
