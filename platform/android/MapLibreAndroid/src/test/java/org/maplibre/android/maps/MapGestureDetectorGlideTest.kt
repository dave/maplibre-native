package org.maplibre.android.maps

import android.graphics.PointF
import android.os.Looper
import android.view.MotionEvent
import org.robolectric.RuntimeEnvironment
import io.mockk.every
import io.mockk.mockk
import io.mockk.slot
import io.mockk.verify
import io.mockk.verifyOrder
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
 * report the camera the glide ends at; a rotate glide reports nothing, and neither glide is
 * reported when two start at one lift (they cut each other short).
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

    private data class Glide(val target: CameraPosition, val durationMs: Long, val kind: MapLibreMap.GlideKind)

    private val glides = mutableListOf<Glide>()
    private val panTarget = CameraPosition.Builder().target(LatLng(1.0, 2.0)).zoom(12.3).build()
    private val zoomTarget = CameraPosition.Builder().target(LatLng(3.0, 4.0)).zoom(13.3).build()

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
        every { transform.rawZoom } returns 12.3
        every { transform.tilt } returns 0.0
        every { transform.getCameraForMoveBy(any(), any(), any()) } returns panTarget
        every { transform.getCameraForZoomAround(any(), any()) } returns zoomTarget
        every { gesturesManager.setStandardGestureListener(capture(standard)) } returns Unit
        every { gesturesManager.setStandardScaleGestureListener(capture(scale)) } returns Unit
        every { gesturesManager.setRotateGestureListener(capture(rotate)) } returns Unit
        every { gesturesManager.setMultiFingerTapGestureListener(capture(tap)) } returns Unit
        every { gesturesManager.onTouchEvent(any()) } returns false

        detector = MapGestureDetector(null, transform, mockk(relaxed = true), uiSettings,
            mockk(relaxed = true), mockk(relaxed = true))
        detector.setGesturesManager(context, gesturesManager, true, false)
        detector.addOnCameraGlideListener { target, durationMs, kind -> glides.add(Glide(target, durationMs, kind)) }
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
    fun aFlingReportsThePanEndBeforeItMoves() {
        fling()

        assertEquals(1, glides.size)
        assertEquals(MapLibreMap.GlideKind.PAN, glides[0].kind)
        assertEquals(panTarget, glides[0].target)
        val dx = slot<Double>()
        val dy = slot<Double>()
        val ms = slot<Long>()
        verify { transform.moveBy(capture(dx), capture(dy), capture(ms)) }
        verifyOrder {
            transform.getCameraForMoveBy(dx.captured, dy.captured, ms.captured)
            transform.moveBy(dx.captured, dy.captured, ms.captured)
        }
        assertEquals(ms.captured, glides[0].durationMs)
    }

    private fun assertZoomGlideEndsAtTheLastFrame(expectedFocal: PointF, expectedDuration: Long?) {
        assertEquals(1, glides.size)
        assertEquals(MapLibreMap.GlideKind.ZOOM, glides[0].kind)
        assertEquals(zoomTarget, glides[0].target)
        val reportedZoom = slot<Double>()
        val reportedFocal = slot<PointF>()
        verify { transform.getCameraForZoomAround(capture(reportedZoom), capture(reportedFocal)) }
        assertEquals(expectedFocal, reportedFocal.captured)
        if (expectedDuration != null) assertEquals(expectedDuration, glides[0].durationMs)

        // The animator's last frame sets exactly the reported zoom, about the reported point.
        val frames = mutableListOf<Double>()
        val focals = mutableListOf<PointF>()
        runAnimations()
        verify { transform.setZoom(capture(frames), capture(focals)) }
        assertTrue("the animation ran", frames.size > 1)
        assertEquals(reportedZoom.captured, frames.last(), 0.0)
        assertTrue(focals.all { it == reportedFocal.captured })
    }

    @Test
    fun aDoubleTapReportsTheZoomEnd() {
        val down = MotionEvent.obtain(0L, 0L, MotionEvent.ACTION_DOWN, 300f, 500f, 0)
        val up = MotionEvent.obtain(0L, 10L, MotionEvent.ACTION_UP, 300f, 500f, 0)
        standard.captured.onDoubleTapEvent(down)
        assertTrue(standard.captured.onDoubleTapEvent(up))
        assertEquals(0, glides.size) // scheduled, starts at the lift
        detector.onTouchEvent(up)

        assertZoomGlideEndsAtTheLastFrame(PointF(300f, 500f), MapLibreConstants.ANIMATION_DURATION.toLong())
    }

    @Test
    fun aTwoFingerTapReportsTheZoomEnd() {
        val tapDetector = mockk<MultiFingerTapGestureDetector>(relaxed = true)
        every { tapDetector.focalPoint } returns PointF(200f, 700f)
        assertTrue(tap.captured.onMultiFingerTap(tapDetector, 2))
        touch(MotionEvent.ACTION_UP)

        assertZoomGlideEndsAtTheLastFrame(PointF(200f, 700f), MapLibreConstants.ANIMATION_DURATION.toLong())
    }

    @Test
    fun aScaleFlingReportsTheZoomEnd() {
        scaleFling()
        assertEquals(0, glides.size)
        touch(MotionEvent.ACTION_UP)

        assertZoomGlideEndsAtTheLastFrame(PointF(540f, 1000f), null)
    }

    @Test
    fun aRotateFlingReportsNothing() {
        rotateFling()
        touch(MotionEvent.ACTION_UP)
        runAnimations()

        verify { transform.setBearing(any(), any(), any(), any()) } // the rotate glide ran
        assertEquals(0, glides.size)
    }

    @Test
    fun aScaleAndARotateFlingAtOneLiftReportNothing() {
        scaleFling()
        rotateFling()
        touch(MotionEvent.ACTION_UP)

        assertEquals(0, glides.size)
    }

    @Test
    fun aPanFlingAndAScaleFlingAtOneLiftReportNothing() {
        scaleFling()
        fling()
        touch(MotionEvent.ACTION_UP)

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
