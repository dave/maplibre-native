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

// Resolve the source's `majorMultiplier` schedule at a tile's canonical
// zoom to a single positive integer. Returns 0 when the schedule is
// empty or invalid (callers treat 0 as "never major").
std::int64_t resolveMajorMultiplier(const algorithm::contour::IntervalSchedule& schedule, double zoom) {
    const double v = algorithm::contour::intervalForZoom(schedule, zoom);
    if (v <= 0.0) return 0;
    return static_cast<std::int64_t>(std::llround(v));
}

} // namespace

RenderContourSource::RenderContourSource(Immutable<style::ContourSource::Impl> impl_,
                                         const TaggedScheduler& threadPool_)
    : RenderTileSource(std::move(impl_), threadPool_) {}

// Intentionally do NOT call upstream->removeTileLoadListener here.
// Sibling-source destruction order in RenderOrchestrator's
// std::unordered_map<std::string, std::unique_ptr<RenderSource>> is
// unspecified, so by the time we run, the upstream raster-dem source
// may already have been destroyed — dereferencing `upstream` would be
// a use-after-free. The listener_set is owned by upstream, so any
// still-registered listener is destroyed when upstream itself is.
// The listener closure captures only a WeakPtr to this source, so it
// safely no-ops if it fires (e.g. mid-notify) after we're gone.
RenderContourSource::~RenderContourSource() = default;

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
    const auto demZoomRange = upstream != nullptr ? upstream->getZoomRange() : std::nullopt;
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
                       *upstream->baseImpl,
                       upstream->getTileSize(),
                       *demZoomRange,
                       upstream->getBounds(),
                       [&](const OverscaledTileID& tileID, TileObserver* observer_) {
                           auto tile = std::make_unique<ContourTile>(tileID, impl().id, parameters, observer_);
                           // If the DEM tile has already settled, settle this one
                           // now: its notification came before this tile existed.
                           if (const RasterDEMTile* dem = upstream->getTile(tileID)) {
                               if (dem->isRenderable()) {
                                   const auto& opts = impl().getOptions();
                                   const double zoom = static_cast<double>(tileID.canonical.z);
                                   const double interval = algorithm::contour::intervalForZoom(opts.intervals, zoom);
                                   const std::int64_t major = resolveMajorMultiplier(opts.majorMultiplier, zoom);
                                   tile->populateFromDEM(*dem, interval, major, opts.unit);
                               } else if (dem->isComplete()) {
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

    if (resolved == upstream) {
        return; // No change — keep existing subscription as-is.
    }

    // Identity changed (added, removed, or swapped). Drop the old handle
    // without dereferencing the (possibly destroyed) previous upstream —
    // its listener_set was destroyed with it, so any registered listener
    // is already gone. Only the handle integer remains, and we can safely
    // forget it.
    listenerHandle.reset();
    upstream = resolved;
    if (upstream != nullptr) {
        // Capture both `this` (for member access) and a WeakPtr (for
        // liveness). The lock-and-check pattern keeps `this` pinned during
        // the call and bails out if the source has been destroyed — the
        // listener_set may outlive us briefly during multi-source teardown.
        listenerHandle = upstream->addTileLoadListener(
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
        const auto& opts = impl().getOptions();
        const double zoom = static_cast<double>(id.canonical.z);
        const double interval = algorithm::contour::intervalForZoom(opts.intervals, zoom);
        const std::int64_t major = resolveMajorMultiplier(opts.majorMultiplier, zoom);
        tile.populate(*taken, interval, major, opts.unit);
    }
}

} // namespace mln
