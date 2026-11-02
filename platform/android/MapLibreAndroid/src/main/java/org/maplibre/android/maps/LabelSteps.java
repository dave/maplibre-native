package org.maplibre.android.maps;

import androidx.annotation.Keep;
import androidx.annotation.NonNull;
import androidx.annotation.RestrictTo;

import org.maplibre.android.LibraryLoader;

/**
 * The core's step recorder, for debug probes and tests: when on, every GeoJSON source's
 * off-thread steps (a body's parse, its tiles' cuts, parse and layout) and its tiles' hops
 * record their thread and their times. Off by default. While it is off, a step costs one
 * relaxed atomic load and allocates nothing. A step sent to its thread while the recorder is
 * off is not recorded.
 * <p>
 * This is not public API: it may change or go away. A caller outside the library suppresses
 * the {@code RestrictedApi} lint check.
 */
@Keep
@RestrictTo(RestrictTo.Scope.LIBRARY_GROUP)
public final class LabelSteps {

  static {
    LibraryLoader.load();
  }

  private LabelSteps() {
  }

  /** Turns the recorder on or off. */
  public static void setRecording(boolean recording) {
    nativeSetRecording(recording);
  }

  /**
   * The steps recorded since the last call, one a line, tab-separated: source id, step name,
   * thread name, hop (0 or 1), queued, start and end (monotonic seconds; queued is 0 where
   * the recorder cannot see it).
   */
  @NonNull
  public static String take() {
    return nativeTake();
  }

  @Keep
  private static native void nativeSetRecording(boolean recording);

  @Keep
  @NonNull
  private static native String nativeTake();
}
