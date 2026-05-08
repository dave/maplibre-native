package org.maplibre.android.testapp.activity.sources

import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity
import org.maplibre.android.maps.MapLibreMap
import org.maplibre.android.maps.MapView
import org.maplibre.android.maps.Style
import org.maplibre.android.style.layers.FillLayer
import org.maplibre.android.style.layers.PropertyFactory.fillColor
import org.maplibre.android.style.sources.VectorSource
import org.maplibre.android.testapp.R

class PMTilesActivity : AppCompatActivity() {
    private lateinit var mapView: MapView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_pmtiles)
        mapView = findViewById(R.id.mapView)

        mapView.getMapAsync { map ->
            // --8<-- [start:loadStyle]
            map.setStyle("https://raw.githubusercontent.com/wipfli/foursquare-os-places-pmtiles/refs/heads/main/style.json")
            // --8<-- [end:loadStyle]
        }
    }

    @Suppress("unused")
    private fun addSourceExample(map: MapLibreMap) {
        map.setStyle(Style.Builder()) { style ->
            // --8<-- [start:addSource]
            val source = VectorSource(
                "world",
                "pmtiles://https://demotiles.maplibre.org/pmtiles/vector/world.pmtiles"
            )
            style.addSource(source)
            val layer = FillLayer("countries", "world").apply {
                setSourceLayer("countries")
                setProperties(fillColor("#aad3df"))
            }
            style.addLayer(layer)
            // --8<-- [end:addSource]
        }
    }

    @Suppress("unused")
    private fun loadFromAssetExample(style: Style) {
        // --8<-- [start:loadFromAsset]
        val source = VectorSource("world", "pmtiles://asset://world.pmtiles")
        style.addSource(source)
        val layer = FillLayer("countries", "world").apply {
            setSourceLayer("countries")
            setProperties(fillColor("#aad3df"))
        }
        style.addLayer(layer)
        // --8<-- [end:loadFromAsset]
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

    override fun onLowMemory() {
        super.onLowMemory()
        mapView.onLowMemory()
    }

    override fun onDestroy() {
        super.onDestroy()
        mapView.onDestroy()
    }
}
