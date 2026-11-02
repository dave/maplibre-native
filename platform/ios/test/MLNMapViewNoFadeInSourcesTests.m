#import <MapLibre.h>
#import <XCTest/XCTest.h>

/// `MLNMapView.noFadeInSourceIdentifiers`: empty by default, read back as
/// set, kept across a style change, and an identifier with no source in the
/// style is kept as given.
@interface MLNMapViewNoFadeInSourcesTests : XCTestCase <MLNMapViewDelegate>
@property (nonatomic) MLNMapView *mapView;
@property (nonatomic) XCTestExpectation *styleLoaded;
@end

@implementation MLNMapViewNoFadeInSourcesTests

- (NSURL *)styleURL {
    return [[NSBundle bundleForClass:[self class]] URLForResource:@"one-liner" withExtension:@"json"];
}

- (void)setUp {
    [super setUp];
    self.mapView = [[MLNMapView alloc] initWithFrame:CGRectMake(0, 0, 200, 200) styleURL:[self styleURL]];
    self.mapView.delegate = self;
}

- (void)tearDown {
    self.mapView = nil;
    [super tearDown];
}

- (void)mapView:(MLNMapView *)mapView didFinishLoadingStyle:(MLNStyle *)style {
    [self.styleLoaded fulfill];
}

- (void)testTheDefaultIsEmpty {
    XCTAssertEqualObjects(self.mapView.noFadeInSourceIdentifiers, [NSSet set]);
}

- (void)testTheSetIsReadBackAsSet {
    NSSet *identifiers = [NSSet setWithObjects:@"labels", @"no-such-source", nil];
    self.mapView.noFadeInSourceIdentifiers = identifiers;
    XCTAssertEqualObjects(self.mapView.noFadeInSourceIdentifiers, identifiers);

    self.mapView.noFadeInSourceIdentifiers = [NSSet set];
    XCTAssertEqualObjects(self.mapView.noFadeInSourceIdentifiers, [NSSet set]);
}

- (void)testTheSetIsKeptAcrossAStyleChange {
    self.styleLoaded = [self expectationWithDescription:@"the first style loads"];
    [self waitForExpectations:@[ self.styleLoaded ] timeout:10];
    NSSet *identifiers = [NSSet setWithObject:@"labels"];
    self.mapView.noFadeInSourceIdentifiers = identifiers;

    self.styleLoaded = [self expectationWithDescription:@"the second style loads"];
    self.mapView.styleURL = [self styleURL];
    [self waitForExpectations:@[ self.styleLoaded ] timeout:10];
    XCTAssertEqualObjects(self.mapView.noFadeInSourceIdentifiers, identifiers);
}

@end
