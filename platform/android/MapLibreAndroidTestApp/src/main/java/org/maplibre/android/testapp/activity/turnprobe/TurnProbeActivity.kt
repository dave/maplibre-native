package org.maplibre.android.testapp.activity.turnprobe

import android.os.Bundle
import android.view.ViewGroup
import androidx.appcompat.app.AppCompatActivity
import org.maplibre.android.maps.MapLibreMapOptions
import org.maplibre.android.maps.MapView

/**
 * PROBE (throwaway branch probe/android-turn-atomicity): a full-screen
 * TextureView map, as Viatrail uses, for TurnAtomicityProbeTest.
 */
class TurnProbeActivity : AppCompatActivity() {
    lateinit var mapView: MapView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val options = MapLibreMapOptions.createFromAttributes(this, null).textureMode(true)
        mapView = MapView(this, options)
        mapView.layoutParams = ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        )
        setContentView(mapView)
        mapView.onCreate(savedInstanceState)
    }

    override fun onStart() {
        super.onStart()
        mapView.onStart()
    }

    override fun onResume() {
        super.onResume()
        mapView.onResume()
    }

    override fun onPause() {
        super.onPause()
        mapView.onPause()
    }

    override fun onStop() {
        super.onStop()
        mapView.onStop()
    }

    override fun onDestroy() {
        super.onDestroy()
        mapView.onDestroy()
    }
}
