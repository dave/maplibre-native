#import <MapLibre.h>
#import <XCTest/XCTest.h>

/// Answers each request with the status its path names (`/404`, `/204`), or
/// with a body that is not JSON (`/junk`).
@interface MLNSourceBodyFailedURLProtocol : NSURLProtocol
@end

@implementation MLNSourceBodyFailedURLProtocol

+ (BOOL)canInitWithRequest:(NSURLRequest *)request {
    return [request.URL.host isEqualToString:@"source-body-failed.test"];
}

+ (NSURLRequest *)canonicalRequestForRequest:(NSURLRequest *)request {
    return request;
}

- (void)startLoading {
    NSString *name = self.request.URL.lastPathComponent;
    BOOL junk = [name isEqualToString:@"junk"];
    NSHTTPURLResponse *response = [[NSHTTPURLResponse alloc] initWithURL:self.request.URL
                                                              statusCode:junk ? 200 : name.integerValue
                                                             HTTPVersion:@"HTTP/1.1"
                                                            headerFields:nil];
    [self.client URLProtocol:self didReceiveResponse:response cacheStoragePolicy:NSURLCacheStorageNotAllowed];
    if (junk) {
        [self.client URLProtocol:self didLoadData:[@"{not json" dataUsingEncoding:NSUTF8StringEncoding]];
    }
    [self.client URLProtocolDidFinishLoading:self];
}

- (void)stopLoading {
}

@end

/// `-mapView:sourceBodyDidFail:generation:message:`: a shape source's URL body
/// that will not land is reported with the generation `dataGeneration` gave,
/// once, on the main thread, and never as laid out.
@interface MLNSourceBodyFailedTests : XCTestCase <MLNMapViewDelegate, MLNNetworkConfigurationDelegate>

@property (nonatomic) MLNMapView *mapView;
/// The map draws only in a window.
@property (nonatomic) UIWindow *window;
@property (nonatomic) NSURLSession *session;
@property (nonatomic) XCTestExpectation *styleLoadingExpectation;
@property (nonatomic) XCTestExpectation *failedExpectation;
@property (nonatomic) XCTestExpectation *frameExpectation;
@property (nonatomic) uint64_t waitedGeneration;
@property (nonatomic) NSMutableArray<NSNumber *> *failed;
@property (nonatomic) NSMutableArray<NSString *> *messages;
@property (nonatomic) NSMutableArray<NSNumber *> *laidOut;
@property (nonatomic) BOOL reportedOffTheMainThread;

@end

@implementation MLNSourceBodyFailedTests

- (void)setUp {
    [super setUp];
    NSURLSessionConfiguration *configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    configuration.protocolClasses = @[ [MLNSourceBodyFailedURLProtocol class] ];
    self.session = [NSURLSession sessionWithConfiguration:configuration];
    MLNNetworkConfiguration.sharedManager.delegate = self;

    self.failed = [NSMutableArray array];
    self.messages = [NSMutableArray array];
    self.laidOut = [NSMutableArray array];
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
    MLNNetworkConfiguration.sharedManager.delegate = nil;
    [self.session invalidateAndCancel];
    [self.mapView removeFromSuperview];
    self.mapView = nil;
    self.window = nil;
    [super tearDown];
}

- (NSURLSession *)sessionForNetworkConfiguration:(MLNNetworkConfiguration *)configuration {
    return self.session;
}

- (void)mapView:(MLNMapView *)mapView didFinishLoadingStyle:(MLNStyle *)style {
    [_styleLoadingExpectation fulfill];
}

- (void)mapViewDidFinishRenderingFrame:(MLNMapView *)mapView fullyRendered:(BOOL)fullyRendered {
    [self.frameExpectation fulfill];
    self.frameExpectation = nil;
}

- (void)mapView:(MLNMapView *)mapView sourceDidLayOut:(NSString *)sourceIdentifier generation:(uint64_t)generation {
    if ([sourceIdentifier isEqualToString:@"lane"]) {
        [self.laidOut addObject:@(generation)];
    }
}

- (void)mapView:(MLNMapView *)mapView
    sourceBodyDidFail:(NSString *)sourceIdentifier
           generation:(uint64_t)generation
              message:(NSString *)message {
    if (![sourceIdentifier isEqualToString:@"lane"]) {
        return;
    }
    self.reportedOffTheMainThread = self.reportedOffTheMainThread || !NSThread.isMainThread;
    [self.failed addObject:@(generation)];
    [self.messages addObject:message];
    if (generation == self.waitedGeneration) {
        [self.failedExpectation fulfill];
    }
}

/// Sets the URL with this name on a lane source in the style, and waits for
/// the report of its generation. Then waits for one more frame, and checks
/// that the report came once and the generation is not reported laid out.
- (void)assertFailureForName:(NSString *)name {
    MLNShapeSource *source = [[MLNShapeSource alloc] initWithIdentifier:@"lane"
                                                                  shape:nil
                                                                options:@{MLNShapeSourceOptionLabelLane : @YES}];
    [self.mapView.style addSource:source];
    MLNCircleStyleLayer *layer = [[MLNCircleStyleLayer alloc] initWithIdentifier:@"lane-circles" source:source];
    [self.mapView.style addLayer:layer];

    source.URL = [NSURL URLWithString:[@"https://source-body-failed.test/" stringByAppendingString:name]];
    self.waitedGeneration = source.dataGeneration;
    XCTAssertGreaterThan(self.waitedGeneration, 0u);
    self.failedExpectation = [self expectationWithDescription:@"the body is reported failed"];
    [self waitForExpectations:@[ self.failedExpectation ] timeout:10];

    self.frameExpectation = [self expectationWithDescription:@"one more frame finishes"];
    [self.mapView triggerRepaint];
    [self waitForExpectations:@[ self.frameExpectation ] timeout:10];

    XCTAssertEqualObjects(self.failed, @[ @(self.waitedGeneration) ]);
    XCTAssertFalse(self.reportedOffTheMainThread);
    XCTAssertFalse([self.laidOut containsObject:@(self.waitedGeneration)]);
}

- (void)testAURLThatAnswersAnErrorIsReportedFailedWithItsGeneration {
    [self assertFailureForName:@"404"];
    XCTAssertGreaterThan(self.messages.firstObject.length, 0u);
}

- (void)testAURLThatAnswersNoContentIsReportedFailedWithItsGeneration {
    [self assertFailureForName:@"204"];
    XCTAssertEqualObjects(self.messages.firstObject, @"unexpectedly empty GeoJSON");
}

- (void)testAURLBodyThatDoesNotParseIsReportedFailedWithItsGeneration {
    [self assertFailureForName:@"junk"];
    XCTAssertTrue([self.messages.firstObject containsString:@"parse"], @"%@", self.messages.firstObject);
}

@end
