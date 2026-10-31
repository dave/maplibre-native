#import <Mapbox.h>
#import <XCTest/XCTest.h>

@interface MLNMapView (HiddenTests)
@property (nonatomic) CADisplayLink *displayLink;
@end

/// `setHidden:` (Viatrail #170 F1 pin): a hidden map pauses its display link
/// and draws no frame; shown again, it resumes and draws what changed. A
/// second map, shown in the same window, is the clock: once it has laid out a
/// body set at the same time, the hidden map has drawn nothing.
@interface MLNMapViewHiddenTests : XCTestCase <MLNMapViewDelegate>

@property (nonatomic) UIWindow *window;
@property (nonatomic) MLNMapView *hiddenMap;
@property (nonatomic) MLNMapView *clockMap;
@property (nonatomic) XCTestExpectation *styleLoadingExpectation;
@property (nonatomic) NSInteger stylesToLoad;

@property (nonatomic, weak) MLNMapView *waitedMap;
@property (nonatomic, copy) NSString *waitedSource;
@property (nonatomic) uint64_t waitedGeneration;
@property (nonatomic) XCTestExpectation *laidOutExpectation;

/// Frames and laid-out reports of the hidden map since the test began counting.
@property (nonatomic) NSInteger hiddenFrames;
@property (nonatomic) NSInteger hiddenReports;

@end

@implementation MLNMapViewHiddenTests

- (void)setUp {
    [super setUp];
    NSURL *styleURL = [[NSBundle bundleForClass:[self class]] URLForResource:@"one-liner" withExtension:@"json"];
    CGRect frame = CGRectMake(0, 0, 400, 400);
    self.window = [[UIWindow alloc] initWithFrame:frame];
    [self.window makeKeyAndVisible];

    self.stylesToLoad = 2;
    self.styleLoadingExpectation = [self expectationWithDescription:@"both maps load their style"];
    self.hiddenMap = [[MLNMapView alloc] initWithFrame:frame styleURL:styleURL];
    self.hiddenMap.delegate = self;
    [self.window addSubview:self.hiddenMap];
    self.clockMap = [[MLNMapView alloc] initWithFrame:frame styleURL:styleURL];
    self.clockMap.delegate = self;
    [self.window addSubview:self.clockMap];
    [self waitForExpectationsWithTimeout:10 handler:nil];
}

- (void)tearDown {
    [self.hiddenMap removeFromSuperview];
    [self.clockMap removeFromSuperview];
    self.hiddenMap = nil;
    self.clockMap = nil;
    self.window = nil;
    [super tearDown];
}

- (void)mapView:(MLNMapView *)mapView didFinishLoadingStyle:(MLNStyle *)style {
    if (--self.stylesToLoad == 0) {
        [self.styleLoadingExpectation fulfill];
    }
}

- (void)mapViewDidFinishRenderingFrame:(MLNMapView *)mapView fullyRendered:(BOOL)fullyRendered {
    if (mapView == self.hiddenMap) {
        self.hiddenFrames++;
    }
}

- (void)mapView:(MLNMapView *)mapView sourceDidLayOut:(NSString *)sourceIdentifier generation:(uint64_t)generation {
    if (mapView == self.hiddenMap) {
        self.hiddenReports++;
    }
    if (mapView != self.waitedMap || ![sourceIdentifier isEqualToString:self.waitedSource] ||
        generation != self.waitedGeneration) {
        return;
    }
    [self.laidOutExpectation fulfill];
}

- (MLNShapeSource *)addSource:(NSString *)identifier toMap:(MLNMapView *)mapView {
    MLNPointFeature *point = [[MLNPointFeature alloc] init];
    point.coordinate = mapView.centerCoordinate;
    MLNShapeSource *source = [[MLNShapeSource alloc] initWithIdentifier:identifier shape:point options:nil];
    [mapView.style addSource:source];
    MLNCircleStyleLayer *layer = [[MLNCircleStyleLayer alloc] initWithIdentifier:[identifier stringByAppendingString:@"-layer"]
                                                                          source:source];
    [mapView.style addLayer:layer];
    return source;
}

- (void)waitForLaidOut:(MLNShapeSource *)source onMap:(MLNMapView *)mapView {
    self.waitedMap = mapView;
    self.waitedSource = source.identifier;
    self.waitedGeneration = source.dataGeneration;
    self.laidOutExpectation = [self expectationWithDescription:@"the source's body is laid out"];
    [self waitForExpectationsWithTimeout:10 handler:nil];
}

- (void)testHidingAMapPausesItsDisplayLinkAndShowingItResumesIt {
    CADisplayLink *link = self.hiddenMap.displayLink;
    XCTAssertNotNil(link, @"a map in a window has a display link");
    XCTAssertFalse(link.paused);

    self.hiddenMap.hidden = YES;
    XCTAssertEqual(self.hiddenMap.displayLink, link, @"hiding keeps the display link");
    XCTAssertTrue(link.paused, @"hiding pauses the display link");

    self.hiddenMap.hidden = NO;
    XCTAssertEqual(self.hiddenMap.displayLink, link);
    XCTAssertFalse(link.paused, @"showing resumes the display link");
}

- (void)testAHiddenMapDrawsNoFrameUntilItIsShown {
    self.hiddenMap.hidden = YES;
    self.hiddenFrames = 0;
    self.hiddenReports = 0;

    MLNShapeSource *waiting = [self addSource:@"waiting" toMap:self.hiddenMap];
    MLNShapeSource *clock = [self addSource:@"clock" toMap:self.clockMap];
    [self waitForLaidOut:clock onMap:self.clockMap];
    XCTAssertEqual(self.hiddenFrames, 0, @"a hidden map draws no frame");
    XCTAssertEqual(self.hiddenReports, 0, @"a hidden map lays nothing out");

    self.hiddenMap.hidden = NO;
    [self waitForLaidOut:waiting onMap:self.hiddenMap];
    XCTAssertGreaterThan(self.hiddenFrames, 0, @"shown again, the map draws");
}

@end
