#import <MapLibre.h>
#import <XCTest/XCTest.h>

/// `MLNShapeSource.landingLimit` is a data generation, so it is not negative.
/// A negative value raises instead of wrapping to a huge generation (which
/// would hold nothing, as no limit does), and leaves the limit as it was.
@interface MLNShapeSourceLandingLimitRangeTests : XCTestCase
@end

@implementation MLNShapeSourceLandingLimitRangeTests

- (void)testANegativeLimitRaises {
    MLNShapeSource *source = [[MLNShapeSource alloc] initWithIdentifier:@"shapes" shape:nil options:nil];
    source.landingLimit = @3;
    XCTAssertThrowsSpecificNamed(source.landingLimit = @-1, NSException, NSInvalidArgumentException);
    XCTAssertEqualObjects(source.landingLimit, @3, @"the limit is unchanged");
}

- (void)testAFractionalLimitRaises {
    MLNShapeSource *source = [[MLNShapeSource alloc] initWithIdentifier:@"shapes" shape:nil options:nil];
    XCTAssertThrowsSpecificNamed(source.landingLimit = @1.5, NSException, NSInvalidArgumentException);
    XCTAssertNil(source.landingLimit, @"the limit is unchanged");
}

- (void)testZeroAndNilAreTaken {
    MLNShapeSource *source = [[MLNShapeSource alloc] initWithIdentifier:@"shapes" shape:nil options:nil];
    source.landingLimit = @0;
    XCTAssertEqualObjects(source.landingLimit, @0);
    source.landingLimit = nil;
    XCTAssertNil(source.landingLimit);
}

@end
