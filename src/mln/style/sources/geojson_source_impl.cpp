#include <mln/style/sources/geojson_source_impl.hpp>
#include <mln/tile/tile_id.hpp>
#include <mln/util/constants.hpp>
#include <mln/util/feature.hpp>
#include <mln/util/string.hpp>
#include <mln/util/thread_pool.hpp>
#include <mln/util/identity.hpp>
#include <mln/util/label_steps.hpp>
#include <mln/util/monotonic_timer.hpp>
#include <mln/style/conversion/geojson.hpp>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#endif

#include <mapbox/geojsonvt.hpp>
#include <supercluster.hpp>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <cmath>

namespace mln {
namespace style {

class GeoJSONVTData final : public GeoJSONData {
    void getTile(const CanonicalTileID& id, const std::function<void(TileFeatures)>& fn, bool runSynchronously) final {
        assert(fn);
        if (runSynchronously) {
            fn(slice(*impl, id, sourceID));
        } else {
            sequencedScheduler->scheduleAndReplyValue(
                util::SimpleIdentity::Empty,
                [id,
                 geoJSONVT_impl = this->impl,
                 sourceID_ = sourceID,
                 queued = util::LabelSteps::recording() ? util::MonotonicTimer::now()
                                                        : std::chrono::duration<double>{0}]() -> TileFeatures {
                    return slice(*geoJSONVT_impl, id, sourceID_, queued);
                },
                fn);
        }
    }

    static TileFeatures slice(mapbox::geojsonvt::GeoJSONVT& vt,
                              const CanonicalTileID& id,
                              const std::string& sourceID,
                              std::chrono::duration<double> queued = std::chrono::duration<double>{0}) {
        const util::LabelSteps::Scope step(sourceID, "slice", false, queued);
        return vt.getTile(id.z, id.x, id.y).features;
    }

    Features getChildren(const std::uint32_t) final { return {}; }

    Features getLeaves(const std::uint32_t, const std::uint32_t, const std::uint32_t) final { return {}; }

    std::uint8_t getClusterExpansionZoom(std::uint32_t) final { return 0; }

    friend GeoJSONData;
    GeoJSONVTData(const GeoJSON& geoJSON,
                  const mapbox::geojsonvt::Options& options,
                  std::shared_ptr<Scheduler> sequencedScheduler_,
                  std::string sourceID_)
        : impl(std::make_shared<mapbox::geojsonvt::GeoJSONVT>(geoJSON, options)),
          sequencedScheduler(std::move(sequencedScheduler_)),
          sourceID(std::move(sourceID_)) {
        assert(sequencedScheduler);
    }

    std::shared_ptr<mapbox::geojsonvt::GeoJSONVT> impl; // Accessed on worker thread.
    std::shared_ptr<Scheduler> sequencedScheduler;
    const std::string sourceID;
};

class SuperclusterData final : public GeoJSONData {
    void getTile(const CanonicalTileID& id, const std::function<void(TileFeatures)>& fn, bool) final {
        assert(fn);
        fn(impl.getTile(id.z, id.x, id.y));
    }

    Features getChildren(const std::uint32_t cluster_id) final { return impl.getChildren(cluster_id); }

    Features getLeaves(const std::uint32_t cluster_id, const std::uint32_t limit, const std::uint32_t offset) final {
        return impl.getLeaves(cluster_id, limit, offset);
    }

    std::uint8_t getClusterExpansionZoom(std::uint32_t cluster_id) final {
        return impl.getClusterExpansionZoom(cluster_id);
    }

    friend GeoJSONData;
    SuperclusterData(const Features& features, const mapbox::supercluster::Options& options)
        : impl(features, options) {}
    mapbox::supercluster::Supercluster impl;
};

template <class T>
T evaluateFeature(const mapbox::feature::feature<double>& f,
                  const std::shared_ptr<expression::Expression>& expression,
                  std::optional<T> accumulated = std::nullopt) {
    const expression::EvaluationResult result = expression->evaluate(accumulated, f);
    if (result) {
        std::optional<T> typed = expression::fromExpressionValue<T>(*result);
        if (typed) {
            return std::move(*typed);
        }
    }
    return T();
}

// static
GeoJSONData::ParseResult GeoJSONData::parse(const std::string& json,
                                            std::shared_ptr<Scheduler> sequencedScheduler,
                                            const Immutable<GeoJSONOptions>& options,
                                            const std::string& sourceID) {
    conversion::Error error;
    std::optional<GeoJSON> geoJSON = conversion::parseGeoJSON(json, error);
    if (!geoJSON) {
        return "Failed to parse GeoJSON data: " + error.message;
    }
    return create(*geoJSON, std::move(sequencedScheduler), options, sourceID);
}

// static
std::shared_ptr<GeoJSONData> GeoJSONData::create(const GeoJSON& geoJSON,
                                                 std::shared_ptr<Scheduler> sequencedScheduler,
                                                 const Immutable<GeoJSONOptions>& options,
                                                 const std::string& sourceID) {
    constexpr double scale = util::EXTENT / util::tileSize_D;
    if (options->cluster && geoJSON.is<Features>() && !geoJSON.get<Features>().empty()) {
        mapbox::supercluster::Options clusterOptions;
        clusterOptions.maxZoom = options->clusterMaxZoom;
        clusterOptions.extent = util::EXTENT;
        clusterOptions.radius = static_cast<uint16_t>(::round(scale * options->clusterRadius));
        clusterOptions.minPoints = options->clusterMinPoints;

        auto feature = std::make_shared<Feature>();
        clusterOptions.map = [feature, options](const PropertyMap& properties) -> PropertyMap {
            PropertyMap ret{};
            if (properties.empty()) return ret;
            for (const auto& p : options->clusterProperties) {
                feature->properties = properties;
                ret[p.first] = evaluateFeature<Value>(*feature, p.second.first);
            }
            return ret;
        };
        clusterOptions.reduce = [feature, options](PropertyMap& toReturn, const PropertyMap& toFill) {
            for (const auto& p : options->clusterProperties) {
                if (!toFill.contains(p.first)) {
                    continue;
                }
                feature->properties = toFill;
                std::optional<Value> accumulated(toReturn[p.first]);
                toReturn[p.first] = evaluateFeature<Value>(*feature, p.second.second, accumulated);
            }
        };
        return std::shared_ptr<GeoJSONData>(new SuperclusterData(geoJSON.get<Features>(), clusterOptions));
    }

    mapbox::geojsonvt::Options vtOptions;
    vtOptions.maxZoom = options->maxzoom;
    vtOptions.extent = util::EXTENT;
    vtOptions.buffer = static_cast<uint16_t>(::round(scale * options->buffer));
    vtOptions.tolerance = scale * options->tolerance;
    vtOptions.lineMetrics = options->lineMetrics;
    // The geojson-vt index build, on whichever thread makes the data.
    const util::LabelSteps::Scope step(sourceID, "index");
    return std::shared_ptr<GeoJSONData>(new GeoJSONVTData(geoJSON, vtOptions, std::move(sequencedScheduler), sourceID));
}

GeoJSONSource::Impl::Impl(std::string id_, Immutable<GeoJSONOptions> options_)
    : Source::Impl(SourceType::GeoJSON, std::move(id_)),
      options(std::move(options_)) {}

GeoJSONSource::Impl::Impl(const GeoJSONSource::Impl& other,
                          std::shared_ptr<GeoJSONData> data_,
                          uint64_t dataGeneration_)
    : Source::Impl(other),
      options(other.options),
      data(std::move(data_)),
      dataGeneration(dataGeneration_),
      overrideSynchronousUpdate(other.overrideSynchronousUpdate) {}

GeoJSONSource::Impl::~Impl() = default;

Range<uint8_t> GeoJSONSource::Impl::getZoomRange() const {
    return {options->minzoom, options->maxzoom};
}

std::weak_ptr<GeoJSONData> GeoJSONSource::Impl::getData() const {
    return data;
}

std::optional<std::string> GeoJSONSource::Impl::getAttribution() const {
    return {};
}

bool GeoJSONSource::Impl::isUpdateSynchronous() const {
    return options->synchronousUpdate || overrideSynchronousUpdate;
}

bool GeoJSONSource::Impl::usesLabelLane() const {
    return options->labelLane;
}

void GeoJSONSource::Impl::setOverrideSynchronousUpdate(bool newOverride) const {
    overrideSynchronousUpdate = newOverride;
}

} // namespace style
} // namespace mln
