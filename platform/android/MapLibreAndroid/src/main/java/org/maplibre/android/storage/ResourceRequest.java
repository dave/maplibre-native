package org.maplibre.android.storage;

import androidx.annotation.Keep;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import org.maplibre.android.LibraryLoader;

/**
 * One request to a {@link ResourceProvider}. Each request has its own object and id.
 * <p>
 * The answers are safe on any thread, and the first one wins. A later answer does nothing but log,
 * and an answer after {@link ResourceProvider#cancel(long)} is ignored.
 * </p>
 * <p>
 * A tile asks in two steps: {@link #isLocalOnly()} true, then a fetch unless the first step gave
 * data or "empty". Any other resource asks once, with {@link #isLocalOnly()} false. A tile whose
 * answer was data or "empty" is not asked for again, even when it is taken back from the tile
 * cache: a URL names its data, so a new URL is the way to new data.
 * </p>
 * <table>
 *   <caption>What the map sees for each answer</caption>
 *   <tr><th>Answer</th><th>Local-only step</th><th>Fetch step, tile</th><th>Fetch step, other</th></tr>
 *   <tr><td>data</td><td>data</td><td>data</td><td>data</td></tr>
 *   <tr><td>empty</td><td>no content</td><td>no content</td><td>no content</td></tr>
 *   <tr><td>unavailable</td><td>not stored</td><td>connection error</td><td>connection error</td></tr>
 *   <tr><td>not found</td><td>not stored</td><td>no content</td><td>not-found error</td></tr>
 *   <tr><td>error</td><td>not stored</td><td>other error</td><td>other error</td></tr>
 * </table>
 */
@Keep
public final class ResourceRequest {

  static {
    LibraryLoader.load();
  }

  /**
   * The tile a request asks for.
   */
  public static final class Tile {
    private final String urlTemplate;
    private final int pixelRatio;
    private final int x;
    private final int y;
    private final int z;

    Tile(@NonNull String urlTemplate, int pixelRatio, int x, int y, int z) {
      this.urlTemplate = urlTemplate;
      this.pixelRatio = pixelRatio;
      this.x = x;
      this.y = y;
      this.z = z;
    }

    /**
     * @return the source's URL template that made the request's URL
     */
    @NonNull
    public String getUrlTemplate() {
      return urlTemplate;
    }

    /**
     * @return the pixel ratio asked for
     */
    public int getPixelRatio() {
      return pixelRatio;
    }

    /**
     * @return the tile's column
     */
    public int getX() {
      return x;
    }

    /**
     * @return the tile's row
     */
    public int getY() {
      return y;
    }

    /**
     * @return the tile's zoom
     */
    public int getZ() {
      return z;
    }
  }

  @Keep
  private long nativePtr;

  private final long id;
  private final int kind;
  private final String url;
  private final String sourceId;
  @Nullable
  private final Tile tile;
  private final boolean localOnly;
  private final boolean lowPriority;

  ResourceRequest(long id, int kind, @NonNull String url, @NonNull String sourceId, @Nullable Tile tile,
                  boolean localOnly, boolean lowPriority, long responder) {
    this.id = id;
    this.kind = kind;
    this.url = url;
    this.sourceId = sourceId;
    this.tile = tile;
    this.localOnly = localOnly;
    this.lowPriority = lowPriority;
    initialize(responder);
  }

  /**
   * @return the request's id: process-wide and increasing, never 0 and never reused
   */
  public long getId() {
    return id;
  }

  /**
   * @return what is asked for, one of the {@link Resource} kinds
   */
  @Resource.Kind
  public int getKind() {
    return kind;
  }

  /**
   * @return the full URL, with its query
   */
  @NonNull
  public String getUrl() {
    return url;
  }

  /**
   * @return the style source that asked; empty for glyphs, sprites, images and styles
   */
  @NonNull
  public String getSourceId() {
    return sourceId;
  }

  /**
   * @return the tile asked for; null if this is not a tile
   */
  @Nullable
  public Tile getTile() {
    return tile;
  }

  /**
   * @return true to read stored data only, and do no slow work
   */
  public boolean isLocalOnly() {
    return localOnly;
  }

  /**
   * @return true if this request is for data the map does not need now, such as a prefetch
   */
  public boolean isLowPriority() {
    return lowPriority;
  }

  /**
   * The resource's decoded bytes (no transfer encoding).
   *
   * @param bytes the bytes, which are copied
   * @throws IllegalArgumentException if bytes is null
   */
  public void answerData(@NonNull byte[] bytes) {
    if (bytes == null) {
      throw new IllegalArgumentException("answerData: bytes is null for " + url);
    }
    nativeAnswerData(bytes);
  }

  /**
   * The resource exists and holds nothing.
   */
  public void answerEmpty() {
    nativeAnswerEmpty();
  }

  /**
   * There is no data now, for a reason that can pass: offline, a timeout.
   *
   * @param why the reason
   * @throws IllegalArgumentException if why is null
   */
  public void answerUnavailable(@NonNull String why) {
    nativeAnswerUnavailable(checkWhy(why));
  }

  /**
   * The resource does not exist.
   *
   * @param why the reason
   * @throws IllegalArgumentException if why is null
   */
  public void answerNotFound(@NonNull String why) {
    nativeAnswerNotFound(checkWhy(why));
  }

  /**
   * A defect, such as a storage or decode failure.
   *
   * @param why the reason
   * @throws IllegalArgumentException if why is null
   */
  public void answerError(@NonNull String why) {
    nativeAnswerError(checkWhy(why));
  }

  @NonNull
  private String checkWhy(@Nullable String why) {
    if (why == null) {
      throw new IllegalArgumentException("the reason is null for " + url);
    }
    return why;
  }

  @Keep
  private native void initialize(long responder);

  @Keep
  @Override
  protected native void finalize() throws Throwable;

  @Keep
  private native void nativeAnswerData(byte[] bytes);

  @Keep
  private native void nativeAnswerEmpty();

  @Keep
  private native void nativeAnswerUnavailable(String why);

  @Keep
  private native void nativeAnswerNotFound(String why);

  @Keep
  private native void nativeAnswerError(String why);
}
