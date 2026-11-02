import MapLibre
import XCTest

/// `MLNShapeSource.landingLimit` over core `GeoJSONSource::setLandingLimit`:
/// an app holds a source at a generation while other work for the same frame
/// is not ready, so the source's new body lands in that frame.
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
