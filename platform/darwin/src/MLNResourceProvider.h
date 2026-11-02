#import <Foundation/Foundation.h>

#import "MLNFoundation.h"
#import "MLNOfflineStorage.h"

NS_ASSUME_NONNULL_BEGIN

/** The tile a request asks for (``MLNResourceRequest/tile``). */
MLN_EXPORT
@interface MLNResourceTile : NSObject

- (instancetype)init NS_UNAVAILABLE;

/** The source's URL template. */
@property (nonatomic, readonly, copy) NSString *urlTemplate;

/** The tile's pixel ratio. */
@property (nonatomic, readonly) NSUInteger pixelRatio;

@property (nonatomic, readonly) NSInteger x;
@property (nonatomic, readonly) NSInteger y;
@property (nonatomic, readonly) NSInteger z;

@end

/**
 One request to an ``MLNResourceProvider``. Each request has its own object and
 identifier.

 Answer a request once, on any thread. The first answer wins. An answer after
 the request was dropped is ignored, and a second answer is logged. A nil argument
 raises an `NSInvalidArgumentException`. Nothing times a request out: one that is
 never answered waits for ever.

 A tile asks in two steps: `localOnly`, then a fetch unless the first step gave
 data or "empty". Any other resource asks once, with `localOnly` set to `NO`.

 | Answer      | `localOnly` step | Fetch step, tile | Fetch step, other |
 |-------------|------------------|------------------|-------------------|
 | data        | data             | data             | data              |
 | empty       | no content       | no content       | no content        |
 | unavailable | not stored       | connection error | connection error  |
 | not found   | not stored       | no content       | not-found error   |
 | error       | not stored       | other error      | other error       |

 A tile whose answer was data or "empty" is not asked for again, even when it is
 taken back from the tile cache: a URL names its data, so a new URL is the way to
 new data.
 */
MLN_EXPORT
@interface MLNResourceRequest : NSObject

- (instancetype)init NS_UNAVAILABLE;

/** Process-wide and increasing, never 0 and never reused. */
@property (nonatomic, readonly) uint64_t identifier;

@property (nonatomic, readonly) MLNResourceKind kind;

/** The full URL, with its query, as the style gave it. */
@property (nonatomic, readonly, copy) NSString *URLString;

/** The style source that asked; empty for glyphs, sprites, images and styles. */
@property (nonatomic, readonly, copy) NSString *sourceIdentifier;

/** The tile, for a tile; `nil` for any other resource. */
@property (nonatomic, readonly, nullable) MLNResourceTile *tile;

/** Read stored data only, and do no slow work. A tile asks with this first. */
@property (nonatomic, readonly) BOOL localOnly;

/** The request is not urgent. */
@property (nonatomic, readonly, getter=isLowPriority) BOOL lowPriority;

/**
 Answers with the resource's bytes, with no transfer encoding.

 @param data The decoded bytes. They are copied.
 */
- (void)answerWithData:(NSData *)data;

/** Answers that the resource exists and holds nothing. */
- (void)answerEmpty;

/**
 Answers that there is no data now, for a reason that can pass: offline, a
 timeout.

 @param why The reason, for the map's error reports.
 */
- (void)answerUnavailable:(NSString *)why;

/**
 Answers that the resource does not exist.

 @param why The reason, for the map's error reports.
 */
- (void)answerNotFound:(NSString *)why;

/**
 Answers with a defect, such as a storage or decode failure.

 @param why The reason, for the map's error reports.
 */
- (void)answerError:(NSString *)why;

@end

/**
 Serves the resources of a URL scheme from inside the process. The URLs of a
 registered scheme (``MLNResourceProviders``) go to no other file source: no disk
 cache, no network.

 Both methods run on the resource loader thread, which every map shares: they
 must return at once, and the answer comes later, on any thread. They are not
 wrapped in an exception handler: an exception they raise is a programmer error,
 and ends the process.
 */
@protocol MLNResourceProvider <NSObject>

/**
 Starts a request. Answer it through its `answer...` methods.

 @param request The request. Keep it until you answer.
 */
- (void)startRequest:(MLNResourceRequest *)request;

/**
 Tells the provider that a request was dropped before it was answered. It is
 called once for such a request, and never for one that was answered first.

 @param identifier The request's ``MLNResourceRequest/identifier``.
 */
- (void)cancelRequestWithIdentifier:(uint64_t)identifier;

@end

/** A provider's requests from start until answer or cancel. Nothing limits them. */
MLN_EXPORT
@interface MLNResourceProviderCounts : NSObject

- (instancetype)init NS_UNAVAILABLE;

/** The requests in flight now. */
@property (nonatomic, readonly) NSUInteger inFlight;

/** The most in flight at once since the provider was registered. */
@property (nonatomic, readonly) NSUInteger peakInFlight;

@end

/** The resource providers of the process. Every method is safe on any thread. */
MLN_EXPORT
@interface MLNResourceProviders : NSObject

- (instancetype)init NS_UNAVAILABLE;

/**
 Sets the provider of a URL scheme for the whole process, for every map and
 snapshotter, in place of any before it. The provider is kept until it is
 replaced or unregistered. A request already started stays with its provider.

 An empty scheme or a nil provider raises an `NSInvalidArgumentException`.

 @param provider The provider.
 @param scheme The text before "://".
 */
+ (void)registerProvider:(id<MLNResourceProvider>)provider forScheme:(NSString *)scheme;

/**
 Removes the provider of a URL scheme. Requests already started stay with it.

 @param scheme The text before "://".
 */
+ (void)unregisterProviderForScheme:(NSString *)scheme;

/**
 The counts of the provider now registered for a URL scheme.

 @param scheme The text before "://".
 @return The counts, or `nil` if the scheme has no provider.
 */
+ (nullable MLNResourceProviderCounts *)countsForScheme:(NSString *)scheme;

@end

NS_ASSUME_NONNULL_END
