package org.maplibre.android.testapp.storage;

import androidx.test.espresso.UiController;
import androidx.test.internal.runner.junit4.AndroidJUnit4ClassRunner;

import org.junit.After;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.maplibre.android.camera.CameraUpdateFactory;
import org.maplibre.android.geometry.LatLng;
import org.maplibre.android.maps.MapView;
import org.maplibre.android.storage.Resource;
import org.maplibre.android.storage.ResourceProvider;
import org.maplibre.android.storage.ResourceProviderCounts;
import org.maplibre.android.storage.ResourceProviders;
import org.maplibre.android.storage.ResourceRequest;
import org.maplibre.android.style.layers.CircleLayer;
import org.maplibre.android.style.layers.LineLayer;
import org.maplibre.android.style.sources.GeoJsonSource;
import org.maplibre.android.style.sources.TileSet;
import org.maplibre.android.style.sources.VectorSource;
import org.maplibre.android.testapp.action.MapLibreMapAction;
import org.maplibre.android.testapp.activity.EspressoTest;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

/**
 * The Android binding of the resource provider, on a real map: {@link ResourceProviders},
 * {@link ResourceProvider} and {@link ResourceRequest}. What each answer means to a map is tested
 * in core. Each wait ends on an event; its bound only turns a missing event into a failure.
 */
@RunWith(AndroidJUnit4ClassRunner.class)
public class ResourceProviderTest extends EspressoTest {
  private static final long BOUND_MS = 30_000;
  private static final String SCHEME = "provider-test";
  private static final String TILES = SCHEME + "://tiles/{z}/{x}/{y}.pbf";
  private static final String POINT = "{\"type\":\"FeatureCollection\",\"features\":["
    + "{\"type\":\"Feature\",\"properties\":{},\"geometry\":{\"type\":\"Point\",\"coordinates\":[0,0]}}]}";

  /** Records what the map asks, on the thread it asks on, and answers with {@link #answer}. */
  private static final class Provider implements ResourceProvider {
    final List<ResourceRequest> started = Collections.synchronizedList(new ArrayList<>());
    final List<Long> cancelled = Collections.synchronizedList(new ArrayList<>());
    final List<Thread> threads = Collections.synchronizedList(new ArrayList<>());
    final Consumer<ResourceRequest> answer;

    Provider(Consumer<ResourceRequest> answer) {
      this.answer = answer;
    }

    @Override
    public void start(ResourceRequest request) {
      threads.add(Thread.currentThread());
      started.add(request);
      answer.accept(request);
    }

    @Override
    public void cancel(long id) {
      threads.add(Thread.currentThread());
      cancelled.add(id);
    }
  }

  private final ExecutorService answers = Executors.newSingleThreadExecutor();
  private final Map<String, Long> failed = Collections.synchronizedMap(new HashMap<>());
  private final Map<String, String> failures = Collections.synchronizedMap(new HashMap<>());

  private static void waitFor(UiController uiController, BooleanSupplier done, String what) {
    long deadline = System.nanoTime() / 1_000_000L + BOUND_MS;
    while (!done.getAsBoolean()) {
      if (System.nanoTime() / 1_000_000L > deadline) {
        fail(what + ": not within " + BOUND_MS + " ms");
      }
      uiController.loopMainThreadForAtLeast(20);
    }
  }

  private static byte[] bytes(String text) {
    return text.getBytes(StandardCharsets.UTF_8);
  }

  private static long inFlight() {
    ResourceProviderCounts counts = ResourceProviders.getCounts(SCHEME);
    assertNotNull(counts);
    return counts.getInFlight();
  }

  @After
  public void unregister() {
    answers.shutdownNow();
    ResourceProviders.unregister(SCHEME);
  }

  private MapView.OnSourceBodyFailedListener recordFailures() {
    MapView.OnSourceBodyFailedListener listener = (sourceId, generation, message) -> {
      failed.put(sourceId, generation);
      failures.put(sourceId, message);
    };
    mapView.addOnSourceBodyFailedListener(listener);
    return listener;
  }

  @Test
  public void bodyAnsweredFromAnotherThreadLandsAndTheRequestSaysWhatItAsks() {
    validateTestSetup();
    Provider provider = new Provider(request -> answers.execute(() -> request.answerData(bytes(POINT))));
    ResourceProviders.register(SCHEME, provider);
    MapLibreMapAction.invoke(maplibreMap, (uiController, maplibreMap) -> {
      List<Long> laidOut = new ArrayList<>();
      MapView.OnSourceLaidOutListener listener = (sourceId, generation) -> {
        if (sourceId.equals("body")) {
          laidOut.add(generation);
        }
      };
      mapView.addOnSourceLaidOutListener(listener);
      maplibreMap.moveCamera(CameraUpdateFactory.newLatLngZoom(new LatLng(0, 0), 2));
      GeoJsonSource source = new GeoJsonSource("body");
      maplibreMap.getStyle().addSource(source);
      maplibreMap.getStyle().addLayer(new CircleLayer("body-circles", "body"));

      long generation = source.setUri(SCHEME + "://body.geojson?v=1");
      waitFor(uiController, () -> laidOut.contains(generation), "generation " + generation + " laid out");
      mapView.removeOnSourceLaidOutListener(listener);

      assertFalse(source.querySourceFeatures(null).isEmpty());
      assertEquals(1, provider.started.size());
      ResourceRequest request = provider.started.get(0);
      assertNotEquals(0, request.getId());
      assertEquals(Resource.SOURCE, request.getKind());
      assertEquals(SCHEME + "://body.geojson?v=1", request.getUrl());
      assertEquals("body", request.getSourceId());
      assertNull(request.getTile());
      assertFalse(request.isLocalOnly());
      assertFalse(request.isLowPriority());
      assertNotEquals(Thread.currentThread(), provider.threads.get(0));
    });
  }

  private Consumer<ResourceRequest> onAnotherThread(Consumer<ResourceRequest> answer) {
    return request -> answers.execute(() -> answer.accept(request));
  }

  private void assertABodyAnswerIsReportedFailed(String id, Consumer<ResourceRequest> answer, String why) {
    ResourceProviders.register(SCHEME, new Provider(answer));
    MapLibreMapAction.invoke(maplibreMap, (uiController, maplibreMap) -> {
      MapView.OnSourceBodyFailedListener listener = recordFailures();
      GeoJsonSource source = new GeoJsonSource(id);
      maplibreMap.getStyle().addSource(source);
      maplibreMap.getStyle().addLayer(new CircleLayer(id + "-circles", id));

      long generation = source.setUri(SCHEME + "://" + id + ".geojson");
      waitFor(uiController, () -> failed.containsKey(id), id + " reported failed");

      assertEquals(Long.valueOf(generation), failed.get(id));
      assertTrue(failures.get(id), failures.get(id).contains(why));
      mapView.removeOnSourceBodyFailedListener(listener);
    });
  }

  @Test
  public void answerUnavailableReachesTheMapWithItsReason() {
    validateTestSetup();
    assertABodyAnswerIsReportedFailed("unavailable",
      onAnotherThread(request -> request.answerUnavailable("no network now")), "no network now");
  }

  @Test
  public void answerNotFoundReachesTheMapWithItsReason() {
    validateTestSetup();
    assertABodyAnswerIsReportedFailed("not-found",
      onAnotherThread(request -> request.answerNotFound("no such body")), "no such body");
  }

  @Test
  public void answerErrorReachesTheMapWithItsReason() {
    validateTestSetup();
    assertABodyAnswerIsReportedFailed("error",
      onAnotherThread(request -> request.answerError("the store broke")), "the store broke");
  }

  @Test
  public void answerEmptyReachesTheMap() {
    validateTestSetup();
    assertABodyAnswerIsReportedFailed("empty", onAnotherThread(ResourceRequest::answerEmpty), "");
  }

  @Test
  public void providerThatThrowsInStartAnswersTheError() {
    validateTestSetup();
    assertABodyAnswerIsReportedFailed("throws", request -> {
      throw new IllegalStateException("the provider broke");
    }, "the provider broke");
  }

  @Test
  public void firstAnswerWinsAndAnAnswerOnTheLoaderThreadLands() {
    validateTestSetup();
    ResourceProviders.register(SCHEME, new Provider(request -> {
      request.answerData(bytes(POINT));
      request.answerError("too late");
    }));
    MapLibreMapAction.invoke(maplibreMap, (uiController, maplibreMap) -> {
      MapView.OnSourceBodyFailedListener failures = recordFailures();
      List<Long> laidOut = new ArrayList<>();
      MapView.OnSourceLaidOutListener listener = (sourceId, generation) -> {
        if (sourceId.equals("first")) {
          laidOut.add(generation);
        }
      };
      mapView.addOnSourceLaidOutListener(listener);
      GeoJsonSource source = new GeoJsonSource("first");
      maplibreMap.getStyle().addSource(source);
      maplibreMap.getStyle().addLayer(new CircleLayer("first-circles", "first"));

      long generation = source.setUri(SCHEME + "://first.geojson");
      waitFor(uiController, () -> laidOut.contains(generation), "generation " + generation + " laid out");

      assertFalse(failed.containsKey("first"));
      assertEquals(0, inFlight());
      mapView.removeOnSourceLaidOutListener(listener);
      mapView.removeOnSourceBodyFailedListener(failures);
    });
  }

  @Test
  public void tileAsksLocalFirstThenFetchesAndTheRequestNamesItsTile() {
    validateTestSetup();
    Provider provider = new Provider(request -> {
      if (request.isLocalOnly()) {
        request.answerNotFound("not stored");
      } else {
        request.answerEmpty();
      }
    });
    ResourceProviders.register(SCHEME, provider);
    MapLibreMapAction.invoke(maplibreMap, (uiController, maplibreMap) -> {
      maplibreMap.moveCamera(CameraUpdateFactory.newLatLngZoom(new LatLng(0, 0), 2));
      maplibreMap.getStyle().addSource(new VectorSource("tiles", new TileSet("2.2.0", TILES)));
      LineLayer layer = new LineLayer("tile-lines", "tiles");
      layer.setSourceLayer("lines");
      maplibreMap.getStyle().addLayer(layer);

      waitFor(uiController, () -> !provider.started.isEmpty(), "a tile's first step");

      ResourceRequest local = provider.started.get(0);
      assertEquals(Resource.TILE, local.getKind());
      assertEquals("tiles", local.getSourceId());
      assertTrue(local.isLocalOnly());
      ResourceRequest.Tile tile = local.getTile();
      assertNotNull(tile);
      assertEquals(TILES, tile.getUrlTemplate());
      // The first tile can be a prefetched parent of the camera's zoom.
      assertEquals(SCHEME + "://tiles/" + tile.getZ() + "/" + tile.getX() + "/" + tile.getY() + ".pbf",
        local.getUrl());
      assertTrue(tile.getPixelRatio() >= 1);
      waitFor(uiController, () -> {
        for (ResourceRequest request : new ArrayList<>(provider.started)) {
          if (!request.isLocalOnly() && request.getUrl().equals(local.getUrl())) {
            return true;
          }
        }
        return false;
      }, "the fetch step of " + local.getUrl());
    });
  }

  @Test
  public void droppedRequestIsCancelledOnTheLoaderThreadAndALateAnswerIsIgnored() {
    validateTestSetup();
    List<ResourceRequest> held = Collections.synchronizedList(new ArrayList<>());
    Provider provider = new Provider(request -> {
      if (request.isLocalOnly()) {
        request.answerNotFound("not stored");
      } else {
        held.add(request);
      }
    });
    ResourceProviders.register(SCHEME, provider);
    MapLibreMapAction.invoke(maplibreMap, (uiController, maplibreMap) -> {
      maplibreMap.moveCamera(CameraUpdateFactory.newLatLngZoom(new LatLng(0, 0), 2));
      maplibreMap.getStyle().addSource(new VectorSource("tiles", new TileSet("2.2.0", TILES)));
      LineLayer layer = new LineLayer("tile-lines", "tiles");
      layer.setSourceLayer("lines");
      maplibreMap.getStyle().addLayer(layer);
      waitFor(uiController, () -> !held.isEmpty(), "a tile's fetch step held");

      ResourceProviderCounts counts = ResourceProviders.getCounts(SCHEME);
      assertNotNull(counts);
      assertTrue(counts.getInFlight() >= 1);
      assertTrue(counts.getPeakInFlight() >= counts.getInFlight());

      maplibreMap.getStyle().removeLayer(layer);
      maplibreMap.getStyle().removeSource("tiles");
      waitFor(uiController, () -> inFlight() == 0, "every held request ended");

      List<Long> heldIds = new ArrayList<>();
      for (ResourceRequest request : new ArrayList<>(held)) {
        heldIds.add(request.getId());
      }
      List<Long> cancelled = new ArrayList<>(provider.cancelled);
      Collections.sort(heldIds);
      Collections.sort(cancelled);
      assertEquals("one cancel for each held request, and none for an answered one", heldIds, cancelled);
      for (Thread thread : new ArrayList<>(provider.threads)) {
        assertEquals("start and cancel run on one thread", provider.threads.get(0), thread);
      }
      assertNotEquals(Thread.currentThread(), provider.threads.get(0));

      long before = ResourceProviders.getCounts(SCHEME).getPeakInFlight();
      held.get(0).answerData(bytes("late"));
      assertEquals(0, inFlight());
      assertEquals(before, ResourceProviders.getCounts(SCHEME).getPeakInFlight());
    });
  }

  @Test
  public void nullAnswerIsRefusedBeforeItReachesNativeCode() {
    validateTestSetup();
    List<Throwable> thrown = Collections.synchronizedList(new ArrayList<>());
    ResourceProviders.register(SCHEME, new Provider(request -> {
      for (Runnable answer : new Runnable[] {
        () -> request.answerData(null),
        () -> request.answerUnavailable(null),
        () -> request.answerNotFound(null),
        () -> request.answerError(null)
      }) {
        try {
          answer.run();
        } catch (IllegalArgumentException expected) {
          thrown.add(expected);
        }
      }
      request.answerData(bytes(POINT));
    }));
    MapLibreMapAction.invoke(maplibreMap, (uiController, maplibreMap) -> {
      GeoJsonSource source = new GeoJsonSource("nulls");
      maplibreMap.getStyle().addSource(source);
      source.setUri(SCHEME + "://nulls.geojson");
      waitFor(uiController, () -> thrown.size() == 4, "four refused answers");
    });
  }

  @Test
  public void registryRefusesBadArgumentsAndCountsOnlyARegisteredScheme() {
    assertNull(ResourceProviders.getCounts(SCHEME));
    Provider provider = new Provider(ResourceRequest::answerEmpty);
    for (Runnable call : new Runnable[] {
      () -> ResourceProviders.register("", provider),
      () -> ResourceProviders.register(null, provider),
      () -> ResourceProviders.register(SCHEME, null),
      () -> ResourceProviders.unregister(null),
      () -> ResourceProviders.getCounts(null)
    }) {
      try {
        call.run();
        fail("a bad argument was taken");
      } catch (IllegalArgumentException expected) {
        assertNotNull(expected.getMessage());
      }
    }
    assertNull(ResourceProviders.getCounts(SCHEME));

    ResourceProviders.register(SCHEME, provider);
    ResourceProviderCounts counts = ResourceProviders.getCounts(SCHEME);
    assertNotNull(counts);
    assertEquals(0, counts.getInFlight());
    assertEquals(0, counts.getPeakInFlight());

    ResourceProviders.unregister(SCHEME);
    assertNull(ResourceProviders.getCounts(SCHEME));
  }

  @Test
  public void unregisteredSchemeIsAnError() {
    validateTestSetup();
    MapLibreMapAction.invoke(maplibreMap, (uiController, maplibreMap) -> {
      MapView.OnSourceBodyFailedListener listener = recordFailures();
      GeoJsonSource source = new GeoJsonSource("no-provider");
      maplibreMap.getStyle().addSource(source);
      maplibreMap.getStyle().addLayer(new CircleLayer("no-provider-circles", "no-provider"));

      long generation = source.setUri(SCHEME + "://nobody.geojson");
      waitFor(uiController, () -> failed.containsKey("no-provider"), "no-provider reported failed");

      assertEquals(Long.valueOf(generation), failed.get("no-provider"));
      mapView.removeOnSourceBodyFailedListener(listener);
    });
  }
}
