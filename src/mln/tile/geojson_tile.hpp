#pragma once

#include <mln/tile/geometry_tile.hpp>
#include <mln/util/feature.hpp>

namespace mln {
namespace style {
class GeoJSONData;
} // namespace style

class TileParameters;

// NOTE: Any derived class must invalidate `weakFactory` in the destructor
class GeoJSONTile final : public GeometryTile {
public:
    GeoJSONTile(const OverscaledTileID&,
                std::string sourceID,
                const TileParameters&,
                std::shared_ptr<style::GeoJSONData>,
                TileObserver* observer = nullptr);

    void updateData(std::shared_ptr<style::GeoJSONData> data, bool needsRelayout, bool runSynchronously);

    /// Whether the tile's layout is complete and was made from `data`'s cut.
    bool isLaidOutWith(const std::shared_ptr<style::GeoJSONData>& data) const;

    void querySourceFeatures(std::vector<Feature>& result,
                             const SourceQueryOptions&,
                             const GlobalStateMap* globalState) override;

private:
    std::shared_ptr<style::GeoJSONData> data;
    // The data whose cut the worker was last given. Weak, and compared by
    // owner, so a new body at a freed one's address never matches it.
    std::weak_ptr<style::GeoJSONData> dataInWorker;
    mapbox::base::WeakPtrFactory<GeoJSONTile> weakFactory{this};
    // Do not add members here, see `WeakPtrFactory`
};

} // namespace mln
