#import <MapLibre.h>
#import <XCTest/XCTest.h>

/// `MLNTileSource.prefetchZoomDelta` takes 0 to 255, the range of the core
/// value. A value outside it raises instead of being clamped, and leaves the
/// delta as it was.
@interface MLNTileSourcePrefetchZoomDeltaRangeTests : XCTestCase
@end

@implementation MLNTileSourcePrefetchZoomDeltaRangeTests

- (MLNVectorTileSource *)source {
    return [[MLNVectorTileSource alloc] initWithIdentifier:@"tiles"
                                          tileURLTemplates:@[ @"http://example.invalid/{z}/{x}/{y}.pbf" ]
                                                   options:nil];
}

- (void)testTheEndsOfTheRangeAreTaken {
    MLNVectorTileSource *source = [self source];
    source.prefetchZoomDelta = @0;
    XCTAssertEqualObjects(source.prefetchZoomDelta, @0);
    source.prefetchZoomDelta = @255;
    XCTAssertEqualObjects(source.prefetchZoomDelta, @255);
}

- (void)testANegativeDeltaRaises {
    MLNVectorTileSource *source = [self source];
    source.prefetchZoomDelta = @2;
    XCTAssertThrowsSpecificNamed(source.prefetchZoomDelta = @-1, NSException, NSInvalidArgumentException);
    XCTAssertEqualObjects(source.prefetchZoomDelta, @2, @"the delta is unchanged");
}

- (void)testADeltaAbove255Raises {
    MLNVectorTileSource *source = [self source];
    XCTAssertThrowsSpecificNamed(source.prefetchZoomDelta = @256, NSException, NSInvalidArgumentException);
    XCTAssertNil(source.prefetchZoomDelta, @"the delta is unchanged");
}

- (void)testAFractionalDeltaRaises {
    MLNVectorTileSource *source = [self source];
    XCTAssertThrowsSpecificNamed(source.prefetchZoomDelta = @1.5, NSException, NSInvalidArgumentException);
    XCTAssertNil(source.prefetchZoomDelta, @"the delta is unchanged");
}

@end
