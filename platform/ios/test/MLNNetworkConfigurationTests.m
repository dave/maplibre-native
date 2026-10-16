#import <Mapbox.h>
#import <XCTest/XCTest.h>
#import "MLNNetworkConfiguration_Private.h"

@interface MLNTestNetworkConfigurationDelegate : NSObject <MLNNetworkConfigurationDelegate>
@property (nonatomic) NSUInteger receivedResponseCount;
@property (nonatomic, nullable) NSURLResponse *replacementResponse;
@end

@implementation MLNTestNetworkConfigurationDelegate

- (MLNNetworkResponse *)didReceiveResponse:(MLNNetworkResponse *)response {
    self.receivedResponseCount += 1;
    if (self.replacementResponse) {
        response.response = self.replacementResponse;
    }
    return response;
}

@end

// Counts the metric events, on the main queue where they arrive.
@interface MLNTestMetricsDelegate : NSObject <MLNNetworkConfigurationMetricsDelegate>
@property (nonatomic) NSUInteger eventCount;
@end

@implementation MLNTestMetricsDelegate

- (void)networkConfiguration:(MLNNetworkConfiguration *)networkConfiguration
      didGenerateMetricEvent:(NSDictionary *)metricEvent {
    NSAssert(NSThread.isMainThread, @"metric events arrive on the main queue");
    self.eventCount += 1;
}

@end

@interface MLNNetworkConfigurationTests : XCTestCase
@end

@implementation MLNNetworkConfigurationTests

// Regression test for https://github.com/mapbox/mapbox-gl-native/issues/14982
- (void)testAccessingEventsFromMultipleThreads {
    MLNNetworkConfiguration *configuration = [[MLNNetworkConfiguration alloc] init];

    // Concurrent
    dispatch_queue_t queue = dispatch_queue_create("org.maplibre.testAccessingEventsFromMultipleThreads", DISPATCH_QUEUE_CONCURRENT);

    NSUInteger numberOfConcurrentBlocks = 20;

    XCTestExpectation *expectation = [self expectationWithDescription:@"wait-for-threads"];
    expectation.expectedFulfillmentCount = numberOfConcurrentBlocks;

    for (NSUInteger i = 0; i < numberOfConcurrentBlocks; i++) {

        NSString *event = [NSString stringWithFormat:@"test://event-%ld", i];
        NSString *resourceType = @"test";

        dispatch_async(queue, ^{
            [configuration startDownloadEvent:event type:resourceType];

            NSURL *url = [NSURL URLWithString:event];
            NSURLResponse *response = [[NSURLResponse alloc] initWithURL:url MIMEType:nil expectedContentLength:0 textEncodingName:nil];

            [configuration stopDownloadEventForResponse:response];

            dispatch_async(dispatch_get_main_queue(), ^{
                [expectation fulfill];
            });
        });
    }

    [self waitForExpectations:@[expectation] timeout:10.0];
}

// Regression test: received responses were converted for the delegate but the delegate was
// never actually called, so `didReceiveResponse:` implementations had no effect.
- (void)testDidReceiveResponseIsForwardedToTheDelegate {
    MLNNetworkConfiguration *configuration = [[MLNNetworkConfiguration alloc] init];
    MLNTestNetworkConfigurationDelegate *delegate = [[MLNTestNetworkConfigurationDelegate alloc] init];
    configuration.delegate = delegate;

    NSURL *url = [NSURL URLWithString:@"test://tile"];
    delegate.replacementResponse = [[NSHTTPURLResponse alloc] initWithURL:url
                                                               statusCode:404
                                                              HTTPVersion:@"HTTP/1.1"
                                                             headerFields:nil];

    MLNInternalNetworkResponse *internalResponse = [[MLNInternalNetworkResponse alloc] init];
    internalResponse.response = [[NSHTTPURLResponse alloc] initWithURL:url
                                                            statusCode:403
                                                           HTTPVersion:@"HTTP/1.1"
                                                          headerFields:nil];
    internalResponse.data = [@"forbidden" dataUsingEncoding:NSUTF8StringEncoding];

    MLNInternalNetworkResponse *processedResponse =
        [(id<MLNNativeNetworkDelegate>)configuration didReceiveResponse:internalResponse];

    XCTAssertEqual(delegate.receivedResponseCount, 1);
    XCTAssertEqual(((NSHTTPURLResponse *)processedResponse.response).statusCode, 404);
    XCTAssertEqualObjects(processedResponse.data, internalResponse.data);
}

- (void)testDidReceiveResponseWithoutDelegateReturnsResponseUnchanged {
    MLNNetworkConfiguration *configuration = [[MLNNetworkConfiguration alloc] init];

    MLNInternalNetworkResponse *internalResponse = [[MLNInternalNetworkResponse alloc] init];
    internalResponse.data = [@"payload" dataUsingEncoding:NSUTF8StringEncoding];

    MLNInternalNetworkResponse *processedResponse =
        [(id<MLNNativeNetworkDelegate>)configuration didReceiveResponse:internalResponse];

    XCTAssertEqual(processedResponse, internalResponse);
}

// Two completions for one path, at once: the key leaves out the query, and the
// session's delegate queue runs several completions together. Both used to pass
// the event check, then one read the event after the other removed it, and the
// event's dictionary literal threw (NSInvalidArgumentException, a SIGABRT about
// a second after launch on the iPhone). Each start gives exactly one event.
- (void)testTwoCompletionsForOnePathAtOnceGiveOneEvent {
    MLNNetworkConfiguration *configuration = [[MLNNetworkConfiguration alloc] init];
    MLNTestMetricsDelegate *metrics = [[MLNTestMetricsDelegate alloc] init];
    configuration.metricsDelegate = metrics;

    dispatch_queue_t queue = dispatch_queue_create("org.maplibre.testTwoCompletionsForOnePathAtOnce",
                                                   DISPATCH_QUEUE_CONCURRENT);
    const NSUInteger rounds = 2000;
    for (NSUInteger i = 0; i < rounds; i++) {
        NSURL *first = [NSURL URLWithString:[NSString stringWithFormat:@"https://example.com/tile?a=%lu", (unsigned long)i]];
        NSURL *second = [NSURL URLWithString:[NSString stringWithFormat:@"https://example.com/tile?b=%lu", (unsigned long)i]];
        [configuration startDownloadEvent:first.relativePath type:@"tile"];
        NSURLResponse *firstResponse = [[NSURLResponse alloc] initWithURL:first MIMEType:nil expectedContentLength:0 textEncodingName:nil];
        NSURLResponse *secondResponse = [[NSURLResponse alloc] initWithURL:second MIMEType:nil expectedContentLength:0 textEncodingName:nil];
        dispatch_group_t group = dispatch_group_create();
        dispatch_group_async(group, queue, ^{
            [configuration stopDownloadEventForResponse:firstResponse];
        });
        dispatch_group_async(group, queue, ^{
            [configuration stopDownloadEventForResponse:secondResponse];
        });
        dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
    }

    // The events reach the delegate on the main queue, after every stop above.
    XCTestExpectation *delivered = [self expectationWithDescription:@"events delivered"];
    dispatch_async(dispatch_get_main_queue(), ^{
        [delivered fulfill];
    });
    [self waitForExpectations:@[delivered] timeout:10.0];
    XCTAssertEqual(metrics.eventCount, rounds);
}

// With no metrics delegate nothing is recorded, so nothing waits to be sent.
- (void)testWithoutAMetricsDelegateNoEventIsRecorded {
    MLNNetworkConfiguration *configuration = [[MLNNetworkConfiguration alloc] init];
    [configuration startDownloadEvent:@"/tile" type:@"tile"];

    // A metrics delegate set later gets no event for that start.
    MLNTestMetricsDelegate *metrics = [[MLNTestMetricsDelegate alloc] init];
    configuration.metricsDelegate = metrics;
    NSURL *url = [NSURL URLWithString:@"https://example.com/tile"];
    [configuration stopDownloadEventForResponse:[[NSURLResponse alloc] initWithURL:url MIMEType:nil expectedContentLength:0 textEncodingName:nil]];

    XCTestExpectation *delivered = [self expectationWithDescription:@"main queue drained"];
    dispatch_async(dispatch_get_main_queue(), ^{
        [delivered fulfill];
    });
    [self waitForExpectations:@[delivered] timeout:10.0];
    XCTAssertEqual(metrics.eventCount, 0u);
}
@end
