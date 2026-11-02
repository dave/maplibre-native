#import <MapLibre.h>
#import <XCTest/XCTest.h>

/// `-visibleCoordinateBoundsAtCenter:zoomLevel:direction:pitch:` gives what
/// `visibleCoordinateBounds` gives with the map at that camera, and the map
/// does not move.
@interface MLNMapViewBoundsAtCameraTests : XCTestCase
@property (nonatomic) MLNMapView *mapView;
@end

@implementation MLNMapViewBoundsAtCameraTests

- (void)setUp {
    [super setUp];
    NSURL *styleURL = [[NSBundle bundleForClass:[self class]] URLForResource:@"one-liner" withExtension:@"json"];
    self.mapView = [[MLNMapView alloc] initWithFrame:CGRectMake(0, 0, 300, 500) styleURL:styleURL];
    self.mapView.contentInset = UIEdgeInsetsMake(40, 10, 120, 30);
}

- (void)tearDown {
    self.mapView = nil;
    [super tearDown];
}

- (void)assertBounds:(MLNCoordinateBounds)actual equal:(MLNCoordinateBounds)expected {
    XCTAssertEqualWithAccuracy(actual.sw.latitude, expected.sw.latitude, 1e-9);
    XCTAssertEqualWithAccuracy(actual.sw.longitude, expected.sw.longitude, 1e-9);
    XCTAssertEqualWithAccuracy(actual.ne.latitude, expected.ne.latitude, 1e-9);
    XCTAssertEqualWithAccuracy(actual.ne.longitude, expected.ne.longitude, 1e-9);
}

/// The bounds at another camera, then the map moved there: the two agree,
/// and asking first did not move the map.
- (void)checkCenter:(CLLocationCoordinate2D)center zoomLevel:(double)zoomLevel direction:(CLLocationDirection)direction pitch:(CGFloat)pitch {
    [self.mapView setCenterCoordinate:CLLocationCoordinate2DMake(10, 20) zoomLevel:3 direction:0 animated:NO];
    MLNCoordinateBounds atCamera = [self.mapView visibleCoordinateBoundsAtCenter:center
                                                                       zoomLevel:zoomLevel
                                                                       direction:direction
                                                                           pitch:pitch];
    XCTAssertEqual(self.mapView.zoomLevel, 3, @"the map does not move");
    XCTAssertEqualWithAccuracy(self.mapView.centerCoordinate.latitude, 10, 1e-9);
    XCTAssertEqualWithAccuracy(self.mapView.centerCoordinate.longitude, 20, 1e-9);

    // Tilt first: setting a camera goes through an altitude, so the center,
    // zoom and direction are set exactly after it, and keep the pitch.
    MLNMapCamera *camera = self.mapView.camera;
    camera.pitch = pitch;
    self.mapView.camera = camera;
    [self.mapView setCenterCoordinate:center zoomLevel:zoomLevel direction:direction animated:NO];
    XCTAssertEqualWithAccuracy(self.mapView.camera.pitch, pitch, 1e-9);
    XCTAssertEqual(self.mapView.zoomLevel, zoomLevel);
    [self assertBounds:atCamera equal:self.mapView.visibleCoordinateBounds];
}

- (void)testTheBoundsAtACameraAreTheBoundsTheMapGivesThere {
    [self checkCenter:CLLocationCoordinate2DMake(47.3, 8.5) zoomLevel:11.4 direction:0 pitch:0];
}

- (void)testARotatedAndTiltedCamera {
    [self checkCenter:CLLocationCoordinate2DMake(-33.9, 151.2) zoomLevel:9.25 direction:37 pitch:40];
}

/// A view across the antimeridian has one edge beyond ±180°, so the bounds
/// span the view (about 3.3° here), not the rest of the world (about 356°).
- (void)testAViewAcrossTheAntimeridianExtendsBeyond180 {
    MLNCoordinateBounds bounds = [self.mapView visibleCoordinateBoundsAtCenter:CLLocationCoordinate2DMake(0, 179.9)
                                                                     zoomLevel:6
                                                                     direction:0
                                                                         pitch:0];
    XCTAssertTrue(bounds.ne.longitude > 180 || bounds.sw.longitude < -180,
                  @"one edge is past ±180°: %f..%f", bounds.sw.longitude, bounds.ne.longitude);
    XCTAssertLessThan(bounds.ne.longitude - bounds.sw.longitude, 10, @"the bounds span the view, not the world");
    [self checkCenter:CLLocationCoordinate2DMake(0, 179.9) zoomLevel:6 direction:0 pitch:0];
}

- (void)testAnInvalidCenterRaises {
    XCTAssertThrowsSpecificNamed([self.mapView visibleCoordinateBoundsAtCenter:kCLLocationCoordinate2DInvalid
                                                                     zoomLevel:6
                                                                     direction:0
                                                                         pitch:0],
                                 NSException, NSInvalidArgumentException);
}

@end
