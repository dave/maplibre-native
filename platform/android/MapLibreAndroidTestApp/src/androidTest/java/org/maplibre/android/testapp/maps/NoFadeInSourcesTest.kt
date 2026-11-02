package org.maplibre.android.testapp.maps

import androidx.test.internal.runner.junit4.AndroidJUnit4ClassRunner
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import org.junit.runner.RunWith
import org.maplibre.android.testapp.action.MapLibreMapAction
import org.maplibre.android.testapp.activity.EspressoTest

/**
 * [org.maplibre.android.maps.MapLibreMap.setNoFadeInSources] on a real map: the ids go to native
 * code and [org.maplibre.android.maps.MapLibreMap.getNoFadeInSources] reads them back from it.
 * What the ids do to symbol placement is tested in core.
 */
@RunWith(AndroidJUnit4ClassRunner::class)
class NoFadeInSourcesTest : EspressoTest() {

    @Test
    fun aNullIdIsRefusedAndTheIdsSetBeforeAreKept() {
        validateTestSetup()
        MapLibreMapAction.invoke(maplibreMap) { _, map ->
            map.setNoFadeInSources(arrayOf("labels"))

            val withNull = arrayOf<String?>("names", null)
            assertThrows(IllegalArgumentException::class.java) {
                @Suppress("UNCHECKED_CAST")
                map.setNoFadeInSources(withNull as Array<String>)
            }

            assertEquals(setOf("labels"), map.noFadeInSources.toSet())
        }
    }
}
