package org.maplibre.android.testapp.style;

import androidx.test.espresso.UiController;
import androidx.test.internal.runner.junit4.AndroidJUnit4ClassRunner;

import org.junit.After;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.maplibre.android.camera.CameraUpdateFactory;
import org.maplibre.android.geometry.LatLng;
import org.maplibre.android.maps.MapView;
import org.maplibre.android.maps.Style;
import org.maplibre.android.module.http.HttpRequestUtil;
import org.maplibre.android.style.layers.LineLayer;
import org.maplibre.android.style.sources.RasterDemSource;
import org.maplibre.android.style.sources.Source;
import org.maplibre.android.style.sources.TileSet;
import org.maplibre.android.style.sources.VectorSource;
import org.maplibre.android.testapp.action.MapLibreMapAction;
import org.maplibre.android.testapp.activity.EspressoTest;

import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;

import okhttp3.MediaType;
import okhttp3.OkHttpClient;
import okhttp3.Protocol;
import okhttp3.Response;
import okhttp3.ResponseBody;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.fail;

/**
 * {@link VectorSource#reloadEmptyTiles()} asks the network again for a tile whose answer was
 * empty (HTTP 204). Without the call, MapLibre does not ask again for a tile that stays in view.
 * Each wait ends on an event; its bound only turns a missing event into a failure.
 */
@RunWith(AndroidJUnit4ClassRunner.class)
public class ReloadEmptyTilesTest extends EspressoTest {
  private static final long BOUND_MS = 30_000;
  private static final String HOST = "reload-empty-tiles.test";
  private static final String DEM_STYLE = "{\"version\":8,\"sources\":{\"dem\":{\"type\":\"raster-dem\","
    + "\"tiles\":[\"https://" + HOST + "/{z}/{x}/{y}.png\"]}},"
    + "\"layers\":[{\"id\":\"shade\",\"type\":\"hillshade\",\"source\":\"dem\"}]}";

  /** Tile requests to {@link #HOST}, each answered 204. */
  private final AtomicInteger tileRequests = new AtomicInteger();

  private static void waitFor(UiController uiController, BooleanSupplier done, String what) {
    long deadline = System.nanoTime() / 1_000_000L + BOUND_MS;
    while (!done.getAsBoolean()) {
      if (System.nanoTime() / 1_000_000L > deadline) {
        fail(what + ": not within " + BOUND_MS + " ms");
      }
      uiController.loopMainThreadForAtLeast(50);
    }
  }

  @After
  public void restoreHttpClient() {
    HttpRequestUtil.setOkHttpClient(null);
  }

  /** Answers every request to {@link #HOST} with a 204, and counts it. */
  private void answerTileRequestsWith204() {
    HttpRequestUtil.setOkHttpClient(new OkHttpClient.Builder()
      .addInterceptor(chain -> {
        if (!chain.request().url().host().equals(HOST)) {
          return chain.proceed(chain.request());
        }
        tileRequests.incrementAndGet();
        return new Response.Builder()
          .request(chain.request())
          .protocol(Protocol.HTTP_1_1)
          .code(204)
          .message("No Content")
          .body(ResponseBody.create(new byte[0], MediaType.get("application/x-protobuf")))
          .build();
      })
      .build());
  }

  @Test
  public void rasterDemSourceFromStyleJsonIsARasterDemSourceAndAsksAgain() {
    validateTestSetup();
    answerTileRequestsWith204();

    MapLibreMapAction.invoke(maplibreMap, (uiController, maplibreMap) -> {
      AtomicInteger idle = new AtomicInteger();
      MapView.OnDidBecomeIdleListener listener = idle::incrementAndGet;
      mapView.addOnDidBecomeIdleListener(listener);

      AtomicReference<Style> loaded = new AtomicReference<>();
      maplibreMap.moveCamera(CameraUpdateFactory.newLatLngZoom(new LatLng(0, 0), 2));
      int idleBefore = idle.get();
      maplibreMap.setStyle(new Style.Builder().fromJson(DEM_STYLE), loaded::set);
      waitFor(uiController, () -> loaded.get() != null, "the style loaded");

      Source source = loaded.get().getSource("dem");
      assertNotNull(source);
      assertEquals(RasterDemSource.class, source.getClass());

      waitFor(uiController, () -> tileRequests.get() > 0 && idle.get() > idleBefore,
        "the tiles asked for and the map idle");
      int asked = tileRequests.get();

      ((RasterDemSource) source).reloadEmptyTiles();
      waitFor(uiController, () -> tileRequests.get() > asked, "an empty tile asked for again");
      mapView.removeOnDidBecomeIdleListener(listener);
    });
  }

  @Test
  public void reloadEmptyTilesAsksAgainForATileThatAnswered204() {
    validateTestSetup();
    answerTileRequestsWith204();

    MapLibreMapAction.invoke(maplibreMap, (uiController, maplibreMap) -> {
      AtomicInteger idle = new AtomicInteger();
      MapView.OnDidBecomeIdleListener listener = idle::incrementAndGet;
      mapView.addOnDidBecomeIdleListener(listener);

      maplibreMap.moveCamera(CameraUpdateFactory.newLatLngZoom(new LatLng(0, 0), 2));
      TileSet tileSet = new TileSet("2.2.0", "https://" + HOST + "/{z}/{x}/{y}.pbf");
      VectorSource source = new VectorSource("reload", tileSet);
      maplibreMap.getStyle().addSource(source);
      LineLayer layer = new LineLayer("reload-lines", source.getId());
      layer.setSourceLayer("lines");
      maplibreMap.getStyle().addLayer(layer);

      // The tiles are asked for and answered 204, and the map goes idle.
      int idleBefore = idle.get();
      waitFor(uiController, () -> tileRequests.get() > 0 && idle.get() > idleBefore,
        "the tiles asked for and the map idle");
      int asked = tileRequests.get();

      source.reloadEmptyTiles();
      waitFor(uiController, () -> tileRequests.get() > asked, "an empty tile asked for again");
      mapView.removeOnDidBecomeIdleListener(listener);
    });
  }
}
