package org.maplibre.android.storage;

import androidx.annotation.Keep;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import org.maplibre.android.LibraryLoader;
import org.maplibre.android.log.Logger;

/**
 * The {@link ResourceProvider}s of the process: one for each URL scheme, for every map and
 * snapshotter. The URLs of a registered scheme go to no other file source: no disk cache, no network.
 * A URL whose scheme has no provider and no file source is an error.
 */
public final class ResourceProviders {

  private static final String TAG = "Mbgl-ResourceProviders";

  static {
    LibraryLoader.load();
  }

  private ResourceProviders() {
  }

  /**
   * Sets the provider of a URL scheme, in place of any before it. A request already started stays
   * with its provider. Safe on any thread.
   *
   * @param scheme   the text before "://"
   * @param provider the provider
   * @throws IllegalArgumentException if scheme is empty or null, or provider is null
   */
  public static void register(@NonNull String scheme, @NonNull ResourceProvider provider) {
    if (scheme == null || scheme.isEmpty()) {
      throw new IllegalArgumentException("register: the scheme is empty");
    }
    if (provider == null) {
      throw new IllegalArgumentException("register: the provider is null for " + scheme);
    }
    nativeRegister(scheme, new Adapter(provider));
  }

  /**
   * Removes the provider of a URL scheme. Requests already started stay with it. Safe on any thread.
   *
   * @param scheme the text before "://"
   * @throws IllegalArgumentException if scheme is null
   */
  public static void unregister(@NonNull String scheme) {
    if (scheme == null) {
      throw new IllegalArgumentException("unregister: the scheme is null");
    }
    nativeUnregister(scheme);
  }

  /**
   * @param scheme the text before "://"
   * @return the counts of the provider now registered for a URL scheme; null if there is none
   * @throws IllegalArgumentException if scheme is null
   */
  @Nullable
  public static ResourceProviderCounts getCounts(@NonNull String scheme) {
    if (scheme == null) {
      throw new IllegalArgumentException("getCounts: the scheme is null");
    }
    long[] counts = nativeCounts(scheme);
    return counts == null ? null : new ResourceProviderCounts(counts[0], counts[1]);
  }

  /**
   * What native code calls, on the resource loader thread. A provider's exception stops here: it
   * must not cross into native code on that thread.
   */
  @Keep
  static final class Adapter {
    private final ResourceProvider provider;

    Adapter(@NonNull ResourceProvider provider) {
      this.provider = provider;
    }

    @Keep
    void start(long id, int kind, @NonNull String url, @NonNull String sourceId, boolean hasTile,
               @NonNull String urlTemplate, int pixelRatio, int x, int y, int z, boolean localOnly,
               boolean lowPriority, long responder) {
      ResourceRequest.Tile tile = hasTile ? new ResourceRequest.Tile(urlTemplate, pixelRatio, x, y, z) : null;
      ResourceRequest request =
        new ResourceRequest(id, kind, url, sourceId, tile, localOnly, lowPriority, responder);
      try {
        provider.start(request);
      } catch (Throwable error) {
        Logger.e(TAG, "A resource provider threw in start for " + url, error);
        request.answerError("The resource provider threw: " + error);
      }
    }

    @Keep
    void cancel(long id) {
      try {
        provider.cancel(id);
      } catch (Throwable error) {
        Logger.e(TAG, "A resource provider threw in cancel for request " + id, error);
      }
    }
  }

  @Keep
  private static native void nativeRegister(String scheme, Adapter adapter);

  @Keep
  private static native void nativeUnregister(String scheme);

  @Keep
  @Nullable
  private static native long[] nativeCounts(String scheme);
}
