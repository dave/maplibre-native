#import <MapLibre.h>
#import <XCTest/XCTest.h>
#import "MLNMockGestureRecognizers.h"

@interface MLNMapView (MLNMapViewGlideEndTests)
- (void)handlePanGesture:(UIPanGestureRecognizer *)pan;
- (void)handlePinchGesture:(UIPinchGestureRecognizer *)pinch;
- (void)handleRotateGesture:(UIRotationGestureRecognizer *)rotate;
- (void)handleDoubleTapGesture:(UITapGestureRecognizer *)doubleTap;
- (void)handleTwoFingerTapGesture:(UITapGestureRecognizer *)twoFingerTap;
@end

/// A pan gesture whose end velocity the test sets.
@interface MLNGlidePanMock : UIPanGestureRecognizer
@property (nonatomic, readwrite) UIGestureRecognizerState state;
@property (nonatomic) CGPoint velocityOverride;
@end

@implementation MLNGlidePanMock
@synthesize state;
- (CGPoint)velocityInView:(UIView *)view {
    return self.velocityOverride;
}
- (CGPoint)translationInView:(UIView *)view {
    return CGPointZero;
}
@end

/// A rotation gesture whose end velocity the test sets.
@interface MLNGlideRotateMock : UIRotationGestureRecognizer
@property (nonatomic, readwrite) UIGestureRecognizerState state;
@property (nonatomic) CGFloat velocityOverride;
@end

@implementation MLNGlideRotateMock
@synthesize state;
- (CGFloat)velocity {
    return self.velocityOverride;
}
- (CGPoint)locationInView:(UIView *)view {
    return view.center;
}
@end

/// One `-mapView:willGlideToCenter:...` call.
@interface MLNGlideReport : NSObject
@property (nonatomic) CLLocationCoordinate2D center;
@property (nonatomic) double zoomLevel;
@property (nonatomic) CLLocationDirection direction;
@property (nonatomic) CGFloat pitch;
@property (nonatomic) NSTimeInterval duration;
@end

@implementation MLNGlideReport
@end

/// `-mapView:willGlideToCenter:zoomLevel:direction:pitch:duration:` reports
/// the camera a glide ends at: a pan fling, a pinch's zoom fling, the double
/// tap and two-finger tap zooms, and
/// `-setZoomLevel:aroundAnchorPoint:duration:animationTimingFunction:` with a
/// duration. Each report equals the camera once the glide has ended. It is
/// made after the glide has started, so a delegate that moves the camera in
/// it cuts the glide short instead of moving its start. A fling's report comes
/// after the other gestures of its lift (the gesture handlers of one touch
/// event run in one main-loop turn) and before the glide's first frame. Two
/// glides that start at one lift cut each other short: neither is reported.
@interface MLNMapViewGlideEndTests : XCTestCase <MLNMapViewDelegate>
@property (nonatomic) UIWindow *window;
@property (nonatomic) MLNMapView *mapView;
@property (nonatomic) XCTestExpectation *styleLoaded;
@property (nonatomic) XCTestExpectation *idle;
@property (nonatomic) NSMutableArray<MLNGlideReport *> *reports;
/// If valid, the delegate moves the camera here in the report.
@property (nonatomic) CLLocationCoordinate2D jumpInReport;
/// Frames that finished between the last gesture and the first report.
@property (nonatomic) NSInteger framesSinceGesture;
@property (nonatomic) NSInteger framesBeforeReport;
@end

@implementation MLNMapViewGlideEndTests

- (void)setUp {
    [super setUp];
    NSURL *styleURL = [[NSBundle bundleForClass:[self class]] URLForResource:@"one-liner" withExtension:@"json"];
    self.window = [[UIWindow alloc] initWithFrame:CGRectMake(0, 0, 400, 400)];
    [self.window makeKeyAndVisible];
    self.mapView = [[MLNMapView alloc] initWithFrame:self.window.bounds styleURL:styleURL];
    self.mapView.delegate = self;
    [self.window addSubview:self.mapView];
    self.styleLoaded = [self expectationWithDescription:@"the style loads"];
    [self waitForExpectations:@[ self.styleLoaded ] timeout:10];
    [self.mapView setCenterCoordinate:CLLocationCoordinate2DMake(47.3, 8.5) zoomLevel:10 animated:NO];
    self.reports = [NSMutableArray array];
    self.jumpInReport = kCLLocationCoordinate2DInvalid;
}

- (void)tearDown {
    [self.mapView removeFromSuperview];
    self.mapView = nil;
    self.window = nil;
    [super tearDown];
}

- (void)mapView:(MLNMapView *)mapView didFinishLoadingStyle:(MLNStyle *)style {
    [self.styleLoaded fulfill];
}

- (void)mapViewDidFinishRenderingFrame:(MLNMapView *)mapView fullyRendered:(BOOL)fullyRendered {
    self.framesSinceGesture++;
}

- (void)mapViewDidBecomeIdle:(MLNMapView *)mapView {
    [self.idle fulfill];
    self.idle = nil;
}

- (void)mapView:(MLNMapView *)mapView
    willGlideToCenter:(CLLocationCoordinate2D)center
            zoomLevel:(double)zoomLevel
            direction:(CLLocationDirection)direction
                pitch:(CGFloat)pitch
             duration:(NSTimeInterval)duration {
    MLNGlideReport *report = [[MLNGlideReport alloc] init];
    report.center = center;
    report.zoomLevel = zoomLevel;
    report.direction = direction;
    report.pitch = pitch;
    report.duration = duration;
    [self.reports addObject:report];
    self.framesBeforeReport = self.framesSinceGesture;
    if (CLLocationCoordinate2DIsValid(self.jumpInReport)) {
        [mapView setCenterCoordinate:self.jumpInReport animated:NO];
    }
}

/// Waits for the map to be idle: every glide has ended.
- (void)waitForIdle {
    self.idle = [self expectationWithDescription:@"the map is idle"];
    [self waitForExpectations:@[ self.idle ] timeout:10];
}

/// Runs what the main queue holds now: the turn after the gestures.
- (void)drainMainQueue {
    XCTestExpectation *drained = [self expectationWithDescription:@"the main queue is drained"];
    dispatch_async(dispatch_get_main_queue(), ^{
        [drained fulfill];
    });
    [self waitForExpectations:@[ drained ] timeout:10];
}

/// The one report equals the camera the map is at.
- (void)assertTheReportIsTheCamera {
    XCTAssertEqual(self.reports.count, 1u);
    MLNGlideReport *report = self.reports.firstObject;
    XCTAssertEqualWithAccuracy(report.center.latitude, self.mapView.centerCoordinate.latitude, 1e-9);
    XCTAssertEqualWithAccuracy(report.center.longitude, self.mapView.centerCoordinate.longitude, 1e-9);
    XCTAssertEqualWithAccuracy(report.zoomLevel, self.mapView.zoomLevel, 1e-9);
    XCTAssertEqualWithAccuracy(report.direction, self.mapView.direction, 1e-9);
    XCTAssertEqualWithAccuracy(report.pitch, self.mapView.camera.pitch, 1e-9);
}

- (MLNGlidePanMock *)beginPan {
    MLNGlidePanMock *pan = [[MLNGlidePanMock alloc] initWithTarget:nil action:nil];
    [self.mapView addGestureRecognizer:pan];
    pan.state = UIGestureRecognizerStateBegan;
    [self.mapView handlePanGesture:pan];
    return pan;
}

- (void)endPan:(MLNGlidePanMock *)pan {
    pan.state = UIGestureRecognizerStateEnded;
    pan.velocityOverride = CGPointMake(900, -400);
    [self.mapView handlePanGesture:pan];
    self.framesSinceGesture = 0;
}

- (UIPinchGestureRecognizerMock *)beginPinch {
    UIPinchGestureRecognizerMock *pinch = [[UIPinchGestureRecognizerMock alloc] initWithTarget:nil action:nil];
    [self.mapView addGestureRecognizer:pinch];
    pinch.locationInViewOverride = CGPointMake(300, 120);
    pinch.state = UIGestureRecognizerStateBegan;
    pinch.scale = 1;
    [self.mapView handlePinchGesture:pinch];
    return pinch;
}

- (void)endPinch:(UIPinchGestureRecognizerMock *)pinch velocity:(CGFloat)velocity {
    pinch.state = UIGestureRecognizerStateEnded;
    pinch.velocity = velocity;
    [self.mapView handlePinchGesture:pinch];
    self.framesSinceGesture = 0;
}

- (MLNGlideRotateMock *)beginRotate {
    MLNGlideRotateMock *rotate = [[MLNGlideRotateMock alloc] initWithTarget:nil action:nil];
    [self.mapView addGestureRecognizer:rotate];
    rotate.state = UIGestureRecognizerStateBegan;
    [self.mapView handleRotateGesture:rotate];
    return rotate;
}

- (void)endRotate:(MLNGlideRotateMock *)rotate {
    rotate.state = UIGestureRecognizerStateEnded;
    rotate.velocityOverride = 6;
    [self.mapView handleRotateGesture:rotate];
    self.framesSinceGesture = 0;
}

- (UITapGestureRecognizerMock *)tapAt:(CGPoint)point {
    UITapGestureRecognizerMock *tap = [[UITapGestureRecognizerMock alloc] initWithTarget:nil action:nil];
    tap.mockTappedView = self.mapView;
    tap.mockTappedPoint = point;
    return tap;
}

- (void)testAPanFlingReportsWhereItEnds {
    [self endPan:[self beginPan]];
    [self drainMainQueue];
    XCTAssertEqual(self.reports.count, 1u, @"reported in the turn after the gesture");
    XCTAssertEqual(self.framesBeforeReport, 0, @"reported before the glide's first frame");
    XCTAssertEqualWithAccuracy(self.reports.firstObject.duration, self.mapView.decelerationRate, 1e-9);
    [self waitForIdle];
    [self assertTheReportIsTheCamera];
}

- (void)testAPinchFlingReportsWhereItEnds {
    [self endPinch:[self beginPinch] velocity:6];
    [self drainMainQueue];
    XCTAssertEqual(self.reports.count, 1u, @"reported in the turn after the gesture");
    XCTAssertEqual(self.framesBeforeReport, 0, @"reported before the glide's first frame");
    XCTAssertGreaterThan(self.reports.firstObject.zoomLevel, 10);
    [self waitForIdle];
    [self assertTheReportIsTheCamera];
}

- (void)testADoubleTapReportsWhereItsZoomEnds {
    [self.mapView handleDoubleTapGesture:[self tapAt:CGPointMake(80, 300)]];
    XCTAssertEqual(self.reports.count, 1u, @"reported as the zoom starts");
    XCTAssertEqual(self.reports.firstObject.zoomLevel, 11);
    [self waitForIdle];
    [self assertTheReportIsTheCamera];
}

- (void)testATwoFingerTapReportsWhereItsZoomEnds {
    [self.mapView handleTwoFingerTapGesture:[self tapAt:CGPointMake(250, 60)]];
    XCTAssertEqual(self.reports.count, 1u, @"reported as the zoom starts");
    XCTAssertEqual(self.reports.firstObject.zoomLevel, 9);
    [self waitForIdle];
    [self assertTheReportIsTheCamera];
}

- (void)testAnAnchoredZoomWithADurationReportsWhereItEnds {
    [self.mapView setZoomLevel:11.6 aroundAnchorPoint:CGPointMake(320, 90) duration:0.2 animationTimingFunction:nil];
    XCTAssertEqual(self.reports.count, 1u, @"reported as the zoom starts");
    XCTAssertEqualWithAccuracy(self.reports.firstObject.duration, 0.2, 1e-9);
    [self waitForIdle];
    [self assertTheReportIsTheCamera];
}

- (void)testAnAnchoredZoomWithNoDurationIsNotAGlide {
    [self.mapView setZoomLevel:11.6 aroundAnchorPoint:CGPointMake(320, 90) duration:0 animationTimingFunction:nil];
    XCTAssertEqual(self.reports.count, 0u);
}

/// The report comes after the glide has started: a delegate that moves the
/// camera in it cuts the glide short, and the map stays where the delegate
/// put it. Reported before the start, the glide would run from there.
- (void)testADelegateThatMovesTheCameraInTheReportCutsTheGlideShort {
    CLLocationCoordinate2D there = CLLocationCoordinate2DMake(-12.5, 130.8);
    self.jumpInReport = there;
    [self endPan:[self beginPan]];
    [self drainMainQueue];
    XCTAssertEqual(self.reports.count, 1u);
    [self waitForIdle];
    XCTAssertEqualWithAccuracy(self.mapView.centerCoordinate.latitude, there.latitude, 1e-9);
    XCTAssertEqualWithAccuracy(self.mapView.centerCoordinate.longitude, there.longitude, 1e-9);
}

/// A rotation glide is not reported.
- (void)testARotationIsNotAGlide {
    [self endRotate:[self beginRotate]];
    [self drainMainQueue];
    XCTAssertEqual(self.reports.count, 0u);
}

/// A pan fling and a zoom fling at one lift, in either order: nothing.
- (void)testAPanFlingThenAPinchFlingAtOneLiftReportNothing {
    MLNGlidePanMock *pan = [self beginPan];
    UIPinchGestureRecognizerMock *pinch = [self beginPinch];
    [self endPan:pan];
    [self endPinch:pinch velocity:6];
    [self drainMainQueue];
    XCTAssertEqual(self.reports.count, 0u);
}

- (void)testAPinchFlingThenAPanFlingAtOneLiftReportNothing {
    MLNGlidePanMock *pan = [self beginPan];
    UIPinchGestureRecognizerMock *pinch = [self beginPinch];
    [self endPinch:pinch velocity:6];
    [self endPan:pan];
    [self drainMainQueue];
    XCTAssertEqual(self.reports.count, 0u);
}

/// A rotation fling at the same lift cuts a zoom fling short too.
- (void)testAPinchFlingAndARotationFlingAtOneLiftReportNothing {
    MLNGlideRotateMock *rotate = [self beginRotate];
    UIPinchGestureRecognizerMock *pinch = [self beginPinch];
    [self endPinch:pinch velocity:6];
    [self endRotate:rotate];
    [self drainMainQueue];
    XCTAssertEqual(self.reports.count, 0u);
}

/// A gesture of the same lift that ends with no fling still cancels the
/// glide before it (each gesture handler cancels the transitions on entry).
/// So the glide is not reported.
- (void)testAPinchThatEndsWithNoFlingAfterAPanFlingAtOneLiftReportsNothing {
    MLNGlidePanMock *pan = [self beginPan];
    UIPinchGestureRecognizerMock *pinch = [self beginPinch];
    [self endPan:pan];
    [self endPinch:pinch velocity:0];
    [self drainMainQueue];
    XCTAssertEqual(self.reports.count, 0u);
}

/// The other order leaves the fling running, so it is reported.
- (void)testAPinchThatEndsWithNoFlingBeforeAPanFlingAtOneLiftReportsTheFling {
    MLNGlidePanMock *pan = [self beginPan];
    UIPinchGestureRecognizerMock *pinch = [self beginPinch];
    [self endPinch:pinch velocity:0];
    [self endPan:pan];
    [self drainMainQueue];
    XCTAssertEqual(self.reports.count, 1u);
    [self waitForIdle];
    [self assertTheReportIsTheCamera];
}

/// Flings at separate lifts: each is reported, and the newest holds.
- (void)testFlingsAtSeparateLiftsAreEachReported {
    MLNGlidePanMock *pan = [self beginPan];
    UIPinchGestureRecognizerMock *pinch = [self beginPinch];
    [self endPan:pan];
    [self drainMainQueue];
    XCTAssertEqual(self.reports.count, 1u);
    [self endPinch:pinch velocity:6];
    [self drainMainQueue];
    XCTAssertEqual(self.reports.count, 2u);
    [self waitForIdle];
    [self.reports removeObjectAtIndex:0];
    [self assertTheReportIsTheCamera];
}

@end
