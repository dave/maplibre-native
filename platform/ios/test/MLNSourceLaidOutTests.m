#import <Mapbox.h>
#import <XCTest/XCTest.h>

/// The laid-out signal on iOS: `-mapView:sourceDidLayOut:generation:` comes
/// for a shape source's body once every tile in view is laid out with it, with
/// the generation `MLNShapeSource.dataGeneration` gave, on the label lane too.
@interface MLNSourceLaidOutTests : XCTestCase <MLNMapViewDelegate>

@property (nonatomic) MLNMapView *mapView;
/// The map draws only in a window.
@property (nonatomic) UIWindow *window;
@property (nonatomic) XCTestExpectation *styleLoadingExpectation;
@property (nonatomic) XCTestExpectation *laidOutExpectation;
@property (nonatomic, copy) NSString *waitedSource;
@property (nonatomic) uint64_t waitedGeneration;
@property (nonatomic) NSMutableArray<NSNumber *> *laidOut;

@end

@implementation MLNSourceLaidOutTests

- (void)setUp {
    [super setUp];
    NSURL *styleURL = [[NSBundle bundleForClass:[self class]] URLForResource:@"one-liner" withExtension:@"json"];
    self.mapView = [[MLNMapView alloc] initWithFrame:CGRectMake(0, 0, 400, 400) styleURL:styleURL];
    self.mapView.delegate = self;
    self.window = [[UIWindow alloc] initWithFrame:self.mapView.bounds];
    [self.window addSubview:self.mapView];
    [self.window makeKeyAndVisible];
    self.laidOut = [NSMutableArray array];
    if (!self.mapView.style) {
        _styleLoadingExpectation = [self expectationWithDescription:@"Map view should finish loading style."];
        [self waitForExpectationsWithTimeout:10 handler:nil];
    }
}

- (void)tearDown {
    [self.mapView removeFromSuperview];
    self.mapView = nil;
    self.window = nil;
    [super tearDown];
}

- (void)mapView:(MLNMapView *)mapView didFinishLoadingStyle:(MLNStyle *)style {
    [_styleLoadingExpectation fulfill];
}

- (void)mapView:(MLNMapView *)mapView sourceDidLayOut:(NSString *)sourceIdentifier generation:(uint64_t)generation {
    if (![sourceIdentifier isEqualToString:self.waitedSource]) {
        return;
    }
    [self.laidOut addObject:@(generation)];
    if (generation == self.waitedGeneration) {
        [self.laidOutExpectation fulfill];
    }
}

- (MLNShapeSource *)addSource:(NSString *)identifier options:(NSDictionary *)options {
    MLNPointFeature *point = [[MLNPointFeature alloc] init];
    point.coordinate = self.mapView.centerCoordinate;
    MLNShapeSource *source = [[MLNShapeSource alloc] initWithIdentifier:identifier shape:point options:options];
    [self.mapView.style addSource:source];
    MLNCircleStyleLayer *layer = [[MLNCircleStyleLayer alloc] initWithIdentifier:[identifier stringByAppendingString:@"-layer"]
                                                                          source:source];
    [self.mapView.style addLayer:layer];
    return source;
}

- (void)waitForLaidOut:(MLNShapeSource *)source {
    self.waitedSource = source.identifier;
    self.waitedGeneration = source.dataGeneration;
    self.laidOutExpectation = [self expectationWithDescription:@"the source's body is laid out"];
    [self waitForExpectationsWithTimeout:10 handler:nil];
}

- (void)testALaneSourcesBodyIsReportedLaidOutWithItsGeneration {
    MLNShapeSource *source = [self addSource:@"lane" options:@{MLNShapeSourceOptionLabelLane : @YES}];
    [self waitForLaidOut:source];

    MLNPointFeature *moved = [[MLNPointFeature alloc] init];
    moved.coordinate = CLLocationCoordinate2DMake(self.mapView.centerCoordinate.latitude + 0.001,
                                                  self.mapView.centerCoordinate.longitude);
    source.shape = moved;
    [self waitForLaidOut:source];
    // Once per generation, in order, never an older one after a newer one.
    NSArray *sorted = [self.laidOut sortedArrayUsingSelector:@selector(compare:)];
    XCTAssertEqualObjects(self.laidOut, sorted);
    XCTAssertEqual([NSSet setWithArray:self.laidOut].count, self.laidOut.count);
}

/// A filter change on a layer of the source moves its generation at once,
/// read from the core (so a second object for the same source agrees), and
/// it is reported laid out.
- (void)testAFilterChangeMovesTheGenerationAndIsReportedLaidOut {
    MLNShapeSource *source = [self addSource:@"lane-filter" options:@{MLNShapeSourceOptionLabelLane : @YES}];
    [self waitForLaidOut:source];
    uint64_t before = source.dataGeneration;
    MLNStyleLayer *layer = [self.mapView.style layerWithIdentifier:@"lane-filter-layer"];
    ((MLNVectorStyleLayer *)layer).predicate = [NSPredicate predicateWithFormat:@"kind != 'x'"];
    XCTAssertGreaterThan(source.dataGeneration, before, @"the filter change takes a generation at once");
    MLNShapeSource *again = (MLNShapeSource *)[self.mapView.style sourceWithIdentifier:@"lane-filter"];
    XCTAssertEqual(again.dataGeneration, source.dataGeneration, @"every object for the source answers the same");
    [self waitForLaidOut:source];
}

- (void)testAPlainSourceIsReportedToo {
    MLNShapeSource *source = [self addSource:@"plain" options:nil];
    [self waitForLaidOut:source];
}

@end
