package org.maplibre.android.testapp.maps

import androidx.test.espresso.Espresso.onView
import androidx.test.espresso.UiController
import androidx.test.espresso.action.CoordinatesProvider
import androidx.test.espresso.action.GeneralLocation
import androidx.test.espresso.action.GeneralSwipeAction
import androidx.test.espresso.action.Press
import androidx.test.espresso.action.Swipe
import androidx.test.espresso.action.ViewActions.doubleClick
import androidx.test.espresso.matcher.ViewMatchers.withId
import androidx.test.internal.runner.junit4.AndroidJUnit4ClassRunner
import org.junit.Assert.assertEquals
import org.junit.Assert.fail
import org.junit.Test
import org.junit.runner.RunWith
import org.maplibre.android.camera.CameraPosition
import org.maplibre.android.camera.CameraUpdateFactory
import org.maplibre.android.geometry.LatLng
import org.maplibre.android.maps.MapLibreMap
import org.maplibre.android.testapp.R
import org.maplibre.android.testapp.action.MapLibreMapAction
import org.maplibre.android.testapp.activity.EspressoTest

/**
 * [MapLibreMap.OnCameraGlideListener] on a real map: a gesture's glide reports the camera that the
 * map then reaches. Each wait ends on an event; its bound only turns a missing event into a failure.
 */
@RunWith(AndroidJUnit4ClassRunner::class)
class CameraGlideTest : EspressoTest() {

    private val glides = mutableListOf<Pair<CameraPosition, MapLibreMap.GlideKind>>()
    private var idlesAfterAGlide = 0

    private fun listen(onGlide: (MapLibreMap) -> Unit = {}) {
        MapLibreMapAction.invoke(maplibreMap) { _, map ->
            map.moveCamera(CameraUpdateFactory.newLatLngZoom(LatLng(46.5, 8.0), 9.0))
            map.addOnCameraGlideListener { target, _, kind ->
                glides += target to kind
                onGlide(map)
            }
            // The map is idle once no camera move runs and every frame is drawn: a move that a
            // listener's camera change did not stop is still running before that.
            mapView.addOnDidBecomeIdleListener {
                if (glides.isNotEmpty()) idlesAfterAGlide++
            }
        }
    }

    /** Waits for the map to go idle after a glide was reported, and returns the camera then. */
    private fun cameraOnceTheGlideEnds(): CameraPosition {
        lateinit var camera: CameraPosition
        MapLibreMapAction.invoke(maplibreMap) { uiController, map ->
            waitFor(uiController, "the glide reported and ended") { idlesAfterAGlide > 0 }
            camera = map.cameraPosition
        }
        return camera
    }

    @Test
    fun aDoubleTapReportsTheCameraItsZoomEndsAt() {
        validateTestSetup()
        listen()
        onView(withId(R.id.mapView)).perform(doubleClick())

        val end = cameraOnceTheGlideEnds()

        assertEquals(1, glides.size)
        assertEquals(MapLibreMap.GlideKind.ZOOM, glides[0].second)
        assertCameraEquals(glides[0].first, end)
    }

    @Test
    fun aFlingReportsTheCameraItsMoveEndsAt() {
        validateTestSetup()
        listen()
        onView(withId(R.id.mapView)).perform(fling())

        val end = cameraOnceTheGlideEnds()

        assertEquals(1, glides.size)
        assertEquals(MapLibreMap.GlideKind.PAN, glides[0].second)
        assertCameraEquals(glides[0].first, end)
    }

    @Test
    fun aListenerThatMovesTheCameraOnAFlingLeavesItWhereTheListenerPutIt() {
        validateTestSetup()
        val elsewhere = LatLng(-30.0, 40.0)
        listen { map -> map.moveCamera(CameraUpdateFactory.newLatLngZoom(elsewhere, 5.0)) }
        onView(withId(R.id.mapView)).perform(fling())

        val end = cameraOnceTheGlideEnds()

        assertEquals(1, glides.size)
        assertEquals(elsewhere.latitude, end.target!!.latitude, DELTA_DEGREES)
        assertEquals(elsewhere.longitude, end.target!!.longitude, DELTA_DEGREES)
        assertEquals(5.0, end.zoom, 1e-9)
    }

    /** A fast swipe from the view's centre to its left edge: a pan fling. */
    private fun fling() = GeneralSwipeAction(
        Swipe.FAST,
        GeneralLocation.CENTER,
        CoordinatesProvider { view ->
            val xy = IntArray(2).also { view.getLocationOnScreen(it) }
            floatArrayOf(xy[0] + view.width * 0.05f, xy[1] + view.height * 0.5f)
        },
        Press.FINGER
    )

    private fun assertCameraEquals(expected: CameraPosition, actual: CameraPosition) {
        assertEquals(expected.target!!.latitude, actual.target!!.latitude, DELTA_DEGREES)
        assertEquals(expected.target!!.longitude, actual.target!!.longitude, DELTA_DEGREES)
        assertEquals(expected.zoom, actual.zoom, 1e-9)
        assertEquals(expected.bearing, actual.bearing, 1e-9)
        assertEquals(expected.tilt, actual.tilt, 1e-9)
    }

    private fun waitFor(uiController: UiController, what: String, done: () -> Boolean) {
        val deadline = System.nanoTime() / 1_000_000L + BOUND_MS
        while (!done()) {
            if (System.nanoTime() / 1_000_000L > deadline) {
                fail("$what: not within $BOUND_MS ms; glides reported: $glides")
            }
            uiController.loopMainThreadForAtLeast(50)
        }
    }

    companion object {
        private const val BOUND_MS = 30_000L

        /** About 1 cm on the ground: rounding in the frames, not a different camera. */
        private const val DELTA_DEGREES = 1e-7
    }
}
