package org.maplibre.android.maps

import android.graphics.PointF
import io.mockk.every
import io.mockk.mockk
import io.mockk.slot
import org.junit.Assert.assertEquals
import org.junit.Test
import org.junit.runner.RunWith
import org.maplibre.android.camera.CameraPosition
import org.maplibre.android.geometry.LatLng
import org.robolectric.RobolectricTestRunner

/**
 * [Projection.getVisibleRegionAt] at the map's own camera gives the bounds
 * [Projection.getVisibleRegion] gives: the same screen points, read the same
 * way. The native side (the same conversion on a copy of the camera state)
 * is pinned by core's Map.LatLngsForPixelsAtCamera; here a native map that
 * answers both calls alike stands in for it.
 */
@RunWith(RobolectricTestRunner::class)
class ProjectionAtCameraTest {

    /** A made-up projection of a rotated, pitched view across the antimeridian: any fixed function will do. */
    private fun latLngAt(x: Double, y: Double) = LatLng(10.0 - y * 0.01 + x * 0.002, 179.0 + x * 0.01 + y * 0.003)

    @Test
    fun theRegionAtTheMapsOwnCameraIsGetVisibleRegions() {
        val native = mockk<NativeMap>(relaxed = true)
        val pixel = slot<PointF>()
        every { native.latLngForPixel(capture(pixel)) } answers { latLngAt(pixel.captured.x.toDouble(), pixel.captured.y.toDouble()) }
        val asked = mutableListOf<Double>()
        every { native.latLngsForPixelsAtCamera(any(), any(), any()) } answers {
            val input = secondArg<DoubleArray>()
            val output = thirdArg<DoubleArray>()
            asked += input.toList()
            for (i in input.indices step 2) {
                val ll = latLngAt(input[i], input[i + 1])
                output[i] = ll.latitude
                output[i + 1] = ll.longitude
            }
        }
        val mapView = mockk<MapView>(relaxed = true)
        every { mapView.width } returns 1080
        every { mapView.height } returns 2154
        val projection = Projection(native, mapView)

        val here = projection.getVisibleRegion()
        val camera = CameraPosition.Builder().target(LatLng(10.0, 179.5)).zoom(12.0).bearing(20.0).tilt(30.0).build()
        val atCamera = projection.getVisibleRegionAt(camera)

        assertEquals(listOf(540.0, 1077.0, 0.0, 0.0, 1080.0, 0.0, 1080.0, 2154.0, 0.0, 2154.0), asked)
        assertEquals(here.latLngBounds, atCamera.latLngBounds)
        assertEquals(here.farLeft, atCamera.farLeft)
        assertEquals(here.nearRight, atCamera.nearRight)
    }
}
