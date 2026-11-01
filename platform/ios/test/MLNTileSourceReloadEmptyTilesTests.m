#import <Mapbox.h>
#import <XCTest/XCTest.h>

/// `-[MLNTileSource reloadEmptyTiles]` asks the network again for a tile whose
/// answer was empty (HTTP 204). Without the call, MapLibre does not ask again
/// for a tile that stays in view.

static NSString *const MLNReloadTestHost = @"reload-empty-tiles.test";

/// Answers every tile request to `MLNReloadTestHost` with 204 and reports it.
@interface MLNReloadEmptyTilesProtocol : NSURLProtocol
@end

static void (^MLNReloadTileRequested)(NSURL *url);

@implementation MLNReloadEmptyTilesProtocol

+ (BOOL)canInitWithRequest:(NSURLRequest *)request {
    return [request.URL.host isEqualToString:MLNReloadTestHost];
}

+ (NSURLRequest *)canonicalRequestForRequest:(NSURLRequest *)request {
    return request;
}

- (void)startLoading {
    NSHTTPURLResponse *response = [[NSHTTPURLResponse alloc] initWithURL:self.request.URL
                                                              statusCode:204
                                                             HTTPVersion:@"HTTP/1.1"
                                                            headerFields:@{}];
    [self.client URLProtocol:self didReceiveResponse:response cacheStoragePolicy:NSURLCacheStorageNotAllowed];
    [self.client URLProtocolDidFinishLoading:self];
    NSURL *url = self.request.URL;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (MLNReloadTileRequested) {
            MLNReloadTileRequested(url);
        }
    });
}

- (void)stopLoading {
}

@end

@interface MLNTileSourceReloadEmptyTilesTests : XCTestCase <MLNMapViewDelegate, MLNNetworkConfigurationDelegate>

@property (nonatomic) MLNMapView *mapView;
/// The map draws only in a window.
@property (nonatomic) UIWindow *window;
@property (nonatomic) NSURLSession *session;
@property (nonatomic) XCTestExpectation *styleLoadingExpectation;
@property (nonatomic) XCTestExpectation *idleExpectation;

@end

@implementation MLNTileSourceReloadEmptyTilesTests

- (void)setUp {
    [super setUp];
    NSURLSessionConfiguration *configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    configuration.protocolClasses = @[ [MLNReloadEmptyTilesProtocol class] ];
    self.session = [NSURLSession sessionWithConfiguration:configuration];
    [MLNNetworkConfiguration sharedManager].delegate = self;

    NSURL *styleURL = [[NSBundle bundleForClass:[self class]] URLForResource:@"one-liner" withExtension:@"json"];
    self.mapView = [[MLNMapView alloc] initWithFrame:CGRectMake(0, 0, 400, 400) styleURL:styleURL];
    self.mapView.delegate = self;
    self.window = [[UIWindow alloc] initWithFrame:self.mapView.bounds];
    [self.window addSubview:self.mapView];
    [self.window makeKeyAndVisible];
    if (!self.mapView.style) {
        _styleLoadingExpectation = [self expectationWithDescription:@"Map view should finish loading style."];
        [self waitForExpectationsWithTimeout:10 handler:nil];
    }
}

- (void)tearDown {
    MLNReloadTileRequested = nil;
    [MLNNetworkConfiguration sharedManager].delegate = nil;
    [self.mapView removeFromSuperview];
    self.mapView = nil;
    self.window = nil;
    [self.session invalidateAndCancel];
    self.session = nil;
    [super tearDown];
}

- (NSURLSession *)sessionForNetworkConfiguration:(MLNNetworkConfiguration *)configuration {
    return self.session;
}

- (void)mapView:(MLNMapView *)mapView didFinishLoadingStyle:(MLNStyle *)style {
    [_styleLoadingExpectation fulfill];
}

- (void)mapViewDidBecomeIdle:(MLNMapView *)mapView {
    [self.idleExpectation fulfill];
    self.idleExpectation = nil;
}

- (void)testAnEmptyTileIsAskedForAgain {
    // A new path each run, so that no answer from an earlier run is in the
    // ambient cache.
    NSString *templateURL = [NSString stringWithFormat:@"http://%@/%@/{z}/{x}/{y}.pbf", MLNReloadTestHost,
                                                       [NSUUID UUID].UUIDString];
    __block NSUInteger requests = 0;
    __block XCTestExpectation *requested = [self expectationWithDescription:@"the tile is asked for"];
    MLNReloadTileRequested = ^(NSURL *url) {
        requests++;
        [requested fulfill];
        requested = nil;
    };

    MLNVectorTileSource *source =
        [[MLNVectorTileSource alloc] initWithIdentifier:@"reload"
                                       tileURLTemplates:@[ templateURL ]
                                                options:@{MLNTileSourceOptionMaximumZoomLevel : @0}];
    [self.mapView.style addSource:source];
    MLNLineStyleLayer *layer = [[MLNLineStyleLayer alloc] initWithIdentifier:@"reload-layer" source:source];
    layer.sourceLayerIdentifier = @"lines";
    [self.mapView.style addLayer:layer];

    // The first answer lands, and the map has nothing left to load.
    self.idleExpectation = [self expectationWithDescription:@"the map is idle after the empty tile"];
    [self waitForExpectations:@[ requested, self.idleExpectation ] timeout:10];
    XCTAssertEqual(requests, 1u);

    requested = [self expectationWithDescription:@"the empty tile is asked for again"];
    [source reloadEmptyTiles];
    [self waitForExpectations:@[ requested ] timeout:10];
    XCTAssertEqual(requests, 2u);
}

@end
