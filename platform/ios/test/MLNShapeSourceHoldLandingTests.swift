import MapLibre
import XCTest

/// `MLNShapeSource.holdsLanding` over core `GeoJSONSource::setHoldLanding`.
/// Viatrail holds its label sources while a tile swap waits, so the labels
/// land in the swap's frame.
class MLNShapeSourceHoldLandingTests: XCTestCase {
    func testHoldsLandingIsOffByDefaultAndRoundTrips() {
        let source = MLNShapeSource(identifier: "labels", shape: nil, options: [.labelLane: true])
        XCTAssertFalse(source.holdsLanding)
        source.holdsLanding = true
        XCTAssertTrue(source.holdsLanding)
        source.holdsLanding = false
        XCTAssertFalse(source.holdsLanding)
    }
}
