#import <MapLibre.h>
#import <XCTest/XCTest.h>

static NSString *const MLNTestScheme = @"mlnprovidertest";

/// Keeps what the map asks, and hands each event to the test on the main thread.
@interface MLNTestResourceProvider : NSObject <MLNResourceProvider>
@property (nonatomic, copy) void (^onStart)(MLNResourceRequest *request, BOOL onMainThread);
@property (nonatomic, copy) void (^onCancel)(uint64_t identifier, BOOL onMainThread);
@end

@implementation MLNTestResourceProvider

- (void)startRequest:(MLNResourceRequest *)request {
    BOOL onMainThread = NSThread.isMainThread;
    dispatch_async(dispatch_get_main_queue(), ^{
        self.onStart(request, onMainThread);
    });
}

- (void)cancelRequestWithIdentifier:(uint64_t)identifier {
    BOOL onMainThread = NSThread.isMainThread;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self.onCancel) {
            self.onCancel(identifier, onMainThread);
        }
    });
}

@end

/// `MLNResourceProvider` through a real map: a request starts with its fields, an
/// answer reaches the source, and a dropped request is cancelled once.
@interface MLNResourceProviderTests : XCTestCase <MLNMapViewDelegate>
@property (nonatomic) MLNMapView *mapView;
/// The map draws only in a window.
@property (nonatomic) UIWindow *window;
@property (nonatomic) MLNTestResourceProvider *provider;
@property (nonatomic) XCTestExpectation *styleLoaded;
@property (nonatomic) XCTestExpectation *laidOut;
@property (nonatomic) XCTestExpectation *failed;
@property (nonatomic) uint64_t waitedGeneration;
@property (nonatomic, copy) NSString *failure;
@end

@implementation MLNResourceProviderTests

- (void)setUp {
    [super setUp];
    self.provider = [[MLNTestResourceProvider alloc] init];
    [MLNResourceProviders registerProvider:self.provider forScheme:MLNTestScheme];
    NSURL *styleURL = [[NSBundle bundleForClass:[self class]] URLForResource:@"one-liner" withExtension:@"json"];
    self.mapView = [[MLNMapView alloc] initWithFrame:CGRectMake(0, 0, 400, 400) styleURL:styleURL];
    self.mapView.delegate = self;
    self.window = [[UIWindow alloc] initWithFrame:self.mapView.bounds];
    [self.window addSubview:self.mapView];
    [self.window makeKeyAndVisible];
    self.styleLoaded = [self expectationWithDescription:@"the style loads"];
    [self waitForExpectations:@[ self.styleLoaded ] timeout:10];
}

- (void)tearDown {
    [self.mapView removeFromSuperview];
    self.mapView = nil;
    self.window = nil;
    [MLNResourceProviders unregisterProviderForScheme:MLNTestScheme];
    [super tearDown];
}

- (void)mapView:(MLNMapView *)mapView didFinishLoadingStyle:(MLNStyle *)style {
    [self.styleLoaded fulfill];
}

- (void)mapView:(MLNMapView *)mapView sourceDidLayOut:(NSString *)sourceIdentifier generation:(uint64_t)generation {
    if ([sourceIdentifier isEqualToString:@"body"] && generation == self.waitedGeneration) {
        [self.laidOut fulfill];
    }
}

- (void)mapView:(MLNMapView *)mapView
    sourceBodyDidFail:(NSString *)sourceIdentifier
           generation:(uint64_t)generation
              message:(NSString *)message {
    if ([sourceIdentifier isEqualToString:@"body"] && generation == self.waitedGeneration) {
        self.failure = message;
        [self.failed fulfill];
    }
}

- (NSString *)URLNamed:(NSString *)name {
    return [NSString stringWithFormat:@"%@://bodies/%@?v=7", MLNTestScheme, name];
}

/// A shape source in the style, with a layer so that its tiles are made.
- (MLNShapeSource *)addSourceWithURLNamed:(NSString *)name {
    MLNShapeSource *source = [[MLNShapeSource alloc] initWithIdentifier:@"body"
                                                                  shape:nil
                                                                options:@{MLNShapeSourceOptionLabelLane : @YES}];
    [self.mapView.style addSource:source];
    [self.mapView.style addLayer:[[MLNCircleStyleLayer alloc] initWithIdentifier:@"body-circles" source:source]];
    source.URL = [NSURL URLWithString:[self URLNamed:name]];
    return source;
}

/// Waits for the next request to start, and gives it back.
- (MLNResourceRequest *)waitForRequest {
    XCTestExpectation *started = [self expectationWithDescription:@"a request starts"];
    __block MLNResourceRequest *result;
    self.provider.onStart = ^(MLNResourceRequest *request, BOOL onMainThread) {
        if (!result) {
            XCTAssertFalse(onMainThread, @"a request starts on the loader thread");
            result = request;
            [started fulfill];
        }
    };
    [self waitForExpectations:@[ started ] timeout:10];
    return result;
}

- (NSData *)point {
    NSString *body = @"{\"type\":\"Point\",\"coordinates\":[0,0]}";
    return [body dataUsingEncoding:NSUTF8StringEncoding];
}

- (void)testAGeoJSONBodyAsksWithItsFieldsAndItsAnswerIsLaidOut {
    MLNShapeSource *source = [self addSourceWithURLNamed:@"data"];
    self.waitedGeneration = source.dataGeneration;
    MLNResourceRequest *request = [self waitForRequest];

    XCTAssertGreaterThan(request.identifier, 0u);
    XCTAssertEqual(request.kind, MLNResourceKindSource);
    XCTAssertEqualObjects(request.URLString, [self URLNamed:@"data"]);
    XCTAssertEqualObjects(request.sourceIdentifier, @"body");
    XCTAssertFalse(request.localOnly);
    XCTAssertFalse(request.isLowPriority);
    XCTAssertNil(request.tile);

    self.laidOut = [self expectationWithDescription:@"the body is laid out"];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        [request answerWithData:[self point]];
    });
    [self waitForExpectations:@[ self.laidOut ] timeout:10];
}

/// A tile asks `localOnly` first. "Not found" there goes on to the fetch step, and
/// "empty" there ends it.
- (void)testATileAsksInTwoStepsWithItsCoordinates {
    MLNVectorTileSource *tiles = [[MLNVectorTileSource alloc]
        initWithIdentifier:@"tiles"
          tileURLTemplates:@[ [NSString stringWithFormat:@"%@://tiles/{z}/{x}/{y}", MLNTestScheme] ]
                   options:@{MLNTileSourceOptionMaximumZoomLevel : @0}];
    XCTestExpectation *steps = [self expectationWithDescription:@"the tile's two steps"];
    NSMutableArray<NSNumber *> *localOnly = [NSMutableArray array];
    __block MLNResourceRequest *first;
    self.provider.onStart = ^(MLNResourceRequest *request, BOOL onMainThread) {
        if (![request.sourceIdentifier isEqualToString:@"tiles"]) {
            return;
        }
        [localOnly addObject:@(request.localOnly)];
        if (request.localOnly) {
            first = request;
            [request answerNotFound:@"not stored"];
        } else {
            XCTAssertNotEqual(request.identifier, first.identifier, @"each step is its own request");
            XCTAssertEqual(request.kind, MLNResourceKindTile);
            XCTAssertEqual(request.tile.x, 0);
            XCTAssertEqual(request.tile.y, 0);
            XCTAssertEqual(request.tile.z, 0);
            XCTAssertEqualObjects(request.tile.urlTemplate, ([NSString stringWithFormat:@"%@://tiles/{z}/{x}/{y}", MLNTestScheme]));
            XCTAssertEqualObjects(request.URLString, ([NSString stringWithFormat:@"%@://tiles/0/0/0", MLNTestScheme]));
            [request answerEmpty];
            [steps fulfill];
        }
    };
    [self.mapView.style addSource:tiles];
    MLNCircleStyleLayer *layer = [[MLNCircleStyleLayer alloc] initWithIdentifier:@"tile-circles" source:tiles];
    layer.sourceLayerIdentifier = @"any";
    [self.mapView.style addLayer:layer];
    [self.mapView setZoomLevel:0 animated:NO];
    [self waitForExpectations:@[ steps ] timeout:10];
    XCTAssertEqualObjects(localOnly.firstObject, @YES);
    XCTAssertEqualObjects(localOnly.lastObject, @NO);
}

- (void)assertAFailedBodyReaches:(NSString *)message whenAnswered:(void (^)(MLNResourceRequest *request))answer {
    MLNShapeSource *source = [self addSourceWithURLNamed:@"failing"];
    self.waitedGeneration = source.dataGeneration;
    MLNResourceRequest *request = [self waitForRequest];
    self.failed = [self expectationWithDescription:@"the body is reported failed"];
    answer(request);
    [self waitForExpectations:@[ self.failed ] timeout:10];
    XCTAssertEqualObjects(self.failure, message);
}

- (void)testAnEmptyAnswerReachesTheSource {
    [self assertAFailedBodyReaches:@"unexpectedly empty GeoJSON"
                      whenAnswered:^(MLNResourceRequest *request) {
                          [request answerEmpty];
                      }];
}

- (void)testAnUnavailableAnswerReachesTheSourceWithItsReason {
    [self assertAFailedBodyReaches:@"offline"
                      whenAnswered:^(MLNResourceRequest *request) {
                          [request answerUnavailable:@"offline"];
                      }];
}

- (void)testANotFoundAnswerReachesTheSourceWithItsReason {
    [self assertAFailedBodyReaches:@"no such body"
                      whenAnswered:^(MLNResourceRequest *request) {
                          [request answerNotFound:@"no such body"];
                      }];
}

- (void)testAnErrorAnswerReachesTheSourceWithItsReason {
    [self assertAFailedBodyReaches:@"disk failure"
                      whenAnswered:^(MLNResourceRequest *request) {
                          [request answerError:@"disk failure"];
                      }];
}

/// A new URL drops the request before it: it is cancelled once, on the loader
/// thread. Its late answer is dropped, so the new URL's body is what lands.
- (void)testADroppedRequestIsCancelledOnceAndItsLateAnswerIsIgnored {
    MLNShapeSource *source = [self addSourceWithURLNamed:@"old"];
    MLNResourceRequest *old = [self waitForRequest];

    XCTestExpectation *cancelled = [self expectationWithDescription:@"the old request is cancelled"];
    NSMutableArray<NSNumber *> *cancels = [NSMutableArray array];
    self.provider.onCancel = ^(uint64_t identifier, BOOL onMainThread) {
        XCTAssertFalse(onMainThread, @"a cancel comes on the loader thread");
        [cancels addObject:@(identifier)];
        if (cancels.count == 1) {
            [cancelled fulfill];
        }
    };
    source.URL = [NSURL URLWithString:[self URLNamed:@"new"]];
    self.waitedGeneration = source.dataGeneration;
    MLNResourceRequest *newer = [self waitForRequest];
    [self waitForExpectations:@[ cancelled ] timeout:10];
    XCTAssertEqualObjects(newer.URLString, [self URLNamed:@"new"]);
    XCTAssertGreaterThan(newer.identifier, old.identifier);

    [old answerWithData:[@"{not json" dataUsingEncoding:NSUTF8StringEncoding]];
    self.laidOut = [self expectationWithDescription:@"the new body is laid out"];
    [newer answerWithData:[self point]];
    [self waitForExpectations:@[ self.laidOut ] timeout:10];
    XCTAssertNil(self.failure, @"the late answer reached nothing");
    XCTAssertEqualObjects(cancels, @[ @(old.identifier) ]);
}

- (void)testTheCountsFollowARequest {
    XCTAssertNil([MLNResourceProviders countsForScheme:@"mlnnosuchscheme"]);
    MLNResourceProviderCounts *counts = [MLNResourceProviders countsForScheme:MLNTestScheme];
    XCTAssertEqual(counts.inFlight, 0u);
    XCTAssertEqual(counts.peakInFlight, 0u);

    MLNShapeSource *source = [self addSourceWithURLNamed:@"counted"];
    self.waitedGeneration = source.dataGeneration;
    MLNResourceRequest *request = [self waitForRequest];
    counts = [MLNResourceProviders countsForScheme:MLNTestScheme];
    XCTAssertEqual(counts.inFlight, 1u);
    XCTAssertEqual(counts.peakInFlight, 1u);

    self.laidOut = [self expectationWithDescription:@"the body is laid out"];
    [request answerWithData:[self point]];
    [self waitForExpectations:@[ self.laidOut ] timeout:10];
    counts = [MLNResourceProviders countsForScheme:MLNTestScheme];
    XCTAssertEqual(counts.inFlight, 0u);
    XCTAssertEqual(counts.peakInFlight, 1u);

    [MLNResourceProviders unregisterProviderForScheme:MLNTestScheme];
    XCTAssertNil([MLNResourceProviders countsForScheme:MLNTestScheme]);
}

- (void)testANilAnswerRaisesAndLeavesTheRequestOpen {
    MLNShapeSource *source = [self addSourceWithURLNamed:@"nil-answer"];
    self.waitedGeneration = source.dataGeneration;
    MLNResourceRequest *request = [self waitForRequest];
    NSData *noData = nil;
    NSString *noReason = nil;
    XCTAssertThrowsSpecificNamed([request answerWithData:noData], NSException, NSInvalidArgumentException);
    XCTAssertThrowsSpecificNamed([request answerUnavailable:noReason], NSException, NSInvalidArgumentException);
    XCTAssertThrowsSpecificNamed([request answerNotFound:noReason], NSException, NSInvalidArgumentException);
    XCTAssertThrowsSpecificNamed([request answerError:noReason], NSException, NSInvalidArgumentException);

    self.laidOut = [self expectationWithDescription:@"the body is laid out"];
    [request answerWithData:[self point]];
    [self waitForExpectations:@[ self.laidOut ] timeout:10];
}

- (void)testAnEmptySchemeOrNoProviderRaises {
    XCTAssertThrowsSpecificNamed([MLNResourceProviders registerProvider:self.provider forScheme:@""], NSException,
                                 NSInvalidArgumentException);
    id<MLNResourceProvider> none = nil;
    XCTAssertThrowsSpecificNamed([MLNResourceProviders registerProvider:none forScheme:@"mlnother"], NSException,
                                 NSInvalidArgumentException);
}

@end
