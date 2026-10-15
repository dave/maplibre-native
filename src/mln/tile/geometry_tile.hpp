#pragma once

#include <mln/actor/optional_actor.hpp>
#include <mln/geometry/feature_index.hpp>
#include <mln/gfx/dynamic_texture_atlas.hpp>
#include <mln/renderer/image_manager.hpp>
#include <mln/text/glyph_manager.hpp>
#include <mln/tile/tile.hpp>
#include <mln/tile/geometry_tile_worker.hpp>
#include <mln/util/feature.hpp>
#include <mln/util/containers.hpp>

#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

namespace mln {

class GeometryTileData;
class RenderLayer;
class SourceQueryOptions;
class TileParameters;
class TileAtlasTextures;

class GeometryTile : public Tile, public GlyphRequestor, public ImageRequestor {
public:
    const std::thread::id renderThreadID = std::this_thread::get_id();

    GeometryTile(const OverscaledTileID&,
                 std::string sourceID,
                 const TileParameters&,
                 TileObserver* observer = nullptr);

    ~GeometryTile() override;

    void setError(std::exception_ptr);
    void setData(std::unique_ptr<const GeometryTileData>);
    // Resets the tile's data and layers and leaves the tile in pending state,
    // waiting for the new data and layers to come.
    void reset();

    std::unique_ptr<TileRenderData> createRenderData() override;
    void setLayers(const std::vector<Immutable<style::LayerProperties>>&,
                   std::shared_ptr<const GlobalStateMap>) override;
    /// For a label lane tile: sends the layers to the lane's worker, and lays
    /// the tile's newest data out with them on this thread at once, in a
    /// synchronous worker of its own, so the render thread never waits for a
    /// body on the lane. The lane's older results are dropped when they come.
    void setLayersInline(const std::vector<Immutable<style::LayerProperties>>&,
                         std::shared_ptr<const GlobalStateMap>) override;
    void setShowCollisionBoxes(bool showCollisionBoxes) override;

    void onGlyphsAvailable(GlyphMap, HBShapeRequests) override;
    void onImagesAvailable(ImageMap, ImageMap, ImageVersionMap versionMap, uint64_t imageCorrelationID) override;

    void getGlyphs(GlyphDependencies);
    void getImages(ImageRequestPair);

    bool layerPropertiesUpdated(const Immutable<style::LayerProperties>&) override;

    void queryRenderedFeatures(std::unordered_map<std::string, std::vector<Feature>>& result,
                               const GeometryCoordinates& queryGeometry,
                               const TransformState&,
                               const std::unordered_map<std::string, const RenderLayer*>& layers,
                               const RenderedQueryOptions& options,
                               const GlobalStateMap* globalState,
                               const mat4& projMatrix,
                               const SourceFeatureState& featureState) override;

    void querySourceFeatures(std::vector<Feature>& result,
                             const SourceQueryOptions&,
                             const GlobalStateMap* globalState) override;

    float getQueryPadding(const std::unordered_map<std::string, const RenderLayer*>&) override;

    void cancel() override;

    class LayoutResult {
    public:
        mln::unordered_map<std::string, LayerRenderData> layerRenderData;
        std::shared_ptr<FeatureIndex> featureIndex;
        gfx::GlyphAtlas glyphAtlas;
        gfx::ImageAtlas imageAtlas;
        gfx::DynamicTextureAtlasPtr dynamicTextureAtlas;

        LayerRenderData* getLayerRenderData(const style::Layer::Impl&);

        LayoutResult(mln::unordered_map<std::string, LayerRenderData>&& renderData_,
                     std::shared_ptr<FeatureIndex>&& featureIndex_,
                     gfx::GlyphAtlas&& glyphAtlas_,
                     gfx::ImageAtlas&& imageAtlas_,
                     gfx::DynamicTextureAtlasPtr dynamicTextureAtlas_)
            : layerRenderData(std::move(renderData_)),
              featureIndex(std::move(featureIndex_)),
              glyphAtlas(std::move(glyphAtlas_)),
              imageAtlas(std::move(imageAtlas_)),
              dynamicTextureAtlas(dynamicTextureAtlas_) {}

        ~LayoutResult();
    };
    void onLayout(std::shared_ptr<LayoutResult>&&, uint64_t correlationID);

    void onError(std::exception_ptr, uint64_t correlationID);

    void setObserver(TileObserver*) override;

    bool holdForFade() const override;
    void markRenderedIdeal() override;
    void markRenderedPreviously() override;
    void performedFadePlacement() override;
    std::shared_ptr<FeatureIndex> getFeatureIndex() const;

    void setFeatureState(const LayerFeatureStates&) override;

protected:
    const GeometryTileData* getData() const;
    LayerRenderData* getLayerRenderData(const style::Layer::Impl&);

    // Used to signal the worker that it should abandon parsing this tile as soon as possible.
    std::atomic<bool> obsolete{false};

private:
    void markObsolete();
    std::vector<Immutable<style::LayerProperties>> layersForZoom(
        const std::vector<Immutable<style::LayerProperties>>&) const;
    HBShapeResults shape(GlyphMap&, const HBShapeRequests&);

    // Takes the glyph and image answers for the inline worker, apart from the
    // lane worker's: a requestor has one image request at a time.
    class InlineRequestor final : public GlyphRequestor, public ImageRequestor {
    public:
        InlineRequestor(GeometryTile& tile_, std::shared_ptr<ImageManager> imageManager_)
            : ImageRequestor(std::move(imageManager_)),
              tile(tile_) {}
        void onGlyphsAvailable(GlyphMap, HBShapeRequests) override;
        void onImagesAvailable(ImageMap, ImageMap, ImageVersionMap, uint64_t imageCorrelationID) override;

    private:
        GeometryTile& tile;
    };

    TaggedScheduler threadPool;

    const std::shared_ptr<Mailbox> mailbox;
    OptionalActor<GeometryTileWorker> worker;

    // A label lane tile's inline layouts (`setLayersInline`): the data it
    // last sent to the worker, the worker's other arguments, and the
    // synchronous worker of the last inline layout.
    const bool laysOutInline;
    std::unique_ptr<const GeometryTileData> lastData;
    const float pixelRatio;
    const gfx::DynamicTextureAtlasPtr dynamicTextureAtlas;
    const std::shared_ptr<FontFaces> fontFaces;
    const bool captureRenderedFeatures;
    std::unique_ptr<InlineRequestor> inlineRequestor;
    std::unique_ptr<OptionalActor<GeometryTileWorker>> inlineWorker;
    // Set while the inline worker runs: its glyph and image requests go
    // through `inlineRequestor`.
    bool inInlineWorker = false;
    // A result older than the last one taken is dropped: with an inline
    // layout, the lane's result for older layers can come after it.
    uint64_t takenCorrelationID = 0;

    const std::shared_ptr<FileSource> fileSource;
    const std::shared_ptr<GlyphManager> glyphManager;
    const std::shared_ptr<ImageManager> imageManager;

    uint64_t correlationID = 0;

    std::shared_ptr<LayoutResult> layoutResult;
    std::shared_ptr<TileAtlasTextures> atlasTextures;

    const MapMode mode;

    bool showCollisionBoxes;

    enum class FadeState {
        Loaded,
        NeedsFirstPlacement,
        NeedsSecondPlacement,
        CanRemove
    };

    FadeState fadeState = FadeState::Loaded;
};

} // namespace mln
