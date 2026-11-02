#import "MLNResourceProvider.h"

#include <mln/storage/resource_provider.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

MLNResourceKind MLNResourceKindFromKind(mln::Resource::Kind kind) {
  switch (kind) {
    case mln::Resource::Kind::Unknown:
      return MLNResourceKindUnknown;
    case mln::Resource::Kind::Style:
      return MLNResourceKindStyle;
    case mln::Resource::Kind::Source:
      return MLNResourceKindSource;
    case mln::Resource::Kind::Tile:
      return MLNResourceKindTile;
    case mln::Resource::Kind::Glyphs:
      return MLNResourceKindGlyphs;
    case mln::Resource::Kind::SpriteImage:
      return MLNResourceKindSpriteImage;
    case mln::Resource::Kind::SpriteJSON:
      return MLNResourceKindSpriteJSON;
    case mln::Resource::Kind::Image:
      return MLNResourceKindImage;
  }
}

std::string MLNResourceAnswerReason(NSString *why) {
  if (!why) {
    [NSException raise:NSInvalidArgumentException format:@"A resource answer needs its reason."];
  }
  return why.UTF8String;
}

}  // namespace

@interface MLNResourceTile ()
- (instancetype)initWithTile:(const mln::Resource::TileData &)tile;
@end

@implementation MLNResourceTile

- (instancetype)initWithTile:(const mln::Resource::TileData &)tile {
  if (self = [super init]) {
    _urlTemplate = @(tile.urlTemplate.c_str());
    _pixelRatio = tile.pixelRatio;
    _x = tile.x;
    _y = tile.y;
    _z = tile.z;
  }
  return self;
}

@end

@interface MLNResourceRequest ()
- (instancetype)initWithRequest:(const mln::ProviderRequest &)request
                      responder:(std::shared_ptr<mln::ProviderResponder>)responder;
@end

@implementation MLNResourceRequest {
  std::shared_ptr<mln::ProviderResponder> _responder;
}

- (instancetype)initWithRequest:(const mln::ProviderRequest &)request
                      responder:(std::shared_ptr<mln::ProviderResponder>)responder {
  if (self = [super init]) {
    _identifier = request.id;
    _kind = MLNResourceKindFromKind(request.kind);
    _URLString = @(request.url.c_str());
    _sourceIdentifier = @(request.sourceID.c_str());
    if (request.tile) {
      _tile = [[MLNResourceTile alloc] initWithTile:*request.tile];
    }
    _localOnly = request.localOnly;
    _lowPriority = request.priority == mln::Resource::Priority::Low;
    _responder = std::move(responder);
  }
  return self;
}

- (void)answerWithData:(NSData *)data {
  if (!data) {
    [NSException raise:NSInvalidArgumentException format:@"A resource answer needs its data."];
  }
  _responder->data(
      std::make_shared<const std::string>(static_cast<const char *>(data.bytes), data.length));
}

- (void)answerEmpty {
  _responder->empty();
}

- (void)answerUnavailable:(NSString *)why {
  _responder->unavailable(MLNResourceAnswerReason(why));
}

- (void)answerNotFound:(NSString *)why {
  _responder->notFound(MLNResourceAnswerReason(why));
}

- (void)answerError:(NSString *)why {
  _responder->error(MLNResourceAnswerReason(why));
}

@end

namespace {

/// Keeps the Objective-C provider, and gives its calls on the loader thread, which
/// has no autorelease pool of its own to drain, a pool each.
class MLNResourceProviderAdapter final : public mln::ResourceProvider {
public:
  explicit MLNResourceProviderAdapter(id<MLNResourceProvider> provider_) : provider(provider_) {}

  void start(const mln::ProviderRequest &request,
             std::shared_ptr<mln::ProviderResponder> responder) override {
    @autoreleasepool {
      [provider startRequest:[[MLNResourceRequest alloc] initWithRequest:request
                                                               responder:std::move(responder)]];
    }
  }

  void cancel(uint64_t identifier) override {
    @autoreleasepool {
      [provider cancelRequestWithIdentifier:identifier];
    }
  }

private:
  id<MLNResourceProvider> provider;
};

}  // namespace

@interface MLNResourceProviderCounts ()
- (instancetype)initWithCounts:(const mln::ResourceProviderCounts &)counts;
@end

@implementation MLNResourceProviderCounts

- (instancetype)initWithCounts:(const mln::ResourceProviderCounts &)counts {
  if (self = [super init]) {
    _inFlight = counts.inFlight;
    _peakInFlight = counts.peakInFlight;
  }
  return self;
}

@end

@implementation MLNResourceProviders

+ (void)registerProvider:(id<MLNResourceProvider>)provider forScheme:(NSString *)scheme {
  if (!provider) {
    [NSException raise:NSInvalidArgumentException format:@"A resource provider cannot be nil."];
  }
  try {
    mln::registerResourceProvider(scheme.UTF8String,
                                  std::make_shared<MLNResourceProviderAdapter>(provider));
  } catch (const std::invalid_argument &error) {
    [NSException raise:NSInvalidArgumentException format:@"%s", error.what()];
  }
}

+ (void)unregisterProviderForScheme:(NSString *)scheme {
  mln::unregisterResourceProvider(scheme.UTF8String);
}

+ (MLNResourceProviderCounts *)countsForScheme:(NSString *)scheme {
  const auto counts = mln::getResourceProviderCounts(scheme.UTF8String);
  return counts ? [[MLNResourceProviderCounts alloc] initWithCounts:*counts] : nil;
}

@end
