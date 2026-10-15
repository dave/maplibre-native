#pragma once

#include <mln/style/expression/expression.hpp>
#include <mln/style/source.hpp>
#include <mln/tile/tile_id.hpp>
#include <mln/util/constants.hpp>
#include <mln/util/geojson.hpp>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>

namespace mln {

class AsyncRequest;
class Scheduler;
namespace style {

struct GeoJSONOptions {
    // GeoJSON-VT options
    uint8_t minzoom = 0;
    uint8_t maxzoom = 18;
    uint16_t tileSize = util::tileSize_I;
    uint16_t buffer = 128;
    double tolerance = 0.375;
    bool lineMetrics = false;

    // Supercluster options
    bool cluster = false;
    uint16_t clusterRadius = 50;
    uint8_t clusterMaxZoom = 17;
    size_t clusterMinPoints = 2;
    using ClusterExpression = std::pair<std::shared_ptr<mln::style::expression::Expression>,
                                        std::shared_ptr<mln::style::expression::Expression>>;
    using ClusterProperties = std::map<std::string, ClusterExpression>;
    ClusterProperties clusterProperties;

    // Update options
    bool synchronousUpdate = false;

    // The source's off-thread work runs on the label lane
    // (`Scheduler::GetLabelLane()`), not on the shared worker pool: its body's
    // parse, its tiles' parse and layout, and its geojson-vt cuts (on a
    // sequenced scheduler of its own). Refused with `synchronousUpdate` and
    // with `cluster` (Supercluster cuts on the render thread).
    bool labelLane = false;

    static Immutable<GeoJSONOptions> defaultOptions();
};
class GeoJSONData {
public:
    using TileFeatures = mapbox::feature::feature_collection<int16_t>;
    using Features = mapbox::feature::feature_collection<double>;
    /// `sourceID` names the source for the step recorder (`util::LabelSteps`) only.
    static std::shared_ptr<GeoJSONData> create(const GeoJSON&,
                                               std::shared_ptr<Scheduler> sequencedScheduler,
                                               const Immutable<GeoJSONOptions>& = GeoJSONOptions::defaultOptions(),
                                               const std::string& sourceID = {});

    /// Parses a GeoJSON body and makes its data, or answers the parse error.
    /// Both a URL body and the Android binding's string body come here.
    using ParseResult = std::variant<std::shared_ptr<GeoJSONData>, std::string>;
    static ParseResult parse(const std::string& json,
                             std::shared_ptr<Scheduler> sequencedScheduler,
                             const Immutable<GeoJSONOptions>&,
                             const std::string& sourceID);

    virtual ~GeoJSONData() = default;
    virtual void getTile(const CanonicalTileID&, const std::function<void(TileFeatures)>&, bool runSynchronously) = 0;

    // SuperclusterData
    virtual Features getChildren(std::uint32_t) = 0;
    virtual Features getLeaves(std::uint32_t, std::uint32_t limit, std::uint32_t offset) = 0;
    virtual std::uint8_t getClusterExpansionZoom(std::uint32_t) = 0;
};

// NOTE: Any derived class must invalidate `weakFactory` in the destructor
class GeoJSONSource final : public Source {
public:
    /// Throws `std::invalid_argument` for `labelLane` with
    /// `synchronousUpdate` or with `cluster`, and `std::runtime_error` if the
    /// label lane cannot be made.
    GeoJSONSource(std::string id, Immutable<GeoJSONOptions> = GeoJSONOptions::defaultOptions());
    ~GeoJSONSource() final;

    // Data generations: each new body gets the next number, counting from 1,
    // and so does each change of a layer's filter or layout property on the
    // source (`markLayersChanged`). The renderer reports "every tile in view
    // is laid out at generation N" (`RendererObserver::onSourceLaidOut`).
    // Each setter answers the generation it gave out; `getLatestGeneration`
    // is the latest one given out. Wait for the latest generation read, or a
    // later one: a report of N means the tiles hold every body and layer
    // change given out up to N. So while a body is pending (asked for, not
    // landed), a layer change takes a generation but the source reports
    // nothing as new until the body lands, and the body then takes the
    // latest generation. An older body that lands while a newer one is
    // pending keeps its own, lower, generation.

    /// The generation for a URL body: it is that body's once it has loaded.
    /// A second, different answer for the URL takes a new generation; the
    /// same bytes again are ignored.
    uint64_t setURL(const std::string& url);
    uint64_t setGeoJSON(const GeoJSON&);
    uint64_t setGeoJSONData(std::shared_ptr<GeoJSONData>);
    /// For a body made off the calling thread: `generation` is one that
    /// `nextDataGeneration()` gave when the body was asked for.
    void setGeoJSONData(std::shared_ptr<GeoJSONData>, uint64_t generation);
    /// Takes the next data generation for a body that is still being made.
    uint64_t nextDataGeneration();
    /// The generation the source holds now, the one its tiles will be
    /// reported laid out at; 0 before the first. A body still pending is not
    /// counted until it lands.
    uint64_t getDataGeneration() const;
    /// The latest generation given out: by a setter, `nextDataGeneration`
    /// or a layer change. The one to wait for after any setter, including
    /// one that answers nothing (a platform's URL or layer property).
    uint64_t getLatestGeneration() const;
    /// The body of `generation` could not be made: reported as a source error.
    void setGeoJSONError(uint64_t generation, const std::string& message);
    /// A layer on this source changed its filter or a layout property: the
    /// source takes the next generation, laid out once its tiles are laid out
    /// with the new layers. The style calls it; answers the new generation.
    uint64_t markLayersChanged();

    /// Where a body's parse runs: the label lane for a `labelLane` source,
    /// else the shared worker pool.
    std::shared_ptr<Scheduler> getParseScheduler() const;
    /// Where the source's geojson-vt cuts run; a body's data is made with it.
    std::shared_ptr<Scheduler> getSliceScheduler() const;

    std::optional<std::string> getURL() const;
    const GeoJSONOptions& getOptions() const;

    class Impl;
    const Impl& impl() const;

    void loadDescription(FileSource&) final;

    bool supportsLayerType(const mln::style::LayerTypeInfo*) const override;

    mapbox::base::WeakPtr<Source> makeWeakPtr() override { return weakFactory.makeWeakPtr(); }

    bool isUpdateSynchronous() const noexcept;
    /// Throws `std::logic_error` for `true` on a `labelLane` source.
    void setOverrideSynchronousUpdate(bool);

protected:
    Mutable<Source::Impl> createMutable() const noexcept final;

private:
    uint64_t landingGeneration(uint64_t generation);

    std::optional<std::string> url;
    uint64_t urlGeneration = 0;
    // The URL's last body, to tell a second, different answer from the same one again.
    std::shared_ptr<const std::string> urlBody;
    uint64_t dataGeneration = 0;
    // The newest body asked for (`nextDataGeneration`) and not landed yet; 0 for none.
    uint64_t pendingBody = 0;
    std::unique_ptr<AsyncRequest> req;
    std::atomic<uint64_t> requestGeneration{0};
    std::shared_ptr<Scheduler> parseScheduler;
    std::shared_ptr<Scheduler> sequencedScheduler;
    mapbox::base::WeakPtrFactory<Source> weakFactory{this};
    // Do not add members here, see `WeakPtrFactory`
};

template <>
inline bool Source::is<GeoJSONSource>() const {
    return getType() == SourceType::GeoJSON;
}

} // namespace style
} // namespace mln
