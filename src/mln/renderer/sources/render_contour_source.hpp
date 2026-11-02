#pragma once

#include <mln/renderer/sources/render_raster_dem_source.hpp>
#include <mln/renderer/sources/render_tile_source.hpp>
#include <mln/style/sources/contour_source_impl.hpp>
#include <mln/tile/contour_tile.hpp>

#include <mapbox/std/weak.hpp>

#include <set>

namespace mln {

// Render-side counterpart of style::ContourSource. Doesn't fetch tiles of
// its own — instead it subscribes to tile-load events on the upstream
// raster-dem render source and drives a vector tile pyramid of
// ContourTiles that emit line features per (z, x, y).
class RenderContourSource final : public RenderTileSource {
public:
    explicit RenderContourSource(Immutable<style::ContourSource::Impl>, const TaggedScheduler&);
    ~RenderContourSource() override;

    void update(Immutable<style::Source::Impl>,
                const std::vector<Immutable<style::LayerProperties>>&,
                bool needsRendering,
                bool needsRelayout,
                const TileParameters&) override;

private:
    const style::ContourSource::Impl& impl() const;

    // Re-resolve `upstream` from the current frame's TileParameters. Handles
    // upstream source being added, removed, or swapped between updates. Tiles
    // built from the previous upstream go.
    void rebindUpstream(const TileParameters& parameters);

    // Listener body: an upstream DEM tile settled. Queue the contour tile at the
    // same OverscaledTileID.
    void onUpstreamTileLoaded(const RasterDEMTile&);

    // Settles the queued tiles, once the render thread's current messages are
    // handled: one DEM arrival notifies up to nine tiles, and several arrivals
    // often come together. So each tile takes one copy of its DEM image, the
    // newest, however many notifications it had.
    void populatePending();
    // Populates a contour tile from its DEM tile, or completes it with no lines
    // if the DEM tile has no data. Nothing yet if the DEM tile has not settled.
    void settle(ContourTile&, const RasterDEMTile&, const RenderRasterDEMSource&);
    std::set<OverscaledTileID> pendingPopulate;
    bool populateScheduled = false;

    // The DEM source this source listens to. A weak pointer, not its address:
    // a new source can get the address of a destroyed one, and must still be
    // seen as new.
    mapbox::base::WeakPtr<RenderRasterDEMSource> upstream;
    // The source's name resolved to nothing usable, and that has been logged.
    bool reportedBadSource = false;
    std::optional<RenderRasterDEMSource::ListenerHandle> listenerHandle;

    mapbox::base::WeakPtrFactory<RenderContourSource> weakFactory{this};
    // Do not add members here, see `WeakPtrFactory`.
};

} // namespace mln
