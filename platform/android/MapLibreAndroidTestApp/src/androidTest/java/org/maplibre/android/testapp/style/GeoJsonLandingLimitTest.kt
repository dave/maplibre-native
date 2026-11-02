package org.maplibre.android.testapp.style

import androidx.test.internal.runner.junit4.AndroidJUnit4ClassRunner
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Test
import org.junit.runner.RunWith
import org.maplibre.android.style.sources.GeoJsonOptions
import org.maplibre.android.style.sources.GeoJsonSource
import org.maplibre.android.testapp.action.MapLibreMapAction
import org.maplibre.android.testapp.activity.EspressoTest

/**
 * [GeoJsonSource.landingLimit] over core `GeoJSONSource::setLandingLimit`: the value goes to native
 * code and is read back from it. What the limit holds is tested in core.
 */
@RunWith(AndroidJUnit4ClassRunner::class)
class GeoJsonLandingLimitTest : EspressoTest() {

    @Test
    fun theLandingLimitIsNoneByDefaultAndRoundTrips() {
        validateTestSetup()
        MapLibreMapAction.invoke(maplibreMap) { _, map ->
            val source = GeoJsonSource("labels", GeoJsonOptions().withLabelLane(true))
            map.style!!.addSource(source)
            assertNull(source.landingLimit)

            source.landingLimit = 3
            assertEquals(3L, source.landingLimit)

            source.landingLimit = 0
            assertEquals(0L, source.landingLimit)

            source.landingLimit = null
            assertNull(source.landingLimit)
        }
    }

    @Test
    fun aNegativeLandingLimitIsRefusedAndTheLimitIsKept() {
        validateTestSetup()
        MapLibreMapAction.invoke(maplibreMap) { _, map ->
            val source = GeoJsonSource("labels", GeoJsonOptions().withLabelLane(true))
            map.style!!.addSource(source)
            source.landingLimit = 2

            assertThrows(IllegalArgumentException::class.java) { source.landingLimit = -1 }

            assertEquals(2L, source.landingLimit)
        }
    }
}
