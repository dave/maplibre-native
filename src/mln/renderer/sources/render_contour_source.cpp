#include <mln/renderer/sources/render_contour_source.hpp>
#include <mln/algorithm/contour/intervals.hpp>
#include <mln/renderer/render_orchestrator.hpp>
#include <mln/renderer/tile_parameters.hpp>
#include <mln/tile/contour_tile.hpp>
#include <mln/tile/raster_dem_tile.hpp>
#include <mln/util/constants.hpp>
#include <mln/util/logging.hpp>
#include <mln/util/string.hpp>

#include <mln/actor/scheduler.hpp>

#include <cmath>
#include <utility>

namespace mln {

using namespace style;

namespace {

// Resolve the source's `majorMultiplier` schedule at a zoom to a single positive
// integer. Returns 0 when the schedule is empty or invalid (callers treat 0 as
// "never major").
std::int64_t resolveMajorMultiplier(const algorithm::contour::IntervalSchedule& schedule, double zoom) {
    const double v = algorithm::contour::intervalForZoom(schedule, zoom);
    if (v <= 0.0) return 0;
    return static_cast<std::int64_t>(std::llround(v));
}

// How to build the lines of a tile. Its schedules are read at the map zoom the
// tile is drawn at. That is the whole zoom the tile pyramid covers, which is the
// map zoom rounded (a contour source covers the zoom its raster-dem source
// covers), and is not capped at the DEM's maxzoom as the canonical zoom is. A
// tile's zoom is that map zoom + 1 with 256 px tiles.
ContourTile::LineOptions lineOptions(const style::ContourSourceOptions& options,
                                     const OverscaledTileID& id,
                                     uint16_t tileSize) {
    const double zoom = id.overscaledZ - std::log2(util::tileSize_D / tileSize);
    return {.intervalDisplayUnits = algorithm::contour::intervalForZoom(options.intervals, zoom),
            .majorMultiplier = resolveMajorMultiplier(options.majorMultiplier, zoom),
            .unit = options.unit};
}

} // namespace

RenderContourSource::RenderContourSource(Immutable<style::ContourSource::Impl> impl_,
                                         const TaggedScheduler& threadPool_)
    : RenderTileSource(std::move(impl_), threadPool_) {}

// RenderOrchestrator destroys sibling sources in no set order, so the upstream
// may already be gone. Then its listeners went with it.
RenderContourSource::~RenderContourSource() {
    if (RenderRasterDEMSource* dem = upstream.get(); dem != nullptr && listenerHandle) {
        dem->removeTileLoadListener(*listenerHandle);
    }
}

const style::ContourSource::Impl& RenderContourSource::impl() const {
    return static_cast<const style::ContourSource::Impl&>(*baseImpl);
}

void RenderContourSource::update(Immutable<style::Source::Impl> baseImpl_,
                                 const std::vector<Immutable<LayerProperties>>& layers,
                                 const bool needsRendering,
                                 const bool needsRelayout,
                                 const TileParameters& parameters) {
    std::swap(baseImpl, baseImpl_);
    enabled = needsRendering;

    rebindUpstream(parameters);

    // While the DEM source or its tileset is not there, the contour source has
    // no tiles: nothing could arrive for them, and the tile coords are not
    // known. The style's own load state covers a tileset that is still loading.
    RenderRasterDEMSource* const dem = upstream.get();
    const auto demZoomRange = dem != nullptr ? dem->getZoomRange() : std::nullopt;
    if (!demZoomRange) {
        tilePyramid.update(
            layers, /*needsRendering=*/false, needsRelayout, parameters, *baseImpl, util::tileSize_I, {0, 0}, {}, {});
        return;
    }

    // Make the tiles the DEM source makes, so that each contour tile has its
    // DEM tile and hears when it settles. The same OverscaledTileIDs need the
    // DEM's tile size, zoom range and bounds, and its source settings for the
    // pyramid (prefetch, parent tiles). At display zoom > maxzoom both pyramids
    // overscale in lock-step: same canonical tile, same line geometry reused
    // for the bigger screen area.
    tilePyramid.update(layers,
                       needsRendering,
                       needsRelayout,
                       parameters,
                       *dem->baseImpl,
                       dem->getTileSize(),
                       *demZoomRange,
                       dem->getBounds(),
                       [&](const OverscaledTileID& tileID, TileObserver* observer_) {
                           auto tile = std::make_unique<ContourTile>(tileID, impl().id, parameters, observer_);
                           // If the DEM tile has already settled, settle this one
                           // now: its notification came before this tile existed.
                           if (const RasterDEMTile* demTile = dem->getTile(tileID)) {
                               if (demTile->isRenderable()) {
                                   tile->populateFromDEM(*demTile,
                                                         lineOptions(impl().getOptions(), tileID, dem->getTileSize()));
                               } else if (demTile->isComplete()) {
                                   tile->setNoData();
                               }
                           }
                           return tile;
                       });
}

void RenderContourSource::rebindUpstream(const TileParameters& parameters) {
    // Resolve the upstream raster-dem source by ID for this frame. A null
    // result means the upstream source has been removed from the style or
    // hasn't been added yet — either way, our cached pointer must drop.
    RenderRasterDEMSource* resolved = nullptr;
    if (parameters.getRenderSource) {
        if (RenderSource* found = parameters.getRenderSource(impl().getOptions().sourceID)) {
            if (found->baseImpl->type == SourceType::RasterDEM) {
                resolved = static_cast<RenderRasterDEMSource*>(found);
            } else {
                Log::Warning(Event::Style,
                             "contour source `" + impl().id + "` references `" + impl().getOptions().sourceID +
                                 "` which is not a raster-dem source");
            }
        }
    }

    RenderRasterDEMSource* const previous = upstream.get();
    if (resolved == previous) {
        return; // No change — keep existing subscription as-is.
    }

    // The upstream changed (added, removed, or swapped). If the previous one is
    // gone its listeners went with it. Tiles and snapshots from it do not
    // describe the new upstream, and a tile still waiting on it would wait for
    // ever.
    if (previous != nullptr && listenerHandle) {
        previous->removeTileLoadListener(*listenerHandle);
    }
    listenerHandle.reset();
    pendingPopulate.clear();
    tilePyramid.clearAll();
    upstream = resolved != nullptr ? resolved->makeWeakPtr() : mapbox::base::WeakPtr<RenderRasterDEMSource>{};
    if (resolved != nullptr) {
        // Capture both `this` (for member access) and a WeakPtr (for
        // liveness). The lock-and-check pattern keeps `this` pinned during
        // the call and bails out if the source has been destroyed.
        listenerHandle = resolved->addTileLoadListener(
            [this, weak = weakFactory.makeWeakPtr()](const RasterDEMTile& demTile) {
                if (auto guard = weak.lock(); weak) {
                    onUpstreamTileLoaded(demTile);
                }
            });
    }
}

void RenderContourSource::onUpstreamTileLoaded(const RasterDEMTile& demTile) {
    // One DEM arrival notifies its own tile and up to eight neighbours whose
    // borders it backfilled, and several arrivals are often handled in one
    // go. Keep the newest snapshot for each tile and populate each once,
    // after the render thread's current messages. A snapshot, not the DEM
    // tile: the DEM source and its tiles can go away before then. No snapshot:
    // the DEM tile has no data, and the contour tile completes with no lines.
    pendingPopulate.insert_or_assign(demTile.id, ContourTile::snapshot(demTile));
    if (populateScheduled) {
        return;
    }
    populateScheduled = true;
    Scheduler::GetCurrent()->schedule([weak = weakFactory.makeWeakPtr(), this] {
        if (auto guard = weak.lock(); weak) {
            populatePending();
        }
    });
}

void RenderContourSource::populatePending() {
    populateScheduled = false;
    const auto pending = std::exchange(pendingPopulate, {});
    const RenderRasterDEMSource* const dem = upstream.get();
    if (dem == nullptr) {
        // The DEM source has gone since these arrived: the next update drops
        // the tiles built from it.
        return;
    }
    for (const auto& [id, taken] : pending) {
        Tile* match = tilePyramid.getTile(id);
        if (match == nullptr) {
            // No contour tile at this coord (yet, or any more) — the createTile
            // callback settles one made later via getTile, so missing the
            // notify is harmless.
            continue;
        }
        auto& tile = static_cast<ContourTile&>(*match);
        if (!taken) {
            tile.setNoData();
            continue;
        }
        tile.populate(*taken, lineOptions(impl().getOptions(), id, dem->getTileSize()));
    }
}

} // namespace mln
