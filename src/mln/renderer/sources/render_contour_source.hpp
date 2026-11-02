#pragma once

#include <mln/renderer/sources/render_raster_dem_source.hpp>
#include <mln/renderer/sources/render_tile_source.hpp>
#include <mln/style/sources/contour_source_impl.hpp>
#include <mln/tile/contour_tile.hpp>

#include <mapbox/std/weak.hpp>

#include <map>
#include <optional>

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

    // Listener body: when an upstream DEM tile settles, queue the matching
    // contour tile in our pyramid (same OverscaledTileID) to be populated, or
    // completed with no lines if the DEM tile has no data.
    void onUpstreamTileLoaded(const RasterDEMTile&);

    // Populates the tiles in `pendingPopulate`, once the render thread's
    // current messages are handled: one DEM arrival notifies up to nine
    // tiles, and several arrivals often come together, so each tile is
    // populated once, from the newest snapshot of its DEM. No snapshot: the
    // DEM tile has no data.
    void populatePending();
    std::map<OverscaledTileID, std::optional<ContourTile::DEMSnapshot>> pendingPopulate;
    bool populateScheduled = false;

    // The DEM source this source listens to. A weak pointer, not its address:
    // a new source can get the address of a destroyed one, and must still be
    // seen as new.
    mapbox::base::WeakPtr<RenderRasterDEMSource> upstream;
    std::optional<RenderRasterDEMSource::ListenerHandle> listenerHandle;

    mapbox::base::WeakPtrFactory<RenderContourSource> weakFactory{this};
    // Do not add members here, see `WeakPtrFactory`.
};

} // namespace mln
