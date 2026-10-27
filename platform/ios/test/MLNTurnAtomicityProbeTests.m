#import <Mapbox.h>
#import <XCTest/XCTest.h>
#import <time.h>

// PROBE (throwaway branch probe/ios-turn-atomicity, task T4 of
// one-frame-by-construction-plan.md). Not a regression test.
//
// The iOS control for the Android turn-atomicity probe
// (TurnAtomicityProbeTest.kt on probe/android-turn-atomicity): 8 bands, each
// flipped blue <-> red by a different kind of write in ONE main-thread turn.
//   0-2 fill-color, 3-4 filter, 5 synchronous shape source body,
//   6 feature state, 7 asynchronous shape source body (positive control).
// MLNTurnProbe (MLNMapView+Metal.mm on this branch) hands over one pixel row
// of each encoded frame. A frame is PARTIAL when bands 0-6 are mixed.

typedef void (^MLNTurnProbeHandler)(const uint8_t *bgra, NSUInteger width, uint64_t encodedNs);

static const int kBands = 8;
static const int kAll = 0xff;
static const int kMain = 0x7f;
#ifndef TURNPROBE_RUNS
#define TURNPROBE_RUNS 200
#endif
static const int kRuns = TURNPROBE_RUNS;
static int gLogged = 0;
static const int kHeavyPoints = 30000;

@interface MLNTurnFrame : NSObject
@property (nonatomic) uint64_t t;
@property (nonatomic) int on;
@property (nonatomic) int off;
@end
@implementation MLNTurnFrame
@end

@interface MLNTurnAtomicityProbeTests : XCTestCase <MLNMapViewDelegate>
@property (nonatomic) MLNMapView *mapView;
@property (nonatomic) UIWindow *window;
@property (nonatomic) XCTestExpectation *styleExpectation;
@property (nonatomic) XCTestExpectation *idleExpectation;
@property (atomic) XCTestExpectation *completeExpectation;
@property (atomic) BOOL targetOn;
@property (nonatomic) BOOL busy;
@property (nonatomic) BOOL heavy;
@property (nonatomic) NSMutableArray<MLNTurnFrame *> *frames;
@property (nonatomic) NSMutableDictionary<NSNumber *, MLNShape *> *heavyShapes;
@end

@implementation MLNTurnAtomicityProbeTests {
    int _xs[kBands];
}

static uint64_t nowNs(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

- (void)setUp {
    [super setUp];
    NSURL *styleURL = [[NSBundle bundleForClass:[self class]] URLForResource:@"one-liner" withExtension:@"json"];
    self.mapView = [[MLNMapView alloc] initWithFrame:CGRectMake(0, 0, 400, 400) styleURL:styleURL];
    self.mapView.delegate = self;
    self.window = [[UIWindow alloc] initWithFrame:self.mapView.bounds];
    [self.window addSubview:self.mapView];
    [self.window makeKeyAndVisible];
    self.frames = [NSMutableArray arrayWithCapacity:200000];
    self.heavyShapes = [NSMutableDictionary dictionary];
    if (!self.mapView.style) {
        self.styleExpectation = [self expectationWithDescription:@"style"];
        [self waitForExpectationsWithTimeout:10 handler:nil];
    }
}

- (void)tearDown {
    [NSClassFromString(@"MLNTurnProbe") performSelector:@selector(setHandler:) withObject:nil];
    [self.mapView removeFromSuperview];
    self.mapView = nil;
    self.window = nil;
    [super tearDown];
}

- (void)mapView:(MLNMapView *)mapView didFinishLoadingStyle:(MLNStyle *)style {
    [self.styleExpectation fulfill];
    self.styleExpectation = nil;
}

- (void)mapViewDidBecomeIdle:(MLNMapView *)mapView {
    XCTestExpectation *e = self.idleExpectation;
    self.idleExpectation = nil;
    [e fulfill];
}

- (void)mapViewDidFinishRenderingFrame:(MLNMapView *)mapView fullyRendered:(BOOL)fullyRendered {
    if (self.busy) {
        [mapView triggerRepaint];
    }
}

// MARK: - shapes and style

static NSString *bandsJSON(int from, int to, BOOL on, int extraPoints) {
    NSMutableString *s = [NSMutableString stringWithString:@"{\"type\":\"FeatureCollection\",\"features\":["];
    for (int i = from; i <= to; i++) {
        double a = i, b = i + 0.9;
        if (i > from) [s appendString:@","];
        [s appendFormat:@"{\"type\":\"Feature\",\"id\":%d,\"properties\":{\"b\":%d,\"on\":%d},"
                        @"\"geometry\":{\"type\":\"Polygon\",\"coordinates\":[[[%g,-3],[%g,-3],[%g,3],[%g,3],[%g,-3]]]}}",
                        i, i, on ? 1 : 0, a, b, b, a, a];
    }
    for (int k = 0; k < extraPoints; k++) {
        [s appendFormat:@",{\"type\":\"Feature\",\"properties\":{},\"geometry\":{\"type\":\"Point\",\"coordinates\":[%g,%g]}}",
                        60.0 + (k % 300) * 0.1, -30.0 + (k / 300) * 0.1];
    }
    [s appendString:@"]}"];
    return s;
}

static MLNShape *bandsShape(int from, int to, BOOL on, int extraPoints) {
    NSData *data = [bandsJSON(from, to, on, extraPoints) dataUsingEncoding:NSUTF8StringEncoding];
    NSError *error = nil;
    MLNShape *shape = [MLNShape shapeWithData:data encoding:NSUTF8StringEncoding error:&error];
    NSCAssert(shape && !error, @"shape: %@", error);
    return shape;
}

static NSArray *kOnColor(void) { return @[ @"rgb", @255, @0, @0 ]; }
static NSArray *kOffColor(void) { return @[ @"rgb", @0, @0, @255 ]; }

- (NSExpression *)color:(BOOL)on {
    return [NSExpression expressionWithMLNJSONObject:on ? kOnColor() : kOffColor()];
}

- (NSPredicate *)overlayFilter:(int)i on:(BOOL)on {
    return [NSPredicate predicateWithFormat:@"b == %d", on ? i : -1];
}

- (void)buildStyle {
    MLNStyle *style = self.mapView.style;
    for (MLNStyleLayer *layer in style.layers.copy) {
        [style removeLayer:layer];
    }
    style.transition = MLNTransitionMake(0, 0);
    MLNBackgroundStyleLayer *bg = [[MLNBackgroundStyleLayer alloc] initWithIdentifier:@"bg"];
    bg.backgroundColor = [NSExpression expressionForConstantValue:UIColor.blackColor];
    [style addLayer:bg];

    NSDictionary *sync = @{MLNShapeSourceOptionSynchronousUpdate : @YES};
    MLNShapeSource *st = [[MLNShapeSource alloc] initWithIdentifier:@"st" shape:bandsShape(0, 4, NO, 0) options:sync];
    MLNShapeSource *g = [[MLNShapeSource alloc] initWithIdentifier:@"g" shape:bandsShape(5, 5, NO, 0) options:sync];
    MLNShapeSource *fs = [[MLNShapeSource alloc] initWithIdentifier:@"fs" shape:bandsShape(6, 6, NO, 0) options:sync];
    MLNShapeSource *ga = [[MLNShapeSource alloc] initWithIdentifier:@"ga" shape:bandsShape(7, 7, NO, 0) options:nil];
    for (MLNShapeSource *s in @[ st, g, fs, ga ]) {
        [style addSource:s];
    }
    for (int i = 0; i <= 4; i++) {
        MLNFillStyleLayer *l = [[MLNFillStyleLayer alloc] initWithIdentifier:[NSString stringWithFormat:@"base%d", i] source:st];
        l.predicate = [NSPredicate predicateWithFormat:@"b == %d", i];
        l.fillColor = [self color:NO];
        l.fillAntialiased = [NSExpression expressionForConstantValue:@NO];
        [style addLayer:l];
    }
    for (int i = 3; i <= 4; i++) {
        MLNFillStyleLayer *l = [[MLNFillStyleLayer alloc] initWithIdentifier:[NSString stringWithFormat:@"over%d", i] source:st];
        l.predicate = [self overlayFilter:i on:NO];
        l.fillColor = [self color:YES];
        l.fillAntialiased = [NSExpression expressionForConstantValue:@NO];
        [style addLayer:l];
    }
    for (MLNShapeSource *s in @[ g, ga ]) {
        MLNFillStyleLayer *l = [[MLNFillStyleLayer alloc] initWithIdentifier:[@"data-" stringByAppendingString:s.identifier] source:s];
        l.fillColor = [NSExpression expressionWithMLNJSONObject:@[ @"case", @[ @"==", @[ @"get", @"on" ], @1 ], kOnColor(), kOffColor() ]];
        l.fillAntialiased = [NSExpression expressionForConstantValue:@NO];
        [style addLayer:l];
    }
    MLNFillStyleLayer *l = [[MLNFillStyleLayer alloc] initWithIdentifier:@"state-fs" source:fs];
    l.fillColor = [NSExpression expressionWithMLNJSONObject:@[ @"case", @[ @"boolean", @[ @"feature-state", @"on" ], @NO ], kOnColor(), kOffColor() ]];
    l.fillAntialiased = [NSExpression expressionForConstantValue:@NO];
    [style addLayer:l];
}

// MARK: - the probe

- (void)installProbe {
    [self.mapView setCenterCoordinate:CLLocationCoordinate2DMake(0, 3.95) zoomLevel:5 animated:NO];
    [self.mapView layoutIfNeeded];
    const CGFloat scale = UIScreen.mainScreen.scale; // the drawable scale (MLNEffectiveScaleFactorForView)
    for (int i = 0; i < kBands; i++) {
        CGPoint p = [self.mapView convertCoordinate:CLLocationCoordinate2DMake(0, i + 0.45) toPointToView:self.mapView];
        _xs[i] = (int)lround(p.x * scale);
    }
    CGPoint p = [self.mapView convertCoordinate:CLLocationCoordinate2DMake(0, 0.45) toPointToView:self.mapView];
    const int row = (int)lround(p.y * scale);
    NSLog(@"TURNPROBE view scale=%g bandsX=%d,%d,%d,%d,%d,%d,%d,%d row=%d", scale, _xs[0], _xs[1], _xs[2], _xs[3], _xs[4],
          _xs[5], _xs[6], _xs[7], row);

    Class probe = NSClassFromString(@"MLNTurnProbe");
    XCTAssertNotNil(probe, @"this build carries the probe");
    [probe performSelector:@selector(setRow:) withObject:@(row)];
    __weak typeof(self) weakSelf = self;
    NSData *xsData = [NSData dataWithBytes:_xs length:sizeof(_xs)];
    MLNTurnProbeHandler handler = ^(const uint8_t *bgra, NSUInteger width, uint64_t encodedNs) {
        typeof(self) strongSelf = weakSelf;
        if (!strongSelf) return;
        const int *xs = (const int *)xsData.bytes;
        int on = 0, off = 0;
        for (int i = 0; i < kBands; i++) {
            if ((NSUInteger)xs[i] >= width) continue;
            const uint8_t *px = bgra + xs[i] * 4;
            const int b = px[0], r = px[2];
            if (r > 180 && b < 80) on |= 1 << i;
            if (b > 180 && r < 80) off |= 1 << i;
        }
        if (gLogged < 12) {
            gLogged++;
            const uint8_t *p0 = bgra + xs[0] * 4;
            NSLog(@"TURNPROBE frame on=0x%x off=0x%x band0 bgra=%d,%d,%d,%d width=%lu", on, off, p0[0], p0[1], p0[2], p0[3],
                  (unsigned long)width);
        }
        MLNTurnFrame *f = [MLNTurnFrame new];
        f.t = encodedNs;
        f.on = on;
        f.off = off;
        @synchronized(strongSelf.frames) {
            [strongSelf.frames addObject:f];
        }
        if ((strongSelf.targetOn ? on : off) == kAll) {
            XCTestExpectation *e = strongSelf.completeExpectation;
            strongSelf.completeExpectation = nil;
            [e fulfill];
        }
    };
    [probe performSelector:@selector(setHandler:) withObject:handler];
}

// MARK: - one turn

- (void)turn:(BOOL)on fsFirst:(BOOL)fsFirst {
    MLNStyle *style = self.mapView.style;
    MLNShapeSource *fs = (MLNShapeSource *)[style sourceWithIdentifier:@"fs"];
    if (fsFirst) [fs setFeatureStateForFeatureID:@"6" state:@{@"on" : @(on)}];
    for (int i = 0; i <= 2; i++) {
        ((MLNFillStyleLayer *)[style layerWithIdentifier:[NSString stringWithFormat:@"base%d", i]]).fillColor = [self color:on];
    }
    for (int i = 3; i <= 4; i++) {
        ((MLNFillStyleLayer *)[style layerWithIdentifier:[NSString stringWithFormat:@"over%d", i]]).predicate = [self overlayFilter:i on:on];
    }
    ((MLNShapeSource *)[style sourceWithIdentifier:@"g"]).shape = self.heavy ? self.heavyShapes[@(on)] : bandsShape(5, 5, on, 0);
    ((MLNShapeSource *)[style sourceWithIdentifier:@"ga"]).shape = bandsShape(7, 7, on, 0);
    if (!fsFirst) [fs setFeatureStateForFeatureID:@"6" state:@{@"on" : @(on)}];
}

/// Returns the frames from the turn up to the first that shows all 8 bands at the target, or nil on a timeout.
- (NSArray<MLNTurnFrame *> *)flip:(BOOL)on fsFirst:(BOOL)fsFirst tStart:(uint64_t *)tStart tEnd:(uint64_t *)tEnd gap:(int64_t *)gap {
    NSUInteger first;
    @synchronized(self.frames) {
        first = self.frames.count;
    }
    self.targetOn = on;
    XCTestExpectation *complete = [self expectationWithDescription:@"complete"];
    self.completeExpectation = complete;
    XCTestExpectation *idle = nil;
    if (!self.busy) {
        idle = [self expectationWithDescription:@"idle"];
        self.idleExpectation = idle;
    }
    *tStart = nowNs();
    [self turn:on fsFirst:fsFirst];
    *tEnd = nowNs();
    NSMutableArray *wait = [NSMutableArray arrayWithObject:complete];
    if (idle) [wait addObject:idle];
    XCTWaiterResult r = [XCTWaiter waitForExpectations:wait timeout:10];
    if (r != XCTWaiterResultCompleted) {
        MLNTurnFrame *last;
        @synchronized(self.frames) {
            last = self.frames.lastObject;
        }
        NSLog(@"TURNPROBE timeout on=%d result=%ld completeLeft=%d idleLeft=%d frames=%lu last on=0x%x off=0x%x", on, (long)r,
              self.completeExpectation != nil, self.idleExpectation != nil, (unsigned long)self.frames.count, last.on, last.off);
        self.completeExpectation = nil;
        self.idleExpectation = nil;
        return nil;
    }
    NSMutableArray *out = [NSMutableArray array];
    uint64_t lastBefore = 0;
    @synchronized(self.frames) {
        for (NSUInteger k = first > 0 ? first - 1 : 0; k < self.frames.count; k++) {
            MLNTurnFrame *f = self.frames[k];
            if (f.t < *tStart) {
                lastBefore = f.t;
                continue;
            }
            [out addObject:f];
            if ((on ? f.on : f.off) == kAll) break;
        }
    }
    *gap = lastBefore ? (int64_t)(*tStart - lastBefore) : -1;
    return out;
}

- (NSString *)runArm:(NSString *)arm {
    NSArray *parts = [arm componentsSeparatedByString:@"-"];
    NSString *mode = parts[0];
    BOOL fsFirst = [parts[1] isEqualToString:@"fsFirst"];
    self.busy = [mode isEqualToString:@"busy"] || [mode isEqualToString:@"heavyBusy"];
    self.heavy = [mode hasPrefix:@"heavy"];
    if (self.heavy && self.heavyShapes.count == 0) {
        self.heavyShapes[@YES] = bandsShape(5, 5, YES, kHeavyPoints);
        self.heavyShapes[@NO] = bandsShape(5, 5, NO, kHeavyPoints);
    }
    if (self.busy) [self.mapView triggerRepaint];
    uint64_t ts, te;
    int64_t gap;
    XCTAssertNotNil([self flip:NO fsFirst:fsFirst tStart:&ts tEnd:&te gap:&gap], @"settle");

    int partialRuns = 0, partialFrames = 0, controlSplit = 0, timeouts = 0, during = 0, fsAlone = 0, allButFs = 0;
    int toMain[8] = {0};
    NSMutableDictionary<NSNumber *, NSNumber *> *lead = [NSMutableDictionary dictionary];
    NSMutableArray<NSNumber *> *turnUs = [NSMutableArray array], *gapUs = [NSMutableArray array];
    for (int run = 1; run <= kRuns; run++) {
        BOOL on = run % 2 == 1;
        NSArray<MLNTurnFrame *> *fr = [self flip:on fsFirst:fsFirst tStart:&ts tEnd:&te gap:&gap];
        if (!fr) {
            timeouts++;
            continue;
        }
        [turnUs addObject:@((te - ts) / 1000)];
        [gapUs addObject:@(gap / 1000)];
        BOOL sawPartial = NO, mainDone = NO;
        int n = 0, leadMask = -1;
        for (MLNTurnFrame *f in fr) {
            if (f.t <= te) during++;
            int tgt = (on ? f.on : f.off) & kMain;
            if (mainDone) continue;
            n++;
            if (tgt != 0 && tgt != kMain) {
                sawPartial = YES;
                partialFrames++;
                if (leadMask < 0) leadMask = tgt;
            }
            if (tgt == kMain) {
                mainDone = YES;
                if (((on ? f.on : f.off) & 0x80) == 0) controlSplit++;
            }
        }
        if (sawPartial) {
            partialRuns++;
            lead[@(leadMask)] = @(lead[@(leadMask)].intValue + 1);
            if (leadMask == 0x40) fsAlone++;
            if (leadMask == 0x3f) allButFs++;
        }
        toMain[MIN(n, 7)]++;
    }
    self.busy = NO;
    NSArray *ts2 = [turnUs sortedArrayUsingSelector:@selector(compare:)];
    NSArray *gs = [gapUs sortedArrayUsingSelector:@selector(compare:)];
    id (^pct)(NSArray *, double) = ^id(NSArray *l, double p) { return l.count ? l[(NSUInteger)((l.count - 1) * p)] : @(-1); };
    NSMutableString *leads = [NSMutableString string];
    for (NSNumber *k in lead) [leads appendFormat:@"0x%x:%@;", k.intValue, lead[k]];
    return [NSString stringWithFormat:@"arm=%@ runs=%d timeouts=%d partialRuns=%d partialFrames=%d fsAloneFirst=%d allButFsFirst=%d "
                                      @"leadMasks=%@ controlSplitRuns=%d framesDuringTurn=%d "
                                      @"framesFromTurnToMainComplete(1..7+)=%d/%d/%d/%d/%d/%d/%d "
                                      @"turnUs(p50/p90/max)=%@/%@/%@ gapSinceLastFrameUs(p10/p50/p90)=%@/%@/%@",
                                      arm, kRuns, timeouts, partialRuns, partialFrames, fsAlone, allButFs, leads, controlSplit, during,
                                      toMain[1], toMain[2], toMain[3], toMain[4], toMain[5], toMain[6], toMain[7], pct(ts2, .5),
                                      pct(ts2, .9), pct(ts2, 1.0), pct(gs, .1), pct(gs, .5), pct(gs, .9)];
}

- (void)testOneTurnOneFrame {
    [self buildStyle];
    [self installProbe];
    NSArray *arms = @[ @"idle-fsLast", @"busy-fsFirst", @"heavyIdle-fsFirst", @"heavyBusy-fsLast", @"idle-fsFirst", @"busy-fsLast",
                       @"heavyBusy-fsFirst" ];
    for (int round = 1; round <= 3; round++) {
        for (NSString *arm in arms) {
            NSLog(@"TURNPROBE SUMMARY round=%d %@", round, [self runArm:arm]);
        }
    }
}

@end
