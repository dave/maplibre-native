package org.maplibre.android.testapp.maps

import androidx.test.espresso.Espresso.onView
import androidx.test.espresso.UiController
import androidx.test.espresso.ViewAction
import androidx.test.espresso.matcher.ViewMatchers.isDisplayed
import androidx.test.espresso.matcher.ViewMatchers.withId
import androidx.test.internal.runner.junit4.AndroidJUnit4ClassRunner
import androidx.test.rule.ActivityTestRule
import android.view.View
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.hamcrest.Matcher
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.fail
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.maplibre.android.maps.MapLibreMap
import org.maplibre.android.maps.MapView
import org.maplibre.android.maps.Style
import org.maplibre.android.module.http.HttpRequestUtil
import org.maplibre.android.testapp.R
import org.maplibre.android.testapp.activity.espresso.EspressoTestActivity
import java.util.Collections

/**
 * A map view that is given no style loads none: it asks the network for nothing, so no
 * default style is fetched from a remote server before the app sets its own.
 *
 * The map's first request after it is ready is the marker: a source URL in a style the test
 * sets. A request made before the marker came from the map on its own.
 */
@RunWith(AndroidJUnit4ClassRunner::class)
class NoDefaultStyleTest {

    @Rule
    @JvmField
    val rule = ActivityTestRule(EspressoTestActivity::class.java, false, false)

    /** Every host asked, in order. */
    private val hosts: MutableList<String> = Collections.synchronizedList(mutableListOf())

    @After
    fun restoreHttpClient() {
        HttpRequestUtil.setOkHttpClient(null)
    }

    @Test
    fun aMapViewWithNoStyleAsksTheNetworkForNothing() {
        HttpRequestUtil.setOkHttpClient(
            OkHttpClient.Builder()
                .addInterceptor { chain ->
                    hosts += chain.request().url.host
                    Response.Builder()
                        .request(chain.request())
                        .protocol(Protocol.HTTP_1_1)
                        .code(404)
                        .message("Not Found")
                        .body(ByteArray(0).toResponseBody("application/json".toMediaType()))
                        .build()
                }
                .build()
        )
        rule.launchActivity(null)

        onMap { uiController, map ->
            // Every task the map's creation posted to the main thread has run.
            uiController.loopMainThreadUntilIdle()
            map.setStyle(Style.Builder().fromJson(MARKER_STYLE))
            waitFor(uiController) { hosts.contains(MARKER_HOST) }
        }

        assertEquals("the hosts asked before the marker", emptyList<String>(), hosts.takeWhile { it != MARKER_HOST })
    }

    /** Runs [action] on the main thread with the activity's map, once it is ready. */
    private fun onMap(action: (UiController, MapLibreMap) -> Unit) {
        lateinit var mapView: MapView
        var map: MapLibreMap? = null
        rule.runOnUiThread {
            mapView = rule.activity.findViewById(R.id.mapView)
            mapView.getMapAsync { map = it }
        }
        onView(withId(android.R.id.content)).perform(object : ViewAction {
            override fun getConstraints(): Matcher<View> = isDisplayed()

            override fun getDescription() = "the map, once it is ready"

            override fun perform(uiController: UiController, view: View) {
                waitFor(uiController) { map != null }
                action(uiController, map!!)
            }
        })
    }

    /** Loops the main thread until [done]; the bound only turns a missing event into a failure. */
    private fun waitFor(uiController: UiController, done: () -> Boolean) {
        val deadline = System.nanoTime() / 1_000_000L + BOUND_MS
        while (!done()) {
            if (System.nanoTime() / 1_000_000L > deadline) {
                fail("not within $BOUND_MS ms; hosts asked: $hosts")
            }
            uiController.loopMainThreadForAtLeast(50)
        }
    }

    companion object {
        private const val BOUND_MS = 30_000L
        private const val MARKER_HOST = "no-default-style.test"
        private const val MARKER_STYLE = """{"version": 8, "sources": {"marker": {"type": "geojson",
            "data": "https://$MARKER_HOST/marker.geojson"}}, "layers": [{"id": "marker", "type": "circle",
            "source": "marker"}]}"""
    }
}
