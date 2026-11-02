#pragma once

#include <cstdint>
#include <exception>
#include <string>

namespace mln {
namespace style {

class Source;

class SourceObserver {
public:
    virtual ~SourceObserver() = default;

    virtual void onSourceLoaded(Source&) {}
    virtual void onSourceChanged(Source&) {}
    virtual void onSourceError(Source&, std::exception_ptr) {}
    /// A GeoJSON body of this generation will not land (`GeoJSONSource`).
    virtual void onSourceBodyFailed(Source&, uint64_t /*generation*/, const std::string& /*message*/) {}

    // Source description needs to be reloaded
    virtual void onSourceDescriptionChanged(Source&) {}
};

} // namespace style
} // namespace mln
