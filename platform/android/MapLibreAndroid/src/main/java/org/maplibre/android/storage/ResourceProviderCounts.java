package org.maplibre.android.storage;

/**
 * A provider's requests from start until answer or cancel. Nothing limits them.
 *
 * @see ResourceProviders#getCounts(String)
 */
public final class ResourceProviderCounts {
  private final long inFlight;
  private final long peakInFlight;

  ResourceProviderCounts(long inFlight, long peakInFlight) {
    this.inFlight = inFlight;
    this.peakInFlight = peakInFlight;
  }

  /**
   * @return the requests in flight now
   */
  public long getInFlight() {
    return inFlight;
  }

  /**
   * @return the most in flight at once since the provider was registered
   */
  public long getPeakInFlight() {
    return peakInFlight;
  }
}
