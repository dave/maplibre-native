#import <Mapbox.h>
#import <XCTest/XCTest.h>

/// A source added to a style keeps its Objective-C object for as long as it is
/// in the style (Viatrail fork, 2026-10-16): a lookup returns the same object,
/// and state kept on it (`MLNShapeSource.shape`) survives every other
/// reference being dropped. With only the weak core-to-object link, an
/// optimised build freed the object at once and a lookup made a new one whose
/// `shape` was nil. Removed from the style, the object is freed again.
@interface MLNSourceLifetimeTests : XCTestCase <MLNMapViewDelegate>

@property (nonatomic) MLNMapView *mapView;
@property (nonatomic) XCTestExpectation *styleLoadingExpectation;

@end

@implementation MLNSourceLifetimeTests

- (void)setUp {
    [super setUp];
    NSURL *styleURL = [[NSBundle bundleForClass:[self class]] URLForResource:@"one-liner" withExtension:@"json"];
    self.mapView = [[MLNMapView alloc] initWithFrame:CGRectMake(0, 0, 256, 256) styleURL:styleURL];
    self.mapView.delegate = self;
    if (!self.mapView.style) {
        _styleLoadingExpectation = [self expectationWithDescription:@"Map view should finish loading style."];
        [self waitForExpectationsWithTimeout:10 handler:nil];
        _styleLoadingExpectation = nil;
    }
}

- (void)tearDown {
    self.mapView = nil;
    [super tearDown];
}

- (void)mapView:(MLNMapView *)mapView didFinishLoadingStyle:(MLNStyle *)style {
    [_styleLoadingExpectation fulfill];
}

- (void)testAnAddedSourceKeepsItsObjectAndItsShape {
    __weak MLNShapeSource *weakSource = nil;
    @autoreleasepool {
        MLNPointFeature *point = [[MLNPointFeature alloc] init];
        point.coordinate = CLLocationCoordinate2DMake(46, 8);
        MLNShapeSource *source = [[MLNShapeSource alloc] initWithIdentifier:@"kept" shape:point options:nil];
        [self.mapView.style addSource:source];
        weakSource = source;
    }
    XCTAssertNotNil(weakSource, @"the style keeps the added source's object");
    MLNShapeSource *found = (MLNShapeSource *)[self.mapView.style sourceWithIdentifier:@"kept"];
    XCTAssertEqual(found, weakSource, @"a lookup returns the same object");
    XCTAssertNotNil(found.shape, @"and its shape");
    XCTAssertEqualWithAccuracy(found.shape.coordinate.latitude, 46, 1e-9);
}

- (void)testALookedUpSourceKeepsItsObject {
    __weak MLNSource *weakSource = nil;
    @autoreleasepool {
        MLNShapeSource *source = [[MLNShapeSource alloc] initWithIdentifier:@"looked-up" shape:nil options:nil];
        [self.mapView.style addSource:source];
    }
    @autoreleasepool {
        weakSource = [self.mapView.style sourceWithIdentifier:@"looked-up"];
    }
    XCTAssertNotNil(weakSource);
    XCTAssertEqual([self.mapView.style sourceWithIdentifier:@"looked-up"], weakSource);
}

- (void)testARemovedSourceIsFreed {
    __weak MLNShapeSource *weakSource = nil;
    @autoreleasepool {
        MLNShapeSource *source = [[MLNShapeSource alloc] initWithIdentifier:@"removed" shape:nil options:nil];
        [self.mapView.style addSource:source];
        weakSource = source;
    }
    @autoreleasepool {
        [self.mapView.style removeSource:[self.mapView.style sourceWithIdentifier:@"removed"]];
    }
    XCTAssertNil(weakSource, @"out of the style, nothing holds the object: no cycle");
    XCTAssertNil([self.mapView.style sourceWithIdentifier:@"removed"]);
}

- (void)testASourceIsFreedWithItsStyle {
    __weak MLNShapeSource *weakSource = nil;
    @autoreleasepool {
        MLNShapeSource *source = [[MLNShapeSource alloc] initWithIdentifier:@"style-owned" shape:nil options:nil];
        [self.mapView.style addSource:source];
        weakSource = source;
    }
    XCTAssertNotNil(weakSource, @"the style keeps the added source's object");
    @autoreleasepool {
        // A new style replaces the old one's sources: the core source is
        // destroyed, and its peer with it.
        self.mapView.styleJSON = @"{\"version\": 8, \"sources\": {}, \"layers\": []}";
    }
    XCTAssertNil(weakSource, @"the core source destroyed, the source's object goes with it");
}

@end
