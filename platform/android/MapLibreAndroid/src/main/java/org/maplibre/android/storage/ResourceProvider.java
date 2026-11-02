package org.maplibre.android.storage;

import androidx.annotation.NonNull;

/**
 * Serves the resources of a URL scheme from inside the process, in place of the disk cache and the
 * network. Register one with {@link ResourceProviders#register(String, ResourceProvider)}.
 * <p>
 * Both methods run on the resource loader thread, which every map shares, so each must return at
 * once. Answer later, on any thread, through the {@link ResourceRequest}.
 * </p>
 */
public interface ResourceProvider {

  /**
   * A new request. Each request ends once: by the first answer, or by {@link #cancel(long)}.
   * The map waits for the answer, with no time limit, so every request needs an answer unless it
   * is cancelled.
   * <p>
   * If this method throws, the request is answered with {@link ResourceRequest#answerError(String)}.
   * </p>
   *
   * @param request the request, which also takes the answer
   */
  void start(@NonNull ResourceRequest request);

  /**
   * The request with this id was dropped before it was answered. Called once for such a request, and
   * never for a request that was answered. An answer given after this is ignored.
   *
   * @param id {@link ResourceRequest#getId()}
   */
  void cancel(long id);
}
