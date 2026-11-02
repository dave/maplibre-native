package org.maplibre.android.maps

import android.graphics.PointF
import android.os.Looper
import android.view.MotionEvent
import org.robolectric.RuntimeEnvironment
import io.mockk.every
import io.mockk.mockk
import io.mockk.slot
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.maplibre.android.R
import org.maplibre.android.camera.CameraPosition
import org.maplibre.android.constants.MapLibreConstants
import org.maplibre.android.gestures.AndroidGesturesManager
import org.maplibre.android.gestures.MultiFingerTapGestureDetector
import org.maplibre.android.gestures.RotateGestureDetector
import org.maplibre.android.gestures.StandardGestureDetector
import org.maplibre.android.gestures.StandardScaleGestureDetector
import org.maplibre.android.geometry.LatLng
import org.robolectric.RobolectricTestRunner
import org.robolectric.Shadows.shadowOf
import java.time.Duration

/**
 * The camera glide report: a pan fling, a scale fling, a double tap and a two-finger tap each
 * report the camera the glide ends at, once the glide has started; a rotate glide reports nothing,
 * and neither glide is reported when two start at one lift (they cut each other short).
 *
 * The transform here keeps a camera: a move or a zoom changes it, as the map's would, so each
 * test checks the camera a report names against the camera the map reaches.
 */
@RunWith(RobolectricTestRunner::class)
class MapGestureDetectorGlideTest {

    private val context = RuntimeEnvironment.getApplication()
    private val transform = mockk<Transform>(relaxed = true)
    private val uiSettings = mockk<UiSettings>(relaxed = true)
    private val gesturesManager = mockk<AndroidGesturesManager>(relaxed = true)
    private val standard = slot<StandardGestureDetector.StandardOnGestureListener>()
    private val scale = slot<StandardScaleGestureDetector.StandardOnScaleGestureListener>()
    private val rotate = slot<RotateGestureDetector.OnRotateGestureListener>()
    private val tap = slot<MultiFingerTapGestureDetector.OnMultiFingerTapGestureListener>()
    private lateinit var detector: MapGestureDetector

    /** The map's camera. */
    private var camera = CameraPosition.Builder().target(LatLng(10.0, 20.0)).zoom(12.3).build()

    /** True while a moveBy runs; a camera jump stops it, as on the map. */
    private var moving = false

    /** A report, with the state the map was in when the listener ran. */
    private data class Glide(
        val target: CameraPosition,
        val durationMs: Long,
        val kind: MapLibreMap.GlideKind,
        val moving: Boolean
    )

    private val glides = mutableListOf<Glide>()

    /** What the listener does to the camera, if anything. */
    private var onGlide: (() -> Unit)? = null

    /** A made-up projection: one screen pixel is a millionth of a degree. */
    private fun movedBy(from: CameraPosition, dx: Double, dy: Double): CameraPosition =
        CameraPosition.Builder(from).target(LatLng(from.target!!.latitude + dy * 1e-6, from.target!!.longitude - dx * 1e-6))
            .build()

    private fun zoomedTo(from: CameraPosition, zoom: Double): CameraPosition = CameraPosition.Builder(from).zoom(zoom).build()

    @Before
    fun setUp() {
        every { uiSettings.isScrollGesturesEnabled } returns true
        every { uiSettings.isFlingVelocityAnimationEnabled } returns true
        every { uiSettings.isHorizontalScrollGesturesEnabled } returns true
        every { uiSettings.isZoomGesturesEnabled } returns true
        every { uiSettings.isDoubleTapGesturesEnabled } returns true
        every { uiSettings.isQuickZoomGesturesEnabled } returns true
        every { uiSettings.isScaleVelocityAnimationEnabled } returns true
        every { uiSettings.isRotateVelocityAnimationEnabled } returns true
        every { uiSettings.isIncreaseScaleThresholdWhenRotating } returns false
        every { uiSettings.pixelRatio } returns 2f
        every { uiSettings.flingThreshold } returns 1000L
        every { uiSettings.flingAnimationBaseTime } returns 150L
        every { uiSettings.width } returns 1080f
        every { uiSettings.height } returns 2000f
        every { uiSettings.zoomRate } returns 1f
        every { transform.rawZoom } answers { camera.zoom }
        every { transform.tilt } returns 0.0
        every { transform.getCameraForMoveBy(any(), any(), any()) } answers { movedBy(camera, firstArg(), secondArg()) }
        every { transform.moveBy(any(), any(), any()) } answers {
            moving = true
            camera = movedBy(camera, firstArg(), secondArg())
        }
        every { transform.getCameraForZoomAround(any(), any()) } answers { zoomedTo(camera, firstArg()) }
        every { transform.setZoom(any(), any()) } answers { camera = zoomedTo(camera, firstArg()) }
        every { gesturesManager.setStandardGestureListener(capture(standard)) } returns Unit
        every { gesturesManager.setStandardScaleGestureListener(capture(scale)) } returns Unit
        every { gesturesManager.setRotateGestureListener(capture(rotate)) } returns Unit
        every { gesturesManager.setMultiFingerTapGestureListener(capture(tap)) } returns Unit
        every { gesturesManager.onTouchEvent(any()) } returns false

        detector = MapGestureDetector(null, transform, mockk(relaxed = true), uiSettings,
            mockk(relaxed = true), mockk(relaxed = true))
        detector.setGesturesManager(context, gesturesManager, true, false)
        detector.addOnCameraGlideListener { target, durationMs, kind ->
            glides.add(Glide(target, durationMs, kind, moving))
            onGlide?.invoke()
        }
    }

    private fun touch(action: Int, x: Float = 300f, y: Float = 500f) {
        val event = MotionEvent.obtain(0L, 0L, action, x, y, 0)
        detector.onTouchEvent(event)
        event.recycle()
    }

    private fun fling() {
        val down = MotionEvent.obtain(0L, 0L, MotionEvent.ACTION_DOWN, 300f, 500f, 0)
        val up = MotionEvent.obtain(0L, 50L, MotionEvent.ACTION_UP, 600f, 900f, 0)
        assertTrue(standard.captured.onFling(down, up, 3000f, -4000f))
    }

    private fun scaleFling() {
        val scaleDetector = mockk<StandardScaleGestureDetector>(relaxed = true)
        every { scaleDetector.pointersCount } returns 1 // quick zoom: focal point is the map centre
        every { scaleDetector.currentSpan } returns 600f
        every { scaleDetector.previousSpan } returns 100f
        every { scaleDetector.isScalingOut } returns false
        assertTrue(scale.captured.onScaleBegin(scaleDetector))
        val minimum = context.resources.getDimension(R.dimen.maplibre_minimum_scale_velocity)
        scale.captured.onScaleEnd(scaleDetector, minimum, minimum)
    }

    private fun rotateFling() {
        val rotateDetector = mockk<RotateGestureDetector>(relaxed = true)
        every { rotateDetector.focalPoint } returns PointF(400f, 400f)
        every { rotateDetector.deltaSinceLast } returns 5f
        rotate.captured.onRotateEnd(rotateDetector, 100f, 100f, 1000f)
    }

    /** Runs the started animators to their last frame. */
    private fun runAnimations() {
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofSeconds(5))
    }

    @Test
    fun aFlingReportsTheCameraItsMoveEndsAtOnceTheMoveHasStarted() {
        val before = camera
        fling()
        assertEquals("reported before the lift ends", 0, glides.size)
        touch(MotionEvent.ACTION_UP)

        assertEquals(1, glides.size)
        assertEquals(MapLibreMap.GlideKind.PAN, glides[0].kind)
        assertTrue("the move had started when the listener ran", glides[0].moving)
        assertEquals(camera, glides[0].target)
        assertTrue("the fling moved the camera", camera != before)
        assertTrue(glides[0].durationMs > 0)
    }

    @Test
    fun aListenerThatMovesTheCameraOnAFlingLeavesItWhereTheListenerPutIt() {
        val elsewhere = CameraPosition.Builder().target(LatLng(-30.0, 40.0)).zoom(5.0).build()
        // A camera jump from the listener, as moveCamera makes: it stops the running move.
        onGlide = {
            camera = elsewhere
            moving = false
        }
        fling()
        touch(MotionEvent.ACTION_UP)

        assertEquals(1, glides.size)
        assertEquals(elsewhere, camera)
    }

    private fun assertZoomGlideEndsAtTheLastFrame(expectedDuration: Long?) {
        assertEquals(1, glides.size)
        assertEquals(MapLibreMap.GlideKind.ZOOM, glides[0].kind)
        if (expectedDuration != null) assertEquals(expectedDuration, glides[0].durationMs)
        val reported = glides[0].target
        assertTrue("the zoom is not yet at the end when reported", camera.zoom != reported.zoom)

        runAnimations()

        // The animator's last frame sets exactly the reported camera.
        assertEquals(reported, camera)
    }

    @Test
    fun aDoubleTapReportsTheZoomEnd() {
        val down = MotionEvent.obtain(0L, 0L, MotionEvent.ACTION_DOWN, 300f, 500f, 0)
        val up = MotionEvent.obtain(0L, 10L, MotionEvent.ACTION_UP, 300f, 500f, 0)
        standard.captured.onDoubleTapEvent(down)
        assertTrue(standard.captured.onDoubleTapEvent(up))
        assertEquals(0, glides.size) // scheduled, starts at the lift
        detector.onTouchEvent(up)

        assertZoomGlideEndsAtTheLastFrame(MapLibreConstants.ANIMATION_DURATION.toLong())
    }

    @Test
    fun aTwoFingerTapReportsTheZoomEnd() {
        val tapDetector = mockk<MultiFingerTapGestureDetector>(relaxed = true)
        every { tapDetector.focalPoint } returns PointF(200f, 700f)
        assertTrue(tap.captured.onMultiFingerTap(tapDetector, 2))
        touch(MotionEvent.ACTION_UP)

        assertZoomGlideEndsAtTheLastFrame(MapLibreConstants.ANIMATION_DURATION.toLong())
    }

    @Test
    fun aScaleFlingReportsTheZoomEnd() {
        scaleFling()
        assertEquals(0, glides.size)
        touch(MotionEvent.ACTION_UP)

        assertZoomGlideEndsAtTheLastFrame(null)
    }

    @Test
    fun aRotateFlingReportsNothing() {
        rotateFling()
        touch(MotionEvent.ACTION_UP)
        runAnimations()

        assertEquals(0, glides.size)
    }

    @Test
    fun aScaleAndARotateFlingAtOneLiftReportNothing() {
        scaleFling()
        rotateFling()
        touch(MotionEvent.ACTION_UP)
        runAnimations()

        assertEquals(0, glides.size)
    }

    @Test
    fun aScaleFlingThenAPanFlingAtOneLiftReportNothing() {
        scaleFling()
        fling()
        touch(MotionEvent.ACTION_UP)
        runAnimations()

        assertEquals(0, glides.size)
    }

    @Test
    fun aPanFlingThenAScaleFlingAtOneLiftReportNothing() {
        fling()
        scaleFling()
        touch(MotionEvent.ACTION_UP)
        runAnimations()

        assertEquals(0, glides.size)
    }

    @Test
    fun aCancelledLiftReportsNothing() {
        fling()
        touch(MotionEvent.ACTION_CANCEL)

        assertEquals(0, glides.size)
    }

    @Test
    fun aDestroyedMapReportsNothing() {
        // The transform answers no camera once the map is destroyed.
        every { transform.getCameraForMoveBy(any(), any(), any()) } returns null
        every { transform.getCameraForZoomAround(any(), any()) } returns null
        fling()
        touch(MotionEvent.ACTION_UP)
        val tapDetector = mockk<MultiFingerTapGestureDetector>(relaxed = true)
        every { tapDetector.focalPoint } returns PointF(200f, 700f)
        tap.captured.onMultiFingerTap(tapDetector, 2)
        touch(MotionEvent.ACTION_UP)
        runAnimations()

        assertEquals(0, glides.size)
    }

    @Test
    fun aDoubleTapAfterAnEarlierFlingIsReported() {
        fling()
        touch(MotionEvent.ACTION_UP)
        glides.clear()

        touch(MotionEvent.ACTION_DOWN)
        val tapDetector = mockk<MultiFingerTapGestureDetector>(relaxed = true)
        every { tapDetector.focalPoint } returns PointF(200f, 700f)
        tap.captured.onMultiFingerTap(tapDetector, 2)
        touch(MotionEvent.ACTION_UP)

        assertEquals(listOf(MapLibreMap.GlideKind.ZOOM), glides.map { it.kind })
    }
}
