#pragma once

#include <mln/renderer/sources/render_tile_source.hpp>
#include <mln/style/sources/tile_source_impl.hpp>
#include <mln/util/listener_set.hpp>

namespace mln {

class RasterDEMTile;

class RenderRasterDEMSource final : public RenderTileSetSource {
public:
    explicit RenderRasterDEMSource(Immutable<style::TileSource::Impl>, const TaggedScheduler&);

    std::unordered_map<std::string, std::vector<Feature>> queryRenderedFeatures(
        const ScreenLineString& geometry,
        const TransformState& transformState,
        const std::unordered_map<std::string, const RenderLayer*>& layers,
        const RenderedQueryOptions& options,
        const GlobalStateMap* globalState,
        const mat4& projMatrix) const override;

    std::vector<Feature> querySourceFeatures(const SourceQueryOptions&, const GlobalStateMap*) const override;

    // Tile-load listener API. Cross-source consumers (e.g. ContourSource)
    // register a listener and are notified each time a DEM tile settles: it
    // parsed a body with `DEMData` ready (`isRenderable()`), or it will have
    // no data, because the response had no content or the load failed. A
    // tile that keeps its old data through a failed refresh is not notified.
    // `addTileLoadListener` also replays the tiles that have settled
    // synchronously, so a late-registering consumer doesn't miss them.
    using TileLoadListener = std::function<void(const RasterDEMTile&)>;
    using ListenerHandle = ListenerSet<const RasterDEMTile&>::Handle;
    [[nodiscard]] ListenerHandle addTileLoadListener(TileLoadListener);
    void removeTileLoadListener(ListenerHandle);

    // Sync lookup: returns the DEM tile at the given coord, or nullptr if this
    // source has none. Used by cross-source consumers (contour source) to
    // settle newly-created derived tiles from a DEM tile that has already
    // settled, closing the race between consumer-pyramid updates and upstream
    // tile arrivals.
    const RasterDEMTile* getTile(const OverscaledTileID&) const;

    // Zoom range of the upstream DEM tileset, or `std::nullopt` if the
    // tileset hasn't been parsed yet. Cross-source consumers (contour
    // source) use this to mirror the DEM source's tile-pyramid coords
    // exactly — same canonical zoom for every tile, same overzoom
    // behaviour at high display zooms — so derived-tile lookup by
    // `OverscaledTileID` always finds its DEM counterpart.
    std::optional<Range<std::uint8_t>> getZoomRange() const;

    // Tile size (px) of the upstream DEM tileset. Cross-source
    // consumers (contour source) use this so their tilePyramid produces
    // the same OverscaledTileID coordinates as the DEM's pyramid;
    // different tileSize values produce different overscaledZ at the
    // same display zoom, which breaks the contour-to-DEM listener match.
    uint16_t getTileSize() const;

    // Bounds of the upstream DEM tileset: its pyramid makes no tile outside
    // them. Nothing while the tileset is not loaded, or when it has no bounds.
    std::optional<LatLngBounds> getBounds() const;

private:
    // RenderTileSetSource overrides
    void updateInternal(const Tileset&,
                        const std::vector<Immutable<style::LayerProperties>>&,
                        bool needsRendering,
                        bool needsRelayout,
                        const TileParameters&) override;
    const std::optional<Tileset>& getTileset() const override;

    const style::TileSource::Impl& impl() const;

    void onTileChanged(Tile&) override;
    void onTileError(Tile&, std::exception_ptr) override;

    ListenerSet<const RasterDEMTile&> tileLoadListeners;
};

} // namespace mln
