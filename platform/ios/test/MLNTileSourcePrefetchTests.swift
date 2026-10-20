import MapLibre
import XCTest

/// A tile source's own prefetch zoom delta (`MLNTileSource.prefetchZoomDelta`),
/// over core `Source::setPrefetchZoomDelta`. Viatrail sets 0 on its item
/// source so a swap does not wait for a z−4 tile that covers the whole dataset.
class MLNTileSourcePrefetchTests: XCTestCase {
    func testThePrefetchDeltaDefaultsToTheMapsAndIsSetPerSource() {
        let source = MLNVectorTileSource(
            identifier: "items", tileURLTemplates: ["http://example.invalid/{z}/{x}/{y}.pbf"], options: nil)
        XCTAssertNil(source.prefetchZoomDelta, "nil: the map's setting")
        source.prefetchZoomDelta = 0
        XCTAssertEqual(source.prefetchZoomDelta, 0)
        source.prefetchZoomDelta = 2
        XCTAssertEqual(source.prefetchZoomDelta, 2)
        source.prefetchZoomDelta = nil
        XCTAssertNil(source.prefetchZoomDelta)
    }
}
