#pragma once

#include <mln/style/source_impl.hpp>
#include <mln/style/sources/geojson_source.hpp>
#include <mln/util/range.hpp>

namespace mln {

class AsyncRequest;
class CanonicalTileID;

namespace style {

class GeoJSONSource::Impl final : public Source::Impl {
public:
    Impl(std::string id, Immutable<GeoJSONOptions>);
    Impl(const GeoJSONSource::Impl&, std::shared_ptr<GeoJSONData>, uint64_t dataGeneration);
    ~Impl() final;

    Range<uint8_t> getZoomRange() const;
    std::weak_ptr<GeoJSONData> getData() const;
    /// The generation of `getData()` (see `GeoJSONSource`); 0 for none yet.
    uint64_t getDataGeneration() const { return dataGeneration; }
    const Immutable<GeoJSONOptions>& getOptions() const { return options; }
    /// Names the `GeoJSONSource` object this impl came from; every copy of it
    /// keeps the number. The generations count per source object, so a render
    /// source that outlives a style rebuild (the orchestrator keeps one for an
    /// id and type that match) compares this before it trusts what it reported.
    uint64_t getSourceInstance() const { return sourceInstance; }

    std::optional<std::string> getAttribution() const final;

    bool isUpdateSynchronous() const final;
    bool usesLabelLane() const final;
    void setOverrideSynchronousUpdate(bool newOverride) const;
    /// Bodies are laid out but not landed (`GeoJSONSource::setHoldLanding`).
    bool holdsLanding() const { return holdLanding; }
    void setHoldLanding(bool value) { holdLanding = value; }

private:
    Immutable<GeoJSONOptions> options;
    std::shared_ptr<GeoJSONData> data;
    uint64_t dataGeneration = 0;
    uint64_t sourceInstance = 0;
    mutable bool overrideSynchronousUpdate = false;
    bool holdLanding = false;
};

} // namespace style
} // namespace mln
