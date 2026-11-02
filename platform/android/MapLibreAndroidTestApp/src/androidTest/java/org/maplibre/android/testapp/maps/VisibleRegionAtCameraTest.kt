package org.maplibre.android.testapp.maps

import androidx.test.internal.runner.junit4.AndroidJUnit4ClassRunner
import org.junit.Assert.assertEquals
import org.junit.Test
import org.junit.runner.RunWith
import org.maplibre.android.camera.CameraPosition
import org.maplibre.android.camera.CameraUpdateFactory
import org.maplibre.android.geometry.LatLng
import org.maplibre.android.geometry.VisibleRegion
import org.maplibre.android.testapp.action.MapLibreMapAction
import org.maplibre.android.testapp.activity.EspressoTest

/**
 * [org.maplibre.android.maps.Projection.getVisibleRegionAt] on a real map: the camera goes to
 * native code, and the region comes back from it.
 */
@RunWith(AndroidJUnit4ClassRunner::class)
class VisibleRegionAtCameraTest : EspressoTest() {

    @Test
    fun aCameraWithOnlyATargetKeepsTheMapsZoomBearingAndTilt() {
        validateTestSetup()
        MapLibreMapAction.invoke(maplibreMap) { _, map ->
            map.moveCamera(
                CameraUpdateFactory.newCameraPosition(
                    CameraPosition.Builder().target(LatLng(46.5, 8.0)).zoom(9.0).bearing(30.0).tilt(40.0).build()
                )
            )
            val here = map.projection.visibleRegion

            // Zoom, bearing and tilt are left at the builder's default (-1).
            val targetOnly = CameraPosition.Builder().target(map.cameraPosition.target).build()
            val atCamera = map.projection.getVisibleRegionAt(targetOnly)

            assertRegionEquals(here, atCamera)
        }
    }

    private fun assertRegionEquals(expected: VisibleRegion, actual: VisibleRegion) {
        assertLatLngEquals(expected.farLeft!!, actual.farLeft!!)
        assertLatLngEquals(expected.farRight!!, actual.farRight!!)
        assertLatLngEquals(expected.nearLeft!!, actual.nearLeft!!)
        assertLatLngEquals(expected.nearRight!!, actual.nearRight!!)
    }

    private fun assertLatLngEquals(expected: LatLng, actual: LatLng) {
        assertEquals(expected.latitude, actual.latitude, DELTA_DEGREES)
        assertEquals(expected.longitude, actual.longitude, DELTA_DEGREES)
    }

    companion object {
        /** About 0.1 mm on the ground: a degree to radian round trip, not a different camera. */
        private const val DELTA_DEGREES = 1e-9
    }
}
