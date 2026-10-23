#import <Mapbox.h>
#import <XCTest/XCTest.h>

/// `keepsRenderingOffWindow`: a map whose view tree leaves the window (as
/// under a pushed navigation page) keeps its frames and its rendering
/// callbacks; without the flag it stops until it is back in a window.
@interface MLNMapViewOffWindowRenderingTests : XCTestCase <MLNMapViewDelegate>

@property (nonatomic) UIWindow *window;
/// Each map sits in its own host view. The host leaves the window and the map
/// keeps its superview, as a map under a pushed page does.
@property (nonatomic) MLNMapView *flaggedMap;
@property (nonatomic) UIView *flaggedHost;
@property (nonatomic) MLNMapView *plainMap;
@property (nonatomic) UIView *plainHost;
@property (nonatomic) XCTestExpectation *styleLoadingExpectation;
@property (nonatomic) NSInteger stylesToLoad;

/// The map, source and generation a test waits for.
@property (nonatomic, weak) MLNMapView *waitedMap;
@property (nonatomic, copy) NSString *waitedSource;
@property (nonatomic) uint64_t waitedGeneration;
@property (nonatomic) XCTestExpectation *laidOutExpectation;
/// Whether the waited map had a window when its body was laid out.
@property (nonatomic) BOOL laidOutInWindow;

/// Frames each map finished since the test began counting.
@property (nonatomic) NSInteger flaggedFrames;
@property (nonatomic) NSInteger plainFrames;
/// Frames the flagged map finished with no window.
@property (nonatomic) NSInteger flaggedFramesOffWindow;

@end

@implementation MLNMapViewOffWindowRenderingTests

- (void)setUp {
    [super setUp];
    NSURL *styleURL = [[NSBundle bundleForClass:[self class]] URLForResource:@"one-liner" withExtension:@"json"];
    CGRect frame = CGRectMake(0, 0, 400, 400);
    self.window = [[UIWindow alloc] initWithFrame:frame];
    [self.window makeKeyAndVisible];

    self.stylesToLoad = 2;
    self.styleLoadingExpectation = [self expectationWithDescription:@"both maps load their style"];
    self.flaggedHost = [[UIView alloc] initWithFrame:frame];
    self.flaggedMap = [[MLNMapView alloc] initWithFrame:frame styleURL:styleURL];
    self.flaggedMap.delegate = self;
    self.flaggedMap.keepsRenderingOffWindow = YES;
    [self.flaggedHost addSubview:self.flaggedMap];
    [self.window addSubview:self.flaggedHost];

    self.plainHost = [[UIView alloc] initWithFrame:frame];
    self.plainMap = [[MLNMapView alloc] initWithFrame:frame styleURL:styleURL];
    self.plainMap.delegate = self;
    [self.plainHost addSubview:self.plainMap];
    [self.window addSubview:self.plainHost];
    [self waitForExpectationsWithTimeout:10 handler:nil];
}

- (void)tearDown {
    // The display link keeps an off-window map alive while the flag is set.
    self.flaggedMap.keepsRenderingOffWindow = NO;
    self.plainMap.keepsRenderingOffWindow = NO;
    [self.flaggedMap removeFromSuperview];
    [self.plainMap removeFromSuperview];
    self.flaggedMap = nil;
    self.plainMap = nil;
    self.window = nil;
    [super tearDown];
}

- (void)mapView:(MLNMapView *)mapView didFinishLoadingStyle:(MLNStyle *)style {
    if (--self.stylesToLoad == 0) {
        [self.styleLoadingExpectation fulfill];
    }
}

- (void)mapViewDidFinishRenderingFrame:(MLNMapView *)mapView fullyRendered:(BOOL)fullyRendered {
    if (mapView == self.flaggedMap) {
        self.flaggedFrames++;
        if (!mapView.window) {
            self.flaggedFramesOffWindow++;
        }
    } else if (mapView == self.plainMap) {
        self.plainFrames++;
    }
}

- (void)mapView:(MLNMapView *)mapView sourceDidLayOut:(NSString *)sourceIdentifier generation:(uint64_t)generation {
    if (mapView != self.waitedMap || ![sourceIdentifier isEqualToString:self.waitedSource] ||
        generation != self.waitedGeneration) {
        return;
    }
    self.laidOutInWindow = mapView.window != nil;
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

/// The flagged map draws a change made while its view tree is off the window:
/// the body is laid out and frames finish, all with no window.
- (void)testAFlaggedMapDrawsOffItsWindow {
    [self.flaggedHost removeFromSuperview];
    XCTAssertNil(self.flaggedMap.window);
    XCTAssertEqual(self.flaggedMap.superview, self.flaggedHost);
    self.flaggedFramesOffWindow = 0;

    MLNShapeSource *source = [self addSource:@"off-window" toMap:self.flaggedMap];
    [self waitForLaidOut:source onMap:self.flaggedMap];
    XCTAssertFalse(self.laidOutInWindow, @"the body is laid out off the window");
    XCTAssertGreaterThan(self.flaggedFramesOffWindow, 0, @"frames finish off the window");
}

/// Without the flag the map stops off its window. The flagged map, given the
/// same change at the same time, is the clock: once it has laid out its body,
/// the plain map has drawn nothing. Back in the window, the plain map draws.
- (void)testAPlainMapStopsOffItsWindowUntilItIsBack {
    [self.flaggedHost removeFromSuperview];
    [self.plainHost removeFromSuperview];
    XCTAssertNil(self.plainMap.window);
    self.plainFrames = 0;

    MLNShapeSource *plainSource = [self addSource:@"plain" toMap:self.plainMap];
    MLNShapeSource *flaggedSource = [self addSource:@"flagged" toMap:self.flaggedMap];
    [self waitForLaidOut:flaggedSource onMap:self.flaggedMap];
    XCTAssertEqual(self.plainFrames, 0, @"the plain map draws no frame off its window");

    [self.window addSubview:self.plainHost];
    [self waitForLaidOut:plainSource onMap:self.plainMap];
    XCTAssertTrue(self.laidOutInWindow, @"the plain map lays out the body only back in the window");
    XCTAssertGreaterThan(self.plainFrames, 0);
}

/// Clearing the flag off the window stops the map at once.
- (void)testClearingTheFlagOffTheWindowStopsTheMap {
    [self.flaggedHost removeFromSuperview];
    [self.plainHost removeFromSuperview];
    self.flaggedMap.keepsRenderingOffWindow = NO;
    self.plainMap.keepsRenderingOffWindow = YES;
    // The plain map left its window without the flag: it has nothing to keep.
    [self.window addSubview:self.plainHost];
    [self.plainHost removeFromSuperview];
    self.flaggedFrames = 0;

    MLNShapeSource *stopped = [self addSource:@"stopped" toMap:self.flaggedMap];
    MLNShapeSource *clock = [self addSource:@"clock" toMap:self.plainMap];
    [self waitForLaidOut:clock onMap:self.plainMap];
    XCTAssertFalse(self.laidOutInWindow);
    XCTAssertEqual(self.flaggedFrames, 0, @"a map whose flag was cleared off its window draws nothing");

    [self.window addSubview:self.flaggedHost];
    [self waitForLaidOut:stopped onMap:self.flaggedMap];
    XCTAssertTrue(self.laidOutInWindow);
}

@end
