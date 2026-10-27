package org.maplibre.android.testapp.render

import android.opengl.GLES20
import android.util.Log
import androidx.test.internal.runner.junit4.AndroidJUnit4ClassRunner
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.rule.ActivityTestRule
import com.google.gson.JsonObject
import org.junit.After
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.maplibre.android.camera.CameraPosition
import org.maplibre.android.geometry.LatLng
import org.maplibre.android.maps.MapLibreMap
import org.maplibre.android.maps.MapView
import org.maplibre.android.maps.Style
import org.maplibre.android.maps.renderer.MapRenderer
import org.maplibre.android.style.expressions.Expression
import org.maplibre.android.style.layers.FillLayer
import org.maplibre.android.style.layers.PropertyFactory
import org.maplibre.android.style.layers.TransitionOptions
import org.maplibre.android.style.sources.GeoJsonOptions
import org.maplibre.android.style.sources.GeoJsonSource
import org.maplibre.android.testapp.activity.turnprobe.TurnProbeActivity
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * PROBE (throwaway branch probe/android-turn-atomicity, task T4 of
 * one-frame-by-construction-plan.md). Not a regression test.
 *
 * Question: do the map writes of ONE main-thread task reach the screen in
 * ONE rendered frame on Android (TextureView, OpenGL)?
 *
 * The map shows 8 vertical bands. Each band is blue ("off") or red ("on"),
 * and each band is changed by a different kind of write:
 *   0-2  fill-color paint change (one layer each)
 *   3-4  filter change on an overlay layer (one layer each)
 *   5    setGeoJson on a synchronous GeoJSON source (data-driven colour)
 *   6    setFeatureState on a GeoJSON source (feature-state colour)
 *   7    setGeoJson on an ASYNCHRONOUS GeoJSON source: positive control,
 *        known to land in a later main-loop task, so the detector must see
 *        it split from the rest.
 * One main-thread task flips all 8 bands. After every nativeRender, on the
 * render thread and before the swap, the probe reads one pixel row through
 * the band centres. A frame is PARTIAL when bands 0-6 are mixed (some at
 * the new colour, some at the old one).
 */
@RunWith(AndroidJUnit4ClassRunner::class)
class TurnAtomicityProbeTest {

    @get:Rule
    val rule = ActivityTestRule(TurnProbeActivity::class.java)

    private class Frame(val t: Long, val on: Int, val off: Int)

    private val frames = ArrayList<Frame>(200_000)
    private val lock = Object()

    @Volatile private var targetOn = false
    @Volatile private var completeLatch: CountDownLatch? = null
    @Volatile private var busy = false
    @Volatile private var spinNs = 0L
    private var heavy = false
    private val heavyBodies = HashMap<Boolean, String>()

    /** Band 5 plus HEAVY_POINTS points off screen: a sync body that takes many ms to parse in the turn. */
    private fun heavyBody(on: Boolean): String = heavyBodies.getOrPut(on) {
        val band = bands(5..5, on)
        val sb = StringBuilder(band.substring(0, band.length - 2))
        for (k in 0 until HEAVY_POINTS) {
            val x = 60.0 + (k % 300) * 0.1
            val y = -30.0 + (k / 300) * 0.1
            sb.append(",{\"type\":\"Feature\",\"properties\":{},\"geometry\":{\"type\":\"Point\",\"coordinates\":[$x,$y]}}")
        }
        sb.append("]}")
        sb.toString()
    }

    private var idleCount = 0L
    private val idleLock = Object()

    private lateinit var xs: IntArray
    private var glY = 0
    private var width = 0
    private lateinit var row: ByteBuffer

    private lateinit var mapView: MapView
    private lateinit var map: MapLibreMap

    private val args = InstrumentationRegistry.getArguments()

    @After
    fun tearDown() {
        MapRenderer.frameProbe = null
    }

    @Test
    fun oneTurnOneFrame() {
        val runs = (args.getString("runs") ?: "200").toInt()
        val arms = (args.getString("arms") ?: "idle-fsLast,idle-fsFirst,busy-fsLast,busy-fsFirst,slow-fsLast,slow-fsFirst")
            .split(",")
        setUpMap()
        installProbe()

        val out = File(rule.activity.getExternalFilesDir(null), "turnprobe")
        out.mkdirs()
        val summary = StringBuilder()
        for (arm in arms) {
            val line = runArm(arm, runs, File(out, "$arm.csv"))
            Log.i(TAG, "SUMMARY $line")
            summary.append(line).append('\n')
        }
        File(out, "summary.txt").writeText(summary.toString())
    }

    // MARK: - set-up

    private fun setUpMap() {
        val styleLatch = CountDownLatch(1)
        rule.runOnUiThread {
            mapView = rule.activity.mapView
            mapView.addOnDidBecomeIdleListener {
                synchronized(idleLock) {
                    idleCount++
                    idleLock.notifyAll()
                }
            }
            mapView.getMapAsync { m ->
                map = m
                m.cameraPosition = CameraPosition.Builder().target(LatLng(0.0, 3.95)).zoom(5.0).build()
                m.uiSettings.setAllGesturesEnabled(false)
                m.setStyle(Style.Builder().fromJson(STYLE)) { style ->
                    style.transition = TransitionOptions(0, 0, false)
                    val sync = GeoJsonOptions().withSynchronousUpdate(true)
                    style.addSource(GeoJsonSource("st", bands(0..4, false), sync))
                    style.addSource(GeoJsonSource("g", bands(5..5, false), sync))
                    style.addSource(GeoJsonSource("fs", bands(6..6, false), sync))
                    style.addSource(GeoJsonSource("ga", bands(7..7, false)))
                    for (i in 0..4) {
                        style.addLayer(
                            FillLayer("base$i", "st")
                                .withFilter(Expression.raw("[\"==\",[\"get\",\"b\"],$i]"))
                                .withProperties(PropertyFactory.fillColor(OFF), PropertyFactory.fillAntialias(false))
                        )
                    }
                    for (i in 3..4) {
                        style.addLayer(
                            FillLayer("over$i", "st")
                                .withFilter(Expression.raw(filterFor(i, false)))
                                .withProperties(PropertyFactory.fillColor(ON), PropertyFactory.fillAntialias(false))
                        )
                    }
                    for (id in listOf("g", "ga")) {
                        style.addLayer(
                            FillLayer("data-$id", id).withProperties(
                                PropertyFactory.fillColor(
                                    Expression.raw("[\"case\",[\"==\",[\"get\",\"on\"],1],\"$ON\",\"$OFF\"]")
                                ),
                                PropertyFactory.fillAntialias(false)
                            )
                        )
                    }
                    style.addLayer(
                        FillLayer("state-fs", "fs").withProperties(
                            PropertyFactory.fillColor(
                                Expression.raw(
                                    "[\"case\",[\"boolean\",[\"feature-state\",\"on\"],false],\"$ON\",\"$OFF\"]"
                                )
                            ),
                            PropertyFactory.fillAntialias(false)
                        )
                    )
                    styleLatch.countDown()
                }
            }
        }
        assertTrue("style loaded", styleLatch.await(30, TimeUnit.SECONDS))

        rule.runOnUiThread {
            width = mapView.width
            val height = mapView.height
            xs = IntArray(BANDS) { i ->
                map.projection.toScreenLocation(LatLng(0.0, i + 0.45)).x.toInt()
            }
            val y = map.projection.toScreenLocation(LatLng(0.0, 0.45)).y.toInt()
            glY = height - 1 - y
            row = ByteBuffer.allocateDirect(width * 4).order(ByteOrder.nativeOrder())
            Log.i(TAG, "view ${width}x$height bandsX=${xs.joinToString()} y=$y")
        }
    }

    private fun installProbe() {
        val fb = IntArray(1)
        MapRenderer.frameProbe = MapRenderer.FrameProbe { renderer ->
            val t = System.nanoTime()
            GLES20.glGetIntegerv(GLES20.GL_FRAMEBUFFER_BINDING, fb, 0)
            if (fb[0] != 0) GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0)
            row.clear()
            GLES20.glReadPixels(0, glY, width, 1, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, row)
            if (fb[0] != 0) GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, fb[0])
            var on = 0
            var off = 0
            for (i in 0 until BANDS) {
                val o = xs[i] * 4
                val r = row.get(o).toInt() and 0xff
                val b = row.get(o + 2).toInt() and 0xff
                if (r > 180 && b < 80) on = on or (1 shl i)
                if (b > 180 && r < 80) off = off or (1 shl i)
            }
            synchronized(lock) { frames.add(Frame(t, on, off)) }
            val target = if (targetOn) on else off
            if (target == ALL) completeLatch?.countDown()
            val spin = spinNs
            if (spin > 0) {
                val end = System.nanoTime() + spin
                while (System.nanoTime() < end) { /* load generator for the slow arm */ }
            }
            if (busy) renderer.requestRender()
        }
    }

    // MARK: - one arm

    private fun runArm(arm: String, runs: Int, csv: File): String {
        val (mode, order) = arm.split("-")
        busy = mode == "busy" || mode == "slow" || mode == "heavyBusy"
        spinNs = if (mode == "slow") 12_000_000L else 0L
        heavy = mode.startsWith("heavy")
        if (heavy) {
            heavyBody(true)
            heavyBody(false)
        }
        val fsFirst = order == "fsFirst"
        val waitIdle = !busy

        // Settle to all-off, and check the detector reads it.
        flipAndWait(false, fsFirst, waitIdle)

        var partialRuns = 0
        var partialFrames = 0
        var controlSplitRuns = 0
        var timeouts = 0
        var duringTurnFrames = 0
        var fsLeadRuns = 0
        var fsLagRuns = 0
        val framesToMain = IntArray(8)
        val turnUs = ArrayList<Long>(runs)
        val gapUs = ArrayList<Long>(runs)
        val leadMasks = HashMap<Int, Int>()
        val w = csv.bufferedWriter()
        w.write("run,target,frame,t_us_from_turn_start,on_mask,off_mask\n")
        for (run in 1..runs) {
            val on = run % 2 == 1
            val r = flipAndWait(on, fsFirst, waitIdle)
            if (r == null) {
                timeouts++
                continue
            }
            turnUs.add((r.tEnd - r.tStart) / 1000)
            gapUs.add(r.gapBefore / 1000)
            var sawPartial = false
            var mainDone = false
            var nToMain = 0
            var leadMask = -1
            for ((k, f) in r.frames.withIndex()) {
                w.write("$run,${if (on) 1 else 0},$k,${(f.t - r.tStart) / 1000},${f.on},${f.off}\n")
                if (f.t <= r.tEnd) duringTurnFrames++
                val tgt = (if (on) f.on else f.off) and MAIN
                if (!mainDone) {
                    nToMain++
                    if (tgt != 0 && tgt != MAIN) {
                        sawPartial = true
                        partialFrames++
                        if (leadMask < 0) leadMask = tgt
                    }
                    if (tgt == MAIN) {
                        mainDone = true
                        val ctl = (if (on) f.on else f.off) and (1 shl 7)
                        if (ctl == 0) controlSplitRuns++
                    }
                }
            }
            if (sawPartial) {
                partialRuns++
                leadMasks[leadMask] = (leadMasks[leadMask] ?: 0) + 1
                if (leadMask == (1 shl 6)) fsLeadRuns++
                if (leadMask == (MAIN and (1 shl 6).inv())) fsLagRuns++
            }
            framesToMain[minOf(nToMain, 7)]++
        }
        w.close()
        turnUs.sort()
        gapUs.sort()
        fun pct(l: List<Long>, p: Double) = if (l.isEmpty()) -1 else l[((l.size - 1) * p).toInt()]
        return "arm=$arm runs=$runs timeouts=$timeouts partialRuns=$partialRuns partialFrames=$partialFrames " +
            "fsAloneFirst=$fsLeadRuns allButFsFirst=$fsLagRuns leadMasks=${leadMasks.entries.joinToString(";") { "0x" + Integer.toHexString(it.key) + ":" + it.value }} " +
            "controlSplitRuns=$controlSplitRuns framesDuringTurn=$duringTurnFrames " +
            "framesFromTurnToMainComplete(1..7+)=${framesToMain.drop(1).joinToString("/")} " +
            "turnUs(p50/p90/max)=${pct(turnUs, .5)}/${pct(turnUs, .9)}/${pct(turnUs, 1.0)} " +
            "gapSinceLastFrameUs(p10/p50/p90)=${pct(gapUs, .1)}/${pct(gapUs, .5)}/${pct(gapUs, .9)}"
    }

    private class RunResult(val tStart: Long, val tEnd: Long, val gapBefore: Long, val frames: List<Frame>)

    /** Flip all bands in one main-thread task; wait for a frame that shows all 8 at the target. */
    private fun flipAndWait(on: Boolean, fsFirst: Boolean, waitIdle: Boolean): RunResult? {
        val latch = CountDownLatch(1)
        targetOn = on
        val idleBefore = synchronized(idleLock) { idleCount }
        val firstIndex: Int
        var tStart = 0L
        var tEnd = 0L
        synchronized(lock) { firstIndex = frames.size }
        completeLatch = latch
        rule.runOnUiThread {
            val style = map.style!!
            tStart = System.nanoTime()
            if (fsFirst) featureState(style, on)
            for (i in 0..2) {
                style.getLayerAs<FillLayer>("base$i")!!.setProperties(PropertyFactory.fillColor(if (on) ON else OFF))
            }
            for (i in 3..4) {
                style.getLayerAs<FillLayer>("over$i")!!.setFilter(Expression.raw(filterFor(i, on)))
            }
            style.getSourceAs<GeoJsonSource>("g")!!.setGeoJson(if (heavy) heavyBody(on) else bands(5..5, on))
            style.getSourceAs<GeoJsonSource>("ga")!!.setGeoJson(bands(7..7, on))
            if (!fsFirst) featureState(style, on)
            tEnd = System.nanoTime()
        }
        if (!latch.await(10, TimeUnit.SECONDS)) {
            completeLatch = null
            Log.w(TAG, "timeout waiting for target on=$on")
            return null
        }
        completeLatch = null
        if (waitIdle) {
            synchronized(idleLock) {
                val deadline = System.currentTimeMillis() + 10_000
                while (idleCount == idleBefore && System.currentTimeMillis() < deadline) {
                    idleLock.wait(10_000)
                }
            }
        }
        synchronized(lock) {
            var lastBefore = 0L
            val list = ArrayList<Frame>()
            for (k in maxOf(0, firstIndex - 1) until frames.size) {
                val f = frames[k]
                if (f.t < tStart) lastBefore = f.t else list.add(f)
            }
            // Keep only the frames up to the first that showed all 8 bands at the target.
            val cut = list.indexOfFirst { (if (on) it.on else it.off) == ALL }
            val kept = if (cut >= 0) list.subList(0, cut + 1).toList() else list
            return RunResult(tStart, tEnd, if (lastBefore == 0L) -1 else tStart - lastBefore, kept)
        }
    }

    private fun featureState(style: Style, on: Boolean) {
        val s = JsonObject()
        s.addProperty("on", on)
        assertTrue(style.getSourceAs<GeoJsonSource>("fs")!!.setFeatureState("6", s))
    }

    companion object {
        const val TAG = "TURNPROBE"
        const val BANDS = 8
        const val HEAVY_POINTS = 30_000
        const val ALL = 0xff
        const val MAIN = 0x7f
        const val ON = "#FF0000"
        const val OFF = "#0000FF"
        const val STYLE = "{\"version\":8,\"sources\":{},\"layers\":[" +
            "{\"id\":\"bg\",\"type\":\"background\",\"paint\":{\"background-color\":\"#000000\"}}]}"

        fun filterFor(i: Int, on: Boolean) = "[\"==\",[\"get\",\"b\"],${if (on) i else -1}]"

        fun bands(range: IntRange, on: Boolean): String {
            val fs = range.joinToString(",") { i ->
                val a = i.toDouble()
                val b = i + 0.9
                "{\"type\":\"Feature\",\"id\":$i,\"properties\":{\"b\":$i,\"on\":${if (on) 1 else 0}}," +
                    "\"geometry\":{\"type\":\"Polygon\",\"coordinates\":[[[$a,-3],[$b,-3],[$b,3],[$a,3],[$a,-3]]]}}"
            }
            return "{\"type\":\"FeatureCollection\",\"features\":[$fs]}"
        }
    }
}
