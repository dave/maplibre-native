#import <MapLibre.h>
#import <XCTest/XCTest.h>

/// `-setZoomLevel:aroundAnchorPoint:duration:animationTimingFunction:`: the
/// zoom level ends exactly where it is asked, and the coordinate under the
/// anchor point stays under it, at the end and on every frame of the change.
@interface MLNMapViewAnchoredZoomTests : XCTestCase <MLNMapViewDelegate>
@property (nonatomic) UIWindow *window;
@property (nonatomic) MLNMapView *mapView;
@property (nonatomic) XCTestExpectation *styleLoaded;
@property (nonatomic) XCTestExpectation *regionDidChange;
/// The anchor point and the coordinate that must stay under it.
@property (nonatomic) CGPoint anchor;
@property (nonatomic) CLLocationCoordinate2D anchorCoordinate;
/// The largest distance, in points, between the anchor and its coordinate
/// on any frame of the change.
@property (nonatomic) CGFloat largestDrift;
@property (nonatomic) NSInteger framesWhileChanging;
@end

@implementation MLNMapViewAnchoredZoomTests

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
    self.anchor = CGPointMake(300, 100);
    self.anchorCoordinate = [self.mapView convertPoint:self.anchor toCoordinateFromView:self.mapView];
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

- (void)mapViewRegionIsChanging:(MLNMapView *)mapView {
    self.framesWhileChanging++;
    CGPoint point = [mapView convertCoordinate:self.anchorCoordinate toPointToView:mapView];
    self.largestDrift = MAX(self.largestDrift, hypot(point.x - self.anchor.x, point.y - self.anchor.y));
}

- (void)mapView:(MLNMapView *)mapView regionDidChangeAnimated:(BOOL)animated {
    [self.regionDidChange fulfill];
}

- (void)assertTheAnchorKeepsItsCoordinate {
    CGPoint point = [self.mapView convertCoordinate:self.anchorCoordinate toPointToView:self.mapView];
    XCTAssertEqualWithAccuracy(point.x, self.anchor.x, 1e-6);
    XCTAssertEqualWithAccuracy(point.y, self.anchor.y, 1e-6);
}

- (void)testNoDurationJumpsToTheExactZoom {
    [self.mapView setZoomLevel:12.37 aroundAnchorPoint:self.anchor duration:0 animationTimingFunction:nil];
    XCTAssertEqual(self.mapView.zoomLevel, 12.37, @"the zoom is set at once, exactly");
    [self assertTheAnchorKeepsItsCoordinate];
}

- (void)testADurationEndsAtTheExactZoomWithTheAnchorStillOnEveryFrame {
    self.regionDidChange = [self expectationWithDescription:@"the change ends"];
    self.largestDrift = 0;
    self.framesWhileChanging = 0;
    CAMediaTimingFunction *curve = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseOut];
    [self.mapView setZoomLevel:8.6 aroundAnchorPoint:self.anchor duration:0.25 animationTimingFunction:curve];
    XCTAssertEqual(self.mapView.zoomLevel, 10, @"the change is animated, not set at once");
    [self waitForExpectations:@[ self.regionDidChange ] timeout:10];

    XCTAssertEqual(self.mapView.zoomLevel, 8.6, @"the change ends exactly at the zoom asked");
    [self assertTheAnchorKeepsItsCoordinate];
    XCTAssertGreaterThan(self.framesWhileChanging, 0, @"the change has frames between its ends");
    XCTAssertLessThan(self.largestDrift, 1e-6, @"the anchor's coordinate stays under it on every frame");
}

- (void)testTheSameZoomChangesNothing {
    CLLocationCoordinate2D center = self.mapView.centerCoordinate;
    [self.mapView setZoomLevel:10 aroundAnchorPoint:self.anchor duration:0.25 animationTimingFunction:nil];
    XCTAssertEqual(self.mapView.zoomLevel, 10);
    XCTAssertEqual(self.mapView.centerCoordinate.latitude, center.latitude);
    XCTAssertEqual(self.mapView.centerCoordinate.longitude, center.longitude);
}

@end
