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
    // upstream source being added, removed, or swapped between updates.
    // Drops a stale listener handle without dereferencing the previous
    // upstream (its listener_set was destroyed with it).
    void rebindUpstream(const TileParameters& parameters);

    // Listener body: when an upstream DEM tile finishes parsing, locate the
    // matching contour tile in our pyramid (same OverscaledTileID) and call
    // populateFromDEM on it.
    void onUpstreamTileLoaded(const RasterDEMTile&);

    // Populates the tiles in `pendingPopulate`, once the render thread's
    // current messages are handled: one DEM arrival notifies up to nine
    // tiles, and several arrivals often come together, so each tile is
    // populated once, from the newest snapshot of its DEM.
    void populatePending();
    std::map<OverscaledTileID, ContourTile::DEMSnapshot> pendingPopulate;
    bool populateScheduled = false;

    // Cached upstream pointer for use within a single update() call. Cleared
    // in rebindUpstream when the resolved upstream changes (so we never
    // dereference a dangling pointer). Not used in the destructor — see the
    // dtor body for why.
    RenderRasterDEMSource* upstream = nullptr;
    std::optional<RenderRasterDEMSource::ListenerHandle> listenerHandle;

    mapbox::base::WeakPtrFactory<RenderContourSource> weakFactory{this};
    // Do not add members here, see `WeakPtrFactory`.
};

} // namespace mln
