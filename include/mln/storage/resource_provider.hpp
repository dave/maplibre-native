#pragma once

#include <mln/storage/resource.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace mln {

/// One request to a `ResourceProvider`. Each request has its own object and id.
struct ProviderRequest {
    /// Process-wide and increasing, never 0 and never reused.
    uint64_t id;
    Resource::Kind kind;
    /// The full URL, with its query.
    std::string url;
    /// The style source that asked; empty for glyphs, sprites, images and styles.
    std::string sourceID;
    /// For a tile.
    std::optional<Resource::TileData> tile;
    /// Read stored data only, and do no slow work. A tile asks with this first.
    bool localOnly;
    Resource::Priority priority;
};

/// Answers one request. Every call is safe on any thread. The first call wins.
/// A call after the request was dropped is ignored, and a second answer is logged.
///
/// A tile asks in two steps: `localOnly`, then a fetch unless the first step gave
/// data or "empty". Any other resource asks once, with `localOnly` false.
///
/// | Answer      | `localOnly` step   | Fetch step, tile   | Fetch step, other  |
/// |-------------|--------------------|--------------------|--------------------|
/// | data        | data               | data               | data               |
/// | empty       | no content         | no content         | no content         |
/// | unavailable | not stored         | connection error   | connection error   |
/// | notFound    | not stored         | no content         | not-found error    |
/// | error       | not stored         | other error        | other error        |
///
/// A tile whose answer was data or "empty" is not asked for again, even when it is
/// taken back from the tile cache: a URL names its data, so a new URL is the way to
/// new data. See also `TileSource::reloadTiles`.
class ProviderResponder {
public:
    virtual ~ProviderResponder() = default;

    /// The resource's decoded bytes (no transfer encoding).
    virtual void data(std::shared_ptr<const std::string> bytes) = 0;
    /// The resource exists and holds nothing.
    virtual void empty() = 0;
    /// There is no data now, for a reason that can pass: offline, a timeout.
    virtual void unavailable(std::string why) = 0;
    /// The resource does not exist.
    virtual void notFound(std::string why) = 0;
    /// A defect, such as a storage or decode failure.
    virtual void error(std::string why) = 0;
};

/// Serves the resources of a URL scheme from inside the process. The URLs of a
/// registered scheme go to no other file source: no disk cache, no network.
class ResourceProvider {
public:
    virtual ~ResourceProvider() = default;

    /// Runs on the resource loader thread, which every map shares: it must return
    /// at once and answer later, on any thread.
    virtual void start(const ProviderRequest&, std::shared_ptr<ProviderResponder>) = 0;
    /// Runs on the resource loader thread, and must return at once. The request
    /// was dropped before it was answered.
    virtual void cancel(uint64_t id) = 0;
};

/// Sets the provider of a URL scheme (the text before "://") for the whole
/// process, for every map and snapshotter, in place of any before it. A request
/// already started stays with its provider. Safe on any thread. Throws
/// `std::invalid_argument` for an empty scheme or a null provider.
void registerResourceProvider(const std::string& scheme, std::shared_ptr<ResourceProvider>);
/// Removes the provider of a URL scheme. Requests already started stay with it.
void unregisterResourceProvider(const std::string& scheme);

/// The provider's requests from start until answer or cancel. Nothing limits them.
struct ResourceProviderCounts {
    std::size_t inFlight = 0;
    /// The most in flight at once since the provider was registered.
    std::size_t peakInFlight = 0;
};
/// For the provider now registered for a URL scheme; none if there is none.
std::optional<ResourceProviderCounts> getResourceProviderCounts(const std::string& scheme);

} // namespace mln
