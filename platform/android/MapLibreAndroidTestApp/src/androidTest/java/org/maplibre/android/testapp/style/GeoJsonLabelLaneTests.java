package org.maplibre.android.testapp.style;

import androidx.test.espresso.UiController;
import androidx.test.internal.runner.junit4.AndroidJUnit4ClassRunner;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.maplibre.android.maps.LabelSteps;
import org.maplibre.android.maps.MapView;
import org.maplibre.android.module.http.HttpRequestUtil;
import org.maplibre.android.style.expressions.Expression;
import org.maplibre.android.style.layers.CircleLayer;
import org.maplibre.android.style.sources.GeoJsonOptions;
import org.maplibre.android.style.sources.GeoJsonSource;
import org.maplibre.android.testapp.action.MapLibreMapAction;
import org.maplibre.android.testapp.activity.EspressoTest;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.function.BooleanSupplier;

import okhttp3.MediaType;
import okhttp3.OkHttpClient;
import okhttp3.Protocol;
import okhttp3.Response;
import okhttp3.ResponseBody;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

/**
 * The Android binding of the label lane: {@link GeoJsonOptions#withLabelLane}, the data
 * generations the setters answer, {@link MapView.OnSourceLaidOutListener},
 * {@link MapView.OnSourceBodyFailedListener} and {@link LabelSteps}.
 * Each wait ends on an event; its bound only turns a missing event into a failure.
 */
@RunWith(AndroidJUnit4ClassRunner.class)
public class GeoJsonLabelLaneTests extends EspressoTest {
  private static final long BOUND_MS = 30_000;

  private static final String POINTS = "{\"type\":\"FeatureCollection\",\"features\":["
    + "{\"type\":\"Feature\",\"properties\":{},\"geometry\":{\"type\":\"Point\",\"coordinates\":[-20,20]}},"
    + "{\"type\":\"Feature\",\"properties\":{},\"geometry\":{\"type\":\"Point\",\"coordinates\":[20,-20]}}]}";

  private static void waitFor(UiController uiController, BooleanSupplier done, String what) {
    long deadline = System.nanoTime() / 1_000_000L + BOUND_MS;
    while (!done.getAsBoolean()) {
      if (System.nanoTime() / 1_000_000L > deadline) {
        fail(what + ": not within " + BOUND_MS + " ms");
      }
      uiController.loopMainThreadForAtLeast(50);
    }
  }

  @Test
  public void laneSourceIsRefusedWithASynchronousUpdate() {
    validateTestSetup();
    MapLibreMapAction.invoke(maplibreMap, (uiController, maplibreMap) -> {
      try {
        new GeoJsonSource("both", new GeoJsonOptions().withLabelLane(true).withSynchronousUpdate(true));
        fail("a source with labelLane and synchronousUpdate was made");
      } catch (IllegalArgumentException expected) {
        assertTrue(expected.getMessage(), expected.getMessage().contains("labelLane"));
      }
    });
  }

  @Test
  public void laneSourceRefusesASynchronousOverride() {
    validateTestSetup();
    MapLibreMapAction.invoke(maplibreMap, (uiController, maplibreMap) -> {
      GeoJsonSource source = new GeoJsonSource("lane", new GeoJsonOptions().withLabelLane(true));
      try {
        source.setOverrideSynchronousUpdate(true);
        fail("a lane source took a synchronous override");
      } catch (IllegalStateException expected) {
        assertTrue(expected.getMessage(), expected.getMessage().contains("labelLane"));
      }
      source.setOverrideSynchronousUpdate(false);
    });
  }

  @Test
  public void theLastBodyIsReportedLaidOutWithItsGeneration() {
    validateTestSetup();
    MapLibreMapAction.invoke(maplibreMap, (uiController, maplibreMap) -> {
      List<Long> reported = new ArrayList<>();
      MapView.OnSourceLaidOutListener listener = (sourceId, generation) -> {
        if (sourceId.equals("lane")) {
          reported.add(generation);
        }
      };
      mapView.addOnSourceLaidOutListener(listener);
      GeoJsonSource source = new GeoJsonSource("lane", new GeoJsonOptions().withLabelLane(true));
      maplibreMap.getStyle().addSource(source);
      maplibreMap.getStyle().addLayer(new CircleLayer("lane-circles", source.getId()));

      Long first = source.setGeoJson(POINTS);
      Long second = source.setGeoJson(POINTS);
      assertEquals(Long.valueOf(first + 1), second);
      waitFor(uiController, () -> reported.contains(second), "generation " + second + " laid out");
      // Never a generation older than the source held when it was reported.
      for (Long generation : reported) {
        assertTrue(generation <= second);
      }
      mapView.removeOnSourceLaidOutListener(listener);
    });
  }

  @Test
  public void bodyThatDoesNotParseDoesNotHoldTheNextOne() {
    validateTestSetup();
    MapLibreMapAction.invoke(maplibreMap, (uiController, maplibreMap) -> {
      List<Long> reported = new ArrayList<>();
      MapView.OnSourceLaidOutListener listener = (sourceId, generation) -> {
        if (sourceId.equals("lane")) {
          reported.add(generation);
        }
      };
      mapView.addOnSourceLaidOutListener(listener);
      GeoJsonSource source = new GeoJsonSource("lane", new GeoJsonOptions().withLabelLane(true));
      maplibreMap.getStyle().addSource(source);
      maplibreMap.getStyle().addLayer(new CircleLayer("lane-circles", source.getId()));

      // The constructor's empty body first: set while it converts, the broken
      // body would wait as the next body, and the one after would coalesce it
      // away unconverted.
      waitFor(uiController, () -> !reported.isEmpty(), "the constructor's body laid out");
      // The next body waits for the broken one's answer (the binding coalesces
      // bodies, one in conversion at a time): it lands only if that answer comes.
      Long broken = source.setGeoJson("{not json");
      Long next = source.setGeoJson(POINTS);
      waitFor(uiController, () -> reported.contains(next), "the body after the broken one laid out");
      assertFalse(reported.contains(broken));
      mapView.removeOnSourceLaidOutListener(listener);
    });
  }

  /** Reports for the source "lane": generation and message, as they came. */
  private final List<Long> failed = new ArrayList<>();
  private final List<String> failures = new ArrayList<>();

  private MapView.OnSourceBodyFailedListener recordFailures() {
    MapView.OnSourceBodyFailedListener listener = (sourceId, generation, message) -> {
      if (sourceId.equals("lane")) {
        failed.add(generation);
        failures.add(message);
      }
    };
    mapView.addOnSourceBodyFailedListener(listener);
    return listener;
  }

  @Test
  public void bodyThatDoesNotParseIsReportedFailedWithItsGeneration() {
    validateTestSetup();
    MapLibreMapAction.invoke(maplibreMap, (uiController, maplibreMap) -> {
      MapView.OnSourceBodyFailedListener listener = recordFailures();
      GeoJsonSource source = new GeoJsonSource("lane", new GeoJsonOptions().withLabelLane(true));
      maplibreMap.getStyle().addSource(source);

      Long broken = source.setGeoJson("{not json");
      waitFor(uiController, () -> failed.contains(broken), "generation " + broken + " reported failed");

      assertEquals(Collections.singletonList(broken), failed);
      assertTrue(failures.get(0), failures.get(0).contains("parse"));
      mapView.removeOnSourceBodyFailedListener(listener);
    });
  }

  @Test
  public void synchronousBodyThatDoesNotParseIsReportedFailedInTheSetter() {
    validateTestSetup();
    MapLibreMapAction.invoke(maplibreMap, (uiController, maplibreMap) -> {
      MapView.OnSourceBodyFailedListener listener = recordFailures();
      GeoJsonSource source = new GeoJsonSource("lane", new GeoJsonOptions().withSynchronousUpdate(true));
      maplibreMap.getStyle().addSource(source);

      Long broken = source.setGeoJson("{not json");

      assertEquals(Collections.singletonList(broken), failed);
      mapView.removeOnSourceBodyFailedListener(listener);
    });
  }

  @Test
  public void urlThatAnswersAnErrorIsReportedFailedWithItsGeneration() {
    assertAUrlAnsweringIsReportedFailed(404, "Not Found");
  }

  @Test
  public void urlThatAnswersNoContentIsReportedFailedWithItsGeneration() {
    assertAUrlAnsweringIsReportedFailed(204, "No Content");
  }

  private void assertAUrlAnsweringIsReportedFailed(int code, String reason) {
    validateTestSetup();
    HttpRequestUtil.setOkHttpClient(new OkHttpClient.Builder()
      .addInterceptor(chain -> new Response.Builder()
        .request(chain.request())
        .protocol(Protocol.HTTP_1_1)
        .code(code)
        .message(reason)
        .body(ResponseBody.create(new byte[0], MediaType.get("application/json")))
        .build())
      .build());
    try {
      MapLibreMapAction.invoke(maplibreMap, (uiController, maplibreMap) -> {
        MapView.OnSourceBodyFailedListener listener = recordFailures();
        GeoJsonSource source = new GeoJsonSource("lane", new GeoJsonOptions().withLabelLane(true));
        maplibreMap.getStyle().addSource(source);
        maplibreMap.getStyle().addLayer(new CircleLayer("lane-circles", source.getId()));

        long generation = source.setUri("https://source-body-failed.test/body.geojson");
        waitFor(uiController, () -> failed.contains(generation), "generation " + generation + " reported failed");

        mapView.removeOnSourceBodyFailedListener(listener);
      });
    } finally {
      HttpRequestUtil.setOkHttpClient(null);
    }
  }

  @Test
  public void everyStepOfALaneSourceRunsOnTheLane() {
    validateTestSetup();
    MapLibreMapAction.invoke(maplibreMap, (uiController, maplibreMap) -> {
      LabelSteps.take();
      LabelSteps.setRecording(true);
      try {
        List<Long> reported = new ArrayList<>();
        MapView.OnSourceLaidOutListener listener = (sourceId, generation) -> {
          if (sourceId.equals("lane")) {
            reported.add(generation);
          }
        };
        mapView.addOnSourceLaidOutListener(listener);
        GeoJsonSource source = new GeoJsonSource("lane", new GeoJsonOptions().withLabelLane(true));
        maplibreMap.getStyle().addSource(source);
        maplibreMap.getStyle().addLayer(new CircleLayer("lane-circles", source.getId()));
        Long generation = source.setGeoJson(POINTS);
        waitFor(uiController, () -> reported.contains(generation), "generation " + generation + " laid out");
        mapView.removeOnSourceLaidOutListener(listener);

        List<String> names = new ArrayList<>();
        for (String line : LabelSteps.take().split("\n")) {
          String[] fields = line.split("\t");
          if (fields.length < 4 || !fields[0].equals("lane") || fields[3].equals("1")) {
            continue;
          }
          names.add(fields[1]);
          assertTrue(fields[1] + " ran on " + fields[2], fields[2].contains("Label lane"));
        }
        assertTrue(names.toString(), names.contains("convert"));
        assertTrue(names.toString(), names.contains("slice"));
        assertTrue(names.toString(), names.contains("tile-parse"));
        assertTrue(names.toString(), names.contains("tile-layout"));
        assertNotEquals(0, names.size());
      } finally {
        LabelSteps.setRecording(false);
        LabelSteps.take();
      }
    });
  }

  @Test
  public void laneSourceIsRefusedWithClusters() {
    validateTestSetup();
    MapLibreMapAction.invoke(maplibreMap, (uiController, maplibreMap) -> {
      try {
        new GeoJsonSource("both", new GeoJsonOptions().withLabelLane(true).withCluster(true));
        fail("a source with labelLane and cluster was made");
      } catch (IllegalArgumentException expected) {
        assertTrue(expected.getMessage(), expected.getMessage().contains("cluster"));
      }
    });
  }

  @Test
  public void filterChangeMovesTheGenerationAndIsReportedLaidOut() {
    validateTestSetup();
    MapLibreMapAction.invoke(maplibreMap, (uiController, maplibreMap) -> {
      List<Long> reported = new ArrayList<>();
      MapView.OnSourceLaidOutListener listener = (sourceId, generation) -> {
        if (sourceId.equals("lane")) {
          reported.add(generation);
        }
      };
      mapView.addOnSourceLaidOutListener(listener);
      GeoJsonSource source = new GeoJsonSource("lane", new GeoJsonOptions().withLabelLane(true));
      maplibreMap.getStyle().addSource(source);
      CircleLayer layer = new CircleLayer("lane-circles", source.getId());
      maplibreMap.getStyle().addLayer(layer);
      Long body = source.setGeoJson(POINTS);
      waitFor(uiController, () -> reported.contains(body), "the body laid out");

      layer.setFilter(Expression.has("missing"));
      long filtered = source.getDataGeneration();
      assertTrue("the filter change did not move the generation", filtered > body);
      waitFor(uiController, () -> reported.contains(filtered), "the filter change laid out");
      mapView.removeOnSourceLaidOutListener(listener);
    });
  }
}
