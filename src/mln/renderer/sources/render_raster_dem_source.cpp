#include <mln/renderer/sources/render_raster_dem_source.hpp>
#include <mln/renderer/render_tile.hpp>
#include <mln/tile/raster_dem_tile.hpp>
#include <mln/algorithm/update_tile_masks.hpp>
#include <mln/geometry/dem_data.hpp>
#include <mln/renderer/buckets/hillshade_bucket.hpp>
#include <mln/renderer/tile_parameters.hpp>

namespace mln {

using namespace style;

RenderRasterDEMSource::RenderRasterDEMSource(Immutable<style::TileSource::Impl> impl_,
                                             const TaggedScheduler& threadPool_)
    : RenderTileSetSource(std::move(impl_), threadPool_) {}

const style::TileSource::Impl& RenderRasterDEMSource::impl() const {
    return static_cast<const style::TileSource::Impl&>(*baseImpl);
}

const std::optional<Tileset>& RenderRasterDEMSource::getTileset() const {
    return impl().tileset;
}

void RenderRasterDEMSource::updateInternal(const Tileset& tileset,
                                           const std::vector<Immutable<LayerProperties>>& layers,
                                           const bool needsRendering,
                                           const bool needsRelayout,
                                           const TileParameters& parameters) {
    tilePyramid.update(layers,
                       needsRendering,
                       needsRelayout,
                       parameters,
                       *baseImpl,
                       impl().getTileSize(),
                       tileset.zoomRange,
                       tileset.bounds,
                       [&](const OverscaledTileID& tileID, TileObserver* observer_) {
                           return std::make_unique<RasterDEMTile>(tileID, baseImpl->id, parameters, tileset, observer_);
                       });
    algorithm::updateTileMasks(tilePyramid.getRenderedTiles());
}

void RenderRasterDEMSource::onTileChanged(Tile& tile) {
    auto& demtile = static_cast<RasterDEMTile&>(tile);

    std::map<DEMTileNeighbors, DEMTileNeighbors> opposites = {
        {DEMTileNeighbors::Left, DEMTileNeighbors::Right},
        {DEMTileNeighbors::Right, DEMTileNeighbors::Left},
        {DEMTileNeighbors::TopLeft, DEMTileNeighbors::BottomRight},
        {DEMTileNeighbors::TopCenter, DEMTileNeighbors::BottomCenter},
        {DEMTileNeighbors::TopRight, DEMTileNeighbors::BottomLeft},
        {DEMTileNeighbors::BottomRight, DEMTileNeighbors::TopLeft},
        {DEMTileNeighbors::BottomCenter, DEMTileNeighbors::TopCenter},
        {DEMTileNeighbors::BottomLeft, DEMTileNeighbors::TopRight}};

    if (tile.isRenderable() && demtile.neighboringTiles != DEMTileNeighbors::Complete) {
        const CanonicalTileID canonical = tile.id.canonical;
        const auto dim = static_cast<uint32_t>(std::pow(2, canonical.z));
        const uint32_t px = (canonical.x - 1 + dim) % dim;
        const int pxw = canonical.x == 0 ? tile.id.wrap - 1 : tile.id.wrap;
        const uint32_t nx = (canonical.x + 1 + dim) % dim;
        const int nxw = (canonical.x + 1 == dim) ? tile.id.wrap + 1 : tile.id.wrap;

        auto getNeighbor = [&](DEMTileNeighbors mask) {
            if (mask == DEMTileNeighbors::Left) {
                return OverscaledTileID(tile.id.overscaledZ, pxw, canonical.z, px, canonical.y);
            } else if (mask == DEMTileNeighbors::Right) {
                return OverscaledTileID(tile.id.overscaledZ, nxw, canonical.z, nx, canonical.y);
            } else if (mask == DEMTileNeighbors::TopLeft) {
                return OverscaledTileID(tile.id.overscaledZ, pxw, canonical.z, px, canonical.y - 1);
            } else if (mask == DEMTileNeighbors::TopCenter) {
                return OverscaledTileID(tile.id.overscaledZ, tile.id.wrap, canonical.z, canonical.x, canonical.y - 1);
            } else if (mask == DEMTileNeighbors::TopRight) {
                return OverscaledTileID(tile.id.overscaledZ, nxw, canonical.z, nx, canonical.y - 1);
            } else if (mask == DEMTileNeighbors::BottomLeft) {
                return OverscaledTileID(tile.id.overscaledZ, pxw, canonical.z, px, canonical.y + 1);
            } else if (mask == DEMTileNeighbors::BottomCenter) {
                return OverscaledTileID(tile.id.overscaledZ, tile.id.wrap, canonical.z, canonical.x, canonical.y + 1);
            } else if (mask == DEMTileNeighbors::BottomRight) {
                return OverscaledTileID(tile.id.overscaledZ, nxw, canonical.z, nx, canonical.y + 1);
            } else {
                throw std::runtime_error("mask is not a valid tile neighbor");
            }
        };

        for (uint8_t i = 0; i < 8; i++) {
            auto mask = DEMTileNeighbors(std::pow(2, i));
            // only backfill if this neighbor has not been previously backfilled
            if ((demtile.neighboringTiles & mask) != mask) {
                OverscaledTileID neighborid = getNeighbor(mask);
                Tile* renderableNeighbor = tilePyramid.getTile(neighborid);
                if (renderableNeighbor != nullptr && renderableNeighbor->isRenderable()) {
                    auto& borderTile = static_cast<RasterDEMTile&>(*renderableNeighbor);
                    demtile.backfillBorder(borderTile, mask);

                    // Backfill the neighbour from this tile as well, even when
                    // its bit for this tile is already set. This tile's bit is
                    // clear because it is new or has parsed a new body
                    // (`RasterDEMTile::onParsed` clears its bits), so the
                    // neighbour's border holds this tile's old edge or none.
                    const DEMTileNeighbors& borderMask = opposites[mask];
                    borderTile.backfillBorder(demtile, borderMask);
                    // Notify cross-source consumers: borderTile's DEMData
                    // border just changed, so anything derived from it (e.g.
                    // contour-line tiles, which read the buffered view with
                    // its border) needs to regenerate so the new edge values
                    // make it into emitted geometry.
                    if (borderTile.isRenderable()) {
                        tileLoadListeners.notify(borderTile);
                    }
                }
            }
        }
    }
    RenderTileSource::onTileChanged(tile);

    // Fan out to cross-source consumers (contour source, etc.) after the
    // parent class has updated render state. The tile has parsed a body: it
    // has DEMData, or (no content) it has none, and listeners need to hear
    // that as well.
    tileLoadListeners.notify(demtile);
}

void RenderRasterDEMSource::onTileError(Tile& tile, std::exception_ptr error) {
    RenderSource::onTileError(tile, std::move(error));

    // A tile with data keeps it through a failed refresh: nothing changed for
    // a listener. A tile with none will have no data.
    if (!tile.isRenderable()) {
        tileLoadListeners.notify(static_cast<const RasterDEMTile&>(tile));
    }
}

RenderRasterDEMSource::ListenerHandle RenderRasterDEMSource::addTileLoadListener(TileLoadListener listener) {
    // Replay the tiles that have settled to *this* listener only so a
    // late-registering consumer sees what arrived before it existed, without
    // re-notifying listeners that already saw those tiles.
    for (const auto& [_, tilePtr] : tilePyramid.getTiles()) {
        if (tilePtr && (tilePtr->isRenderable() || tilePtr->isComplete())) {
            listener(static_cast<const RasterDEMTile&>(*tilePtr));
        }
    }
    return tileLoadListeners.add(std::move(listener));
}

void RenderRasterDEMSource::removeTileLoadListener(ListenerHandle handle) {
    tileLoadListeners.remove(handle);
}

const RasterDEMTile* RenderRasterDEMSource::getTile(const OverscaledTileID& id) const {
    auto it = tilePyramid.getTiles().find(id);
    if (it == tilePyramid.getTiles().end()) return nullptr;
    return static_cast<const RasterDEMTile*>(it->second.get());
}

std::optional<Range<std::uint8_t>> RenderRasterDEMSource::getZoomRange() const {
    if (const auto& tileset = impl().tileset) {
        return tileset->zoomRange;
    }
    return std::nullopt;
}

uint16_t RenderRasterDEMSource::getTileSize() const {
    return impl().getTileSize();
}

std::optional<LatLngBounds> RenderRasterDEMSource::getBounds() const {
    if (const auto& tileset = impl().tileset) {
        return tileset->bounds;
    }
    return std::nullopt;
}

std::unordered_map<std::string, std::vector<Feature>> RenderRasterDEMSource::queryRenderedFeatures(
    const ScreenLineString&,
    const TransformState&,
    const std::unordered_map<std::string, const RenderLayer*>&,
    const RenderedQueryOptions&,
    const GlobalStateMap*,
    const mat4&) const {
    return std::unordered_map<std::string, std::vector<Feature>>{};
}

std::vector<Feature> RenderRasterDEMSource::querySourceFeatures(const SourceQueryOptions&,
                                                                const GlobalStateMap*) const {
    return {};
}

} // namespace mln
