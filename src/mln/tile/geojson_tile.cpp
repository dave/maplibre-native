#include <mln/renderer/query.hpp>
#include <mln/renderer/tile_parameters.hpp>
#include <mln/style/sources/geojson_source.hpp>
#include <mln/tile/geojson_tile.hpp>
#include <mln/tile/geojson_tile_data.hpp>
#include <mln/util/instrumentation.hpp>
#include <utility>

namespace mln {

using TileFeatures = style::GeoJSONData::TileFeatures;

GeoJSONTile::GeoJSONTile(const OverscaledTileID& overscaledTileID,
                         std::string sourceID_,
                         const TileParameters& parameters,
                         std::shared_ptr<style::GeoJSONData> data_,
                         TileObserver* observer_,
                         uint64_t dataGeneration)
    : GeometryTile(overscaledTileID, std::move(sourceID_), parameters, observer_) {
    updateData(std::move(data_), false /*needsRelayout*/, parameters.isUpdateSynchronous, dataGeneration);
}

void GeoJSONTile::updateData(std::shared_ptr<style::GeoJSONData> data_,
                             bool needsRelayout,
                             bool runSynchronously,
                             uint64_t dataGeneration) {
    MLN_TRACE_FUNC();

    assert(data_);
    // A newer body replaces one waiting for a landing.
    dataAfterLanding = nullptr;
    data = std::move(data_);
    if (needsRelayout) reset();
    data->getTile(
        id.canonical,
        [this, self = weakFactory.makeWeakPtr(), capturedData = data.get(), dataGeneration](TileFeatures features) {
            // If the data has changed, a new request is being processed, ignore this one
            if (auto guard = self.lock(); self && data.get() == capturedData) {
                dataInWorker = data;
                setBodyGeneration(dataGeneration);
                setData(std::make_unique<GeoJSONTileData>(std::move(features)));
            }
        },
        runSynchronously);
}

void GeoJSONTile::updateDataAfterLanding(std::shared_ptr<style::GeoJSONData> data_,
                                         bool runSynchronously,
                                         uint64_t dataGeneration) {
    dataAfterLanding = std::move(data_);
    generationAfterLanding = dataGeneration;
    synchronousAfterLanding = runSynchronously;
}

void GeoJSONTile::onLanded() {
    if (!dataAfterLanding) return;
    auto next = std::move(dataAfterLanding);
    dataAfterLanding = nullptr;
    updateData(std::move(next), false, synchronousAfterLanding, generationAfterLanding);
}

bool GeoJSONTile::isLaidOutWith(const std::shared_ptr<style::GeoJSONData>& data_) const {
    return !dataInWorker.owner_before(data_) && !data_.owner_before(dataInWorker) && isComplete();
}

void GeoJSONTile::querySourceFeatures(std::vector<Feature>& result,
                                      const SourceQueryOptions& options,
                                      const GlobalStateMap* globalState) {
    MLN_TRACE_FUNC();

    // Ignore the sourceLayer, there is only one
    if (auto tileData = getData()) {
        if (auto layer = tileData->getLayer({})) {
            auto featureCount = layer->featureCount();
            for (std::size_t i = 0; i < featureCount; i++) {
                auto feature = layer->getFeature(i);

                // Apply filter, if any
                if (options.filter && !(*options.filter)(style::expression::EvaluationContext{
                                          static_cast<float>(this->id.overscaledZ), feature.get()}
                                                             .withGlobalState(globalState))) {
                    continue;
                }

                result.push_back(convertFeature(*feature, id.canonical));
            }
        }
    }
}

} // namespace mln
