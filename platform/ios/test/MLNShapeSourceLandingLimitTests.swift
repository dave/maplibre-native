import MapLibre
import XCTest

/// `MLNShapeSource.landingLimit` over core `GeoJSONSource::setLandingLimit`.
/// Viatrail holds its label sources at a generation while a tile swap waits,
/// so the labels land in the swap's frame.
class MLNShapeSourceLandingLimitTests: XCTestCase {
    func testTheLandingLimitIsNoneByDefaultAndRoundTrips() {
        let source = MLNShapeSource(identifier: "labels", shape: nil, options: [.labelLane: true])
        XCTAssertNil(source.landingLimit)
        source.landingLimit = 3
        XCTAssertEqual(source.landingLimit, 3)
        source.landingLimit = nil
        XCTAssertNil(source.landingLimit)
    }
}
