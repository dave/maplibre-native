import MapLibre
import XCTest

/// A tile source's own prefetch zoom delta (`MLNTileSource.prefetchZoomDelta`),
/// over core `Source::setPrefetchZoomDelta`. `0` turns prefetching off for one
/// source, so it does not load a low-zoom tile that covers its whole dataset.
class MLNTileSourcePrefetchTests: XCTestCase {
    func testThePrefetchDeltaDefaultsToTheMapsAndIsSetPerSource() {
        let source = MLNVectorTileSource(
            identifier: "items", tileURLTemplates: ["http://example.invalid/{z}/{x}/{y}.pbf"], options: nil
        )
        XCTAssertNil(source.prefetchZoomDelta, "nil: the map's setting")
        source.prefetchZoomDelta = 0
        XCTAssertEqual(source.prefetchZoomDelta, 0)
        source.prefetchZoomDelta = 2
        XCTAssertEqual(source.prefetchZoomDelta, 2)
        source.prefetchZoomDelta = nil
        XCTAssertNil(source.prefetchZoomDelta)
    }
}
