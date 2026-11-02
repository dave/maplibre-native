#import <MapLibre.h>
#import <XCTest/XCTest.h>

/// A map view made with no style loads no style: it does not fetch the
/// default style (`MLNStyle.defaultStyleURL`, a remote URL) behind the app's
/// back, and it stays blank until a style is set.
@interface MLNMapViewNoDefaultStyleTests : XCTestCase
@end

@implementation MLNMapViewNoDefaultStyleTests

- (void)assertLoadsNoStyle:(MLNMapView *)mapView {
    XCTAssertNil(mapView.styleURL, @"no style URL is loaded");
    XCTAssertNil(mapView.style);
}

- (void)testInitWithFrameLoadsNoStyle {
    XCTAssertNotNil(MLNStyle.defaultStyleURL, @"there is a default style to fetch");
    [self assertLoadsNoStyle:[[MLNMapView alloc] initWithFrame:CGRectMake(0, 0, 100, 100)]];
}

- (void)testANilStyleURLLoadsNoStyle {
    [self assertLoadsNoStyle:[[MLNMapView alloc] initWithFrame:CGRectMake(0, 0, 100, 100) styleURL:nil]];
}

- (void)testOptionsWithNoStyleLoadNoStyle {
    [self assertLoadsNoStyle:[[MLNMapView alloc] initWithFrame:CGRectMake(0, 0, 100, 100) options:nil]];
    [self assertLoadsNoStyle:[[MLNMapView alloc] initWithFrame:CGRectMake(0, 0, 100, 100)
                                                       options:[[MLNMapOptions alloc] init]]];
}

/// Reloading a map view that has no style loads nothing.
- (void)testReloadingNoStyleLoadsNoStyle {
    MLNMapView *mapView = [[MLNMapView alloc] initWithFrame:CGRectMake(0, 0, 100, 100)];
    [mapView reloadStyle:nil];
    [self assertLoadsNoStyle:mapView];
}

/// A style set later loads as usual.
- (void)testAStyleSetLaterLoads {
    MLNMapView *mapView = [[MLNMapView alloc] initWithFrame:CGRectMake(0, 0, 100, 100)];
    NSURL *styleURL = [[NSBundle bundleForClass:[self class]] URLForResource:@"one-liner" withExtension:@"json"];
    mapView.styleURL = styleURL;
    XCTAssertEqualObjects(mapView.styleURL, styleURL);
}

@end
