#import <MapLibre.h>
#import <XCTest/XCTest.h>

static NSString *const MLNReloadScheme = @"mlnreloadtest";

/// Answers every request at once, and reports each on the main thread: a tile's
/// `localOnly` step "not stored", its fetch step as the test says, and any other
/// request with an empty GeoJSON body.
@interface MLNReloadTestProvider : NSObject <MLNResourceProvider>
@property (nonatomic, copy) void (^answerFetch)(MLNResourceRequest *request);
@property (nonatomic, copy) void (^onFetch)(void);
@property (nonatomic, copy) void (^onOther)(void);
@end

@implementation MLNReloadTestProvider

- (void)startRequest:(MLNResourceRequest *)request {
    if (request.kind != MLNResourceKindTile) {
        [request answerWithData:[@"{\"type\":\"FeatureCollection\",\"features\":[]}" dataUsingEncoding:NSUTF8StringEncoding]];
        dispatch_async(dispatch_get_main_queue(), ^{
            self.onOther();
        });
        return;
    }
    if (request.localOnly) {
        [request answerNotFound:@"not stored"];
        return;
    }
    self.answerFetch(request);
    dispatch_async(dispatch_get_main_queue(), ^{
        self.onFetch();
    });
}

- (void)cancelRequestWithIdentifier:(uint64_t)identifier {
}

@end

/// `-[MLNTileSource reloadTilesWithFilter:]` asks again only for the tiles whose
/// last answer matches the filter. To show that a tile is not asked again, a
/// marker request goes to the provider after the frame that takes the reload:
/// the loader thread runs requests in order, so any fetch came before it. A failed
/// tile's parents stay incomplete, so that map never becomes idle: the tile's
/// error action says its answer has landed.
@interface MLNTileSourceReloadTilesTests : XCTestCase <MLNMapViewDelegate>
@property (nonatomic) MLNMapView *mapView;
/// The map draws only in a window.
@property (nonatomic) UIWindow *window;
@property (nonatomic) MLNReloadTestProvider *provider;
@property (nonatomic) MLNVectorTileSource *source;
@property (nonatomic) XCTestExpectation *styleLoaded;
@property (nonatomic) XCTestExpectation *idle;
@property (nonatomic) XCTestExpectation *frame;
@property (nonatomic) XCTestExpectation *tileFailed;
@property (nonatomic) NSUInteger fetches;
@end

@implementation MLNTileSourceReloadTilesTests

- (void)setUp {
    [super setUp];
    self.provider = [[MLNReloadTestProvider alloc] init];
    __weak MLNTileSourceReloadTilesTests *weakSelf = self;
    self.provider.onFetch = ^{
        weakSelf.fetches++;
    };
    [MLNResourceProviders registerProvider:self.provider forScheme:MLNReloadScheme];
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
    [MLNResourceProviders unregisterProviderForScheme:MLNReloadScheme];
    [super tearDown];
}

- (void)mapView:(MLNMapView *)mapView didFinishLoadingStyle:(MLNStyle *)style {
    [self.styleLoaded fulfill];
}

- (void)mapViewDidBecomeIdle:(MLNMapView *)mapView {
    [self.idle fulfill];
    self.idle = nil;
}

- (void)mapViewDidFinishRenderingFrame:(MLNMapView *)mapView fullyRendered:(BOOL)fullyRendered {
    [self.frame fulfill];
    self.frame = nil;
}

- (void)mapView:(MLNMapView *)mapView
    tileDidTriggerAction:(MLNTileOperation)operation
                       x:(NSInteger)x
                       y:(NSInteger)y
                       z:(NSInteger)z
                    wrap:(NSInteger)wrap
             overscaledZ:(NSInteger)overscaledZ
                sourceID:(NSString *)sourceID {
    if ([sourceID isEqualToString:@"tiles"] && operation == MLNTileOperationError) {
        [self.tileFailed fulfill];
        self.tileFailed = nil;
    }
}

- (void)waitForFrame {
    self.frame = [self expectationWithDescription:@"a frame finishes"];
    [self.mapView triggerRepaint];
    [self waitForExpectations:@[ self.frame ] timeout:10];
}

- (void)waitForIdle {
    self.idle = [self expectationWithDescription:@"the map is idle"];
    [self.mapView triggerRepaint];
    [self waitForExpectations:@[ self.idle ] timeout:10];
}

/// Adds the source, and waits for its one tile's first answer to land.
- (void)addSourceAnswering:(void (^)(MLNResourceRequest *request))answer failing:(BOOL)failing {
    self.provider.answerFetch = answer;
    XCTestExpectation *tileFailed = failing ? [self expectationWithDescription:@"the tile's error action"] : nil;
    self.tileFailed = tileFailed;
    self.source = [[MLNVectorTileSource alloc]
        initWithIdentifier:@"tiles"
          tileURLTemplates:@[ [NSString stringWithFormat:@"%@://tiles/{z}/{x}/{y}", MLNReloadScheme] ]
                   options:@{MLNTileSourceOptionMaximumZoomLevel : @0}];
    [self.mapView.style addSource:self.source];
    MLNCircleStyleLayer *layer = [[MLNCircleStyleLayer alloc] initWithIdentifier:@"tile-circles" source:self.source];
    layer.sourceLayerIdentifier = @"any";
    [self.mapView.style addLayer:layer];
    XCTestExpectation *fetched = [self expectationWithDescription:@"the tile is fetched"];
    __weak MLNTileSourceReloadTilesTests *weakSelf = self;
    self.provider.onFetch = ^{
        weakSelf.fetches++;
        [fetched fulfill];
    };
    [self waitForExpectations:@[ fetched ] timeout:10];
    self.provider.onFetch = ^{
        weakSelf.fetches++;
    };
    if (failing) {
        [self waitForExpectations:@[ tileFailed ] timeout:10];
        [self waitForFrame];
    } else {
        [self waitForIdle];
    }
    XCTAssertEqual(self.fetches, 1u);
}

/// Waits until every request made so far has reached the provider.
- (void)waitForRequestsSoFar {
    [self waitForFrame];
    XCTestExpectation *marked = [self expectationWithDescription:@"the marker request reaches the provider"];
    self.provider.onOther = ^{
        [marked fulfill];
    };
    MLNShapeSource *marker = [[MLNShapeSource alloc]
        initWithIdentifier:@"marker"
                       URL:[NSURL URLWithString:[NSString stringWithFormat:@"%@://marker", MLNReloadScheme]]
                   options:nil];
    [self.mapView.style addSource:marker];
    [self.mapView triggerRepaint];
    [self waitForExpectations:@[ marked ] timeout:10];
}

- (void)assertReloadWithFilter:(MLNTileReloadFilter)filter asksAgain:(BOOL)asksAgain {
    self.fetches = 0;
    if (asksAgain) {
        XCTestExpectation *fetched = [self expectationWithDescription:@"the tile is asked for again"];
        __weak MLNTileSourceReloadTilesTests *weakSelf = self;
        self.provider.onFetch = ^{
            weakSelf.fetches++;
            [fetched fulfill];
        };
        [self.source reloadTilesWithFilter:filter];
        [self.mapView triggerRepaint];
        [self waitForExpectations:@[ fetched ] timeout:10];
        XCTAssertEqual(self.fetches, 1u);
    } else {
        [self.source reloadTilesWithFilter:filter];
        [self waitForRequestsSoFar];
        XCTAssertEqual(self.fetches, 0u, @"the filter does not match the tile's last answer");
    }
}

- (void (^)(MLNResourceRequest *))answerEmpty {
    return ^(MLNResourceRequest *request) {
        [request answerEmpty];
    };
}

- (void (^)(MLNResourceRequest *))answerUnavailable {
    return ^(MLNResourceRequest *request) {
        [request answerUnavailable:@"offline"];
    };
}

- (void)testAnEmptyTileIsNotAskedAgainByTheFailedFilter {
    [self addSourceAnswering:[self answerEmpty] failing:NO];
    [self assertReloadWithFilter:MLNTileReloadFilterFailed asksAgain:NO];
}

- (void)testAnEmptyTileIsAskedAgainByTheEmptyFilter {
    [self addSourceAnswering:[self answerEmpty] failing:NO];
    [self assertReloadWithFilter:MLNTileReloadFilterEmpty asksAgain:YES];
}

- (void)testAnEmptyTileIsAskedAgainByBoth {
    [self addSourceAnswering:[self answerEmpty] failing:NO];
    [self assertReloadWithFilter:MLNTileReloadFilterEmptyAndFailed asksAgain:YES];
}

- (void)testAFailedTileIsNotAskedAgainByTheEmptyFilter {
    [self addSourceAnswering:[self answerUnavailable] failing:YES];
    [self assertReloadWithFilter:MLNTileReloadFilterEmpty asksAgain:NO];
}

- (void)testAFailedTileIsAskedAgainByTheFailedFilter {
    [self addSourceAnswering:[self answerUnavailable] failing:YES];
    [self assertReloadWithFilter:MLNTileReloadFilterFailed asksAgain:YES];
}

- (void)testAFailedTileIsAskedAgainByBoth {
    [self addSourceAnswering:[self answerUnavailable] failing:YES];
    [self assertReloadWithFilter:MLNTileReloadFilterEmptyAndFailed asksAgain:YES];
}

/// `reloadEmptyTiles` asks again for a failed tile too.
- (void)testReloadEmptyTilesAsksAgainForAFailedTile {
    [self addSourceAnswering:[self answerUnavailable] failing:YES];
    self.fetches = 0;
    XCTestExpectation *fetched = [self expectationWithDescription:@"the tile is asked for again"];
    __weak MLNTileSourceReloadTilesTests *weakSelf = self;
    self.provider.onFetch = ^{
        weakSelf.fetches++;
        [fetched fulfill];
    };
    [self.source reloadEmptyTiles];
    [self.mapView triggerRepaint];
    [self waitForExpectations:@[ fetched ] timeout:10];
}

- (void)testAnUnknownFilterRaises {
    [self addSourceAnswering:[self answerEmpty] failing:NO];
    XCTAssertThrowsSpecificNamed([self.source reloadTilesWithFilter:(MLNTileReloadFilter)99], NSException,
                                 NSInvalidArgumentException);
}

@end
