#import <MapLibre.h>
#import <XCTest/XCTest.h>
#import <objc/runtime.h>

#if MLN_RENDER_BACKEND_METAL

/// Fulfills its expectation when it is freed: attached to an object, it is
/// freed with that object.
@interface MLNDeallocSignal : NSObject
@property (nonatomic) XCTestExpectation *expectation;
@end

@implementation MLNDeallocSignal
- (void)dealloc {
    [_expectation fulfill];
}
@end

static char MLNDeallocSignalKey;

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
    // Stop the maps that render off their window.
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
    [self waitForExpectations:@[ self.laidOutExpectation ] timeout:10];
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

/// Setting the flag on a map that is already off its window starts it at once.
- (void)testSettingTheFlagOffTheWindowStartsTheMap {
    [self.plainHost removeFromSuperview];
    XCTAssertNil(self.plainMap.window);
    self.plainMap.keepsRenderingOffWindow = YES;

    MLNShapeSource *source = [self addSource:@"late" toMap:self.plainMap];
    [self waitForLaidOut:source onMap:self.plainMap];
    XCTAssertFalse(self.laidOutInWindow, @"the body is laid out off the window");
}

/// The display link does not keep a map alive: a flagged map that renders
/// off its window is freed with its view tree, with the flag still set. The
/// map is made in this test's own autorelease pool, so nothing autoreleased
/// before the test holds it.
- (void)testAFlaggedMapOffItsWindowIsFreedWithItsViewTree {
    XCTestExpectation *freed = [self expectationWithDescription:@"the map is freed"];
    @autoreleasepool {
        NSURL *styleURL = [[NSBundle bundleForClass:[self class]] URLForResource:@"one-liner" withExtension:@"json"];
        UIView *host = [[UIView alloc] initWithFrame:self.window.bounds];
        MLNMapView *mapView = [[MLNMapView alloc] initWithFrame:host.bounds styleURL:styleURL];
        mapView.delegate = self;
        mapView.keepsRenderingOffWindow = YES;
        [host addSubview:mapView];
        self.stylesToLoad = 1;
        self.styleLoadingExpectation = [self expectationWithDescription:@"the map loads its style"];
        [self.window addSubview:host];
        [self waitForExpectations:@[ self.styleLoadingExpectation ] timeout:10];

        [host removeFromSuperview];
        MLNShapeSource *source = [self addSource:@"freed" toMap:mapView];
        [self waitForLaidOut:source onMap:mapView];
        XCTAssertFalse(self.laidOutInWindow, @"the map renders off its window");
        XCTAssertTrue(mapView.keepsRenderingOffWindow);

        MLNDeallocSignal *signal = [[MLNDeallocSignal alloc] init];
        signal.expectation = freed;
        objc_setAssociatedObject(mapView, &MLNDeallocSignalKey, signal, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    [self waitForExpectations:@[ freed ] timeout:10];
}

/// Set on a map in a superview that was never in a window, the flag cannot
/// start the map: there is no screen to render on. It raises, and stays off.
- (void)testSettingTheFlagOnAMapNeverInAWindowRaises {
    NSURL *styleURL = [[NSBundle bundleForClass:[self class]] URLForResource:@"one-liner" withExtension:@"json"];
    UIView *host = [[UIView alloc] initWithFrame:self.window.bounds];
    MLNMapView *mapView = [[MLNMapView alloc] initWithFrame:host.bounds styleURL:styleURL];
    [host addSubview:mapView];
    XCTAssertNil(mapView.window);

    XCTAssertThrowsSpecificNamed(mapView.keepsRenderingOffWindow = YES, NSException,
                                 NSInternalInconsistencyException);
    XCTAssertFalse(mapView.keepsRenderingOffWindow, @"the flag stays off");
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

#else

/// Only the Metal renderer draws off a window. On any other renderer the flag
/// raises, and stays off.
@interface MLNMapViewOffWindowRenderingTests : XCTestCase
@end

@implementation MLNMapViewOffWindowRenderingTests

- (void)testSettingTheFlagOnARendererThatCannotRaises {
    MLNMapView *mapView = [[MLNMapView alloc] initWithFrame:CGRectMake(0, 0, 400, 400)];
    XCTAssertThrowsSpecificNamed(mapView.keepsRenderingOffWindow = YES, NSException,
                                 NSInvalidArgumentException);
    XCTAssertFalse(mapView.keepsRenderingOffWindow, @"the flag stays off");
}

@end

#endif
