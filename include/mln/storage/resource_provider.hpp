#pragma once

#include <mln/storage/resource.hpp>

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

} // namespace mln
