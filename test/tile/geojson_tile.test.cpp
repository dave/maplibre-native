#include <mln/test/util.hpp>
#include <mln/test/fake_file_source.hpp>
#include <mln/test/stub_tile_observer.hpp>
#include <mln/tile/geojson_tile.hpp>
#include <mln/tile/tile_loader_impl.hpp>

#include <mln/annotation/annotation_manager.hpp>
#include <mln/map/transform.hpp>
#include <mln/renderer/image_manager.hpp>
#include <mln/renderer/tile_parameters.hpp>
#include <mln/style/layers/circle_layer.hpp>
#include <mln/style/layers/circle_layer_impl.hpp>
#include <mln/style/sources/geojson_source.hpp>
#include <mln/style/style.hpp>
#include <mln/text/glyph_manager.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/gfx/dynamic_texture_atlas.hpp>
#include <mln/renderer/buckets/symbol_bucket.hpp>
#include <mln/renderer/tile_render_data.hpp>
#include <mln/style/image_impl.hpp>
#include <mln/style/layers/symbol_layer.hpp>
#include <mln/style/layers/symbol_layer_impl.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/text/local_glyph_rasterizer.hpp>
#include <mln/util/io.hpp>
#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/headless_backend.hpp>

#include <memory>

using namespace mln;
using namespace mln::style;

class GeoJSONTileTest {
public:
    util::SimpleIdentity uniqueID;
    std::shared_ptr<FileSource> fileSource = std::make_shared<FakeFileSource>();
    TransformState transformState;
    util::RunLoop loop;
    AnnotationManager annotationManager{style};
    std::shared_ptr<ImageManager> imageManager = ImageManager::create();
    std::shared_ptr<GlyphManager> glyphManager = std::make_shared<GlyphManager>();
    gfx::DynamicTextureAtlasPtr dynamicTextureAtlas;

    Tileset tileset{{"https://example.com"}, {0, 22}, "none"};
    TileParameters tileParameters;
    style::Style style;

    GeoJSONTileTest()
        : tileParameters{.pixelRatio = 1.0,
                         .debugOptions = MapDebugOptions(),
                         .transformState = transformState,
                         .fileSource = fileSource,
                         .mode = MapMode::Continuous,
                         .annotationManager = annotationManager.makeWeakPtr(),
                         .imageManager = imageManager,
                         .glyphManager = glyphManager,
                         .prefetchZoomDelta = 0,
                         .threadPool = {Scheduler::GetBackground(), uniqueID},
                         .dynamicTextureAtlas = dynamicTextureAtlas},
          style{fileSource, 1, tileParameters.threadPool} {}
};

namespace {

class FakeGeoJSONData : public GeoJSONData {
public:
    FakeGeoJSONData(TileFeatures features_)
        : features(std::move(features_)) {}

    void getTile(const CanonicalTileID&, const std::function<void(TileFeatures)>& fn, bool) final {
        assert(fn);
        fn(features);
    }

    Features getChildren(const std::uint32_t) final { return {}; }

    Features getLeaves(const std::uint32_t, const std::uint32_t, const std::uint32_t) final { return {}; }

    std::uint8_t getClusterExpansionZoom(std::uint32_t) final { return 0; }

private:
    TileFeatures features;
};

} // namespace

TEST(GeoJSONTile, SynchronousUpdate) {
    GeoJSONTileTest test;

    CircleLayer layer("circle", "source");

    mapbox::feature::feature_collection<int16_t> features;
    features.push_back(mapbox::feature::feature<int16_t>{mapbox::geometry::point<int16_t>(0, 0)});
    auto data = std::make_shared<FakeGeoJSONData>(std::move(features));
    TileParameters tileParameters = test.tileParameters;
    tileParameters.isUpdateSynchronous = true;
    GeoJSONTile tile(OverscaledTileID(0, 0, 0), "source", tileParameters, data);
    Immutable<LayerProperties> layerProperties = makeMutable<CircleLayerProperties>(
        staticImmutableCast<CircleLayer::Impl>(layer.baseImpl));
    std::vector<Immutable<LayerProperties>> layers{layerProperties};
    tile.setLayers(layers, tileParameters.globalState);
    ASSERT_TRUE(tile.isComplete());
    ASSERT_TRUE(tile.isRenderable());
}

TEST(GeoJSONTile, Issue7648) {
    GeoJSONTileTest test;

    CircleLayer layer("circle", "source");

    mapbox::feature::feature_collection<int16_t> features;
    features.push_back(mapbox::feature::feature<int16_t>{mapbox::geometry::point<int16_t>(0, 0)});
    auto data = std::make_shared<FakeGeoJSONData>(std::move(features));
    GeoJSONTile tile(OverscaledTileID(0, 0, 0), "source", test.tileParameters, data);
    Immutable<LayerProperties> layerProperties = makeMutable<CircleLayerProperties>(
        staticImmutableCast<CircleLayer::Impl>(layer.baseImpl));
    StubTileObserver observer;
    observer.tileChanged = [&](const Tile&) {
        // Once present, the bucket should never "disappear", which would cause
        // flickering.
        ASSERT_TRUE(tile.layerPropertiesUpdated(layerProperties));
    };

    std::vector<Immutable<LayerProperties>> layers{layerProperties};
    tile.setLayers(layers, test.tileParameters.globalState);
    tile.setObserver(&observer);

    while (!tile.isComplete()) {
        test.loop.runOnce();
    }

    tile.updateData(data, false, test.tileParameters.isUpdateSynchronous);
    while (!tile.isComplete()) {
        test.loop.runOnce();
    }
}

// Tests that tiles remain renderable if they have been renderable and then had
// an error sent to them, e.g. when revalidating/refreshing the request.
TEST(GeoJSONTile, Issue9927) {
    GeoJSONTileTest test;

    CircleLayer layer("circle", "source");

    mapbox::feature::feature_collection<int16_t> features;
    features.push_back(mapbox::feature::feature<int16_t>{mapbox::geometry::point<int16_t>(0, 0)});
    auto data = std::make_shared<FakeGeoJSONData>(std::move(features));
    GeoJSONTile tile(OverscaledTileID(0, 0, 0), "source", test.tileParameters, data);

    Immutable<LayerProperties> layerProperties = makeMutable<CircleLayerProperties>(
        staticImmutableCast<CircleLayer::Impl>(layer.baseImpl));
    std::vector<Immutable<LayerProperties>> layers{layerProperties};
    tile.setLayers(layers, test.tileParameters.globalState);

    while (!tile.isComplete()) {
        test.loop.runOnce();
    }

    ASSERT_TRUE(tile.isRenderable());
    ASSERT_TRUE(tile.layerPropertiesUpdated(layerProperties));

    // Make sure that once we've had a renderable tile and then receive
    // erroneous data, we retain the previously rendered data and keep the tile
    // renderable.
    tile.setError(std::make_exception_ptr(std::runtime_error("Connection offline")));
    ASSERT_TRUE(tile.isRenderable());
    ASSERT_TRUE(tile.layerPropertiesUpdated(layerProperties));

    // Then simulate a parsing failure and make sure that we keep it renderable
    // in this situation as well. We're using 3 as a correlationID since we've
    // done two three calls that increment this counter (as part of the
    // GeoJSONTile constructor, setLayers, and setPlacementConfig).
    tile.onError(std::make_exception_ptr(std::runtime_error("Parse error")), 3);
    ASSERT_TRUE(tile.isRenderable());
    ASSERT_TRUE(tile.layerPropertiesUpdated(layerProperties));
}

namespace {

/** Rasterizes every glyph locally, so the glyph manager has them at once: getGlyphs answers inside the call. */
class EveryGlyphLocal : public LocalGlyphRasterizer {
public:
    bool canRasterizeGlyph(const FontStack&, GlyphID) override { return true; }
    Glyph rasterizeGlyph(const FontStack&, GlyphID glyphID) override {
        Glyph glyph;
        glyph.id = glyphID;
        glyph.metrics.width = 10;
        glyph.metrics.height = 10;
        glyph.metrics.advance = 12;
        glyph.bitmap = AlphaImage(Size(16, 16));
        return glyph;
    }
};

/** A symbol layer with both an icon and a text, on one point; the icon's image is in the image manager. */
std::unique_ptr<SymbolLayer> iconAndTextLayer() {
    auto layer = std::make_unique<SymbolLayer>("symbol", "source");
    layer->setIconImage(expression::Image("pin"));
    layer->setTextField(expression::Formatted("ab"));
    layer->setTextFont({{"Test Stack"}});
    return layer;
}

struct IconAndTextTile {
    GeoJSONTileTest test;
    // A real texture atlas: the layout takes glyph and icon positions from it,
    // and builds no symbol without them.
    std::unique_ptr<gfx::HeadlessBackend> backend = gfx::HeadlessBackend::Create({256, 256});
    gfx::BackendScope scope{*backend->getRendererBackend()};
    std::shared_ptr<gfx::DynamicTextureAtlas> atlas = std::make_shared<gfx::DynamicTextureAtlas>(
        backend->getRendererBackend()->getContext());
    std::unique_ptr<SymbolLayer> layer = iconAndTextLayer();
    Immutable<LayerProperties> layerProperties = makeMutable<SymbolLayerProperties>(
        staticImmutableCast<SymbolLayer::Impl>(layer->baseImpl));
    std::shared_ptr<FakeGeoJSONData> data;

    explicit IconAndTextTile(std::shared_ptr<GlyphManager> glyphs) {
        test.tileParameters.glyphManager = std::move(glyphs);
        test.tileParameters.dynamicTextureAtlas = atlas;
        test.imageManager->addImage(makeMutable<style::Image::Impl>("pin", PremultipliedImage({16, 16}), 1.0f));
        test.imageManager->setLoaded(true);
        mapbox::feature::feature_collection<int16_t> features;
        features.push_back(mapbox::feature::feature<int16_t>{mapbox::geometry::point<int16_t>(2048, 2048)});
        data = std::make_shared<FakeGeoJSONData>(std::move(features));
    }

    // The atlas keeps its glyph and icon textures for the renderer to free;
    // with no renderer here, the context would assert on them at its end. It
    // is let go instead: a test process ends soon after.
    ~IconAndTextTile() {
        (void)new std::shared_ptr<gfx::DynamicTextureAtlas>(atlas);
        (void)backend.release();
    }

    /** The layer's symbol bucket, or null for none. */
    const SymbolBucket* bucket(GeoJSONTile& tile) {
        auto renderData = tile.createRenderData();
        return static_cast<const SymbolBucket*>(renderData->getBucket(*layer->baseImpl));
    }

    /** Builds the tile with [synchronous] updates and runs the loop until it is complete. */
    std::unique_ptr<GeoJSONTile> build(bool synchronous, std::shared_ptr<FileSource> fileSource = nullptr) {
        TileParameters parameters = test.tileParameters;
        parameters.isUpdateSynchronous = synchronous;
        if (fileSource) parameters.fileSource = std::move(fileSource);
        auto tile = std::make_unique<GeoJSONTile>(OverscaledTileID(0, 0, 0), "source", parameters, data);
        tile->setLayers({layerProperties}, parameters.globalState);
        while (!tile->isComplete()) {
            test.loop.runOnce();
        }
        return tile;
    }
};

std::shared_ptr<StubFileSource> glyphFileSource() {
    auto fileSource = std::make_shared<StubFileSource>();
    fileSource->glyphsResponse = [](const Resource&) {
        Response response;
        response.data = std::make_shared<std::string>(util::read_file("test/fixtures/resources/glyphs.pbf"));
        return response;
    };
    return fileSource;
}

std::shared_ptr<GlyphManager> fetchedGlyphs() {
    auto glyphs = std::make_shared<GlyphManager>();
    glyphs->setURL("test/fixtures/resources/glyphs.pbf");
    return glyphs;
}

} // namespace

// The control: an asynchronous source lays out the icon and the text.
TEST(GeoJSONTile, AnAsynchronousUpdateLaysOutTheIconAndTheText) {
    IconAndTextTile t(std::make_shared<GlyphManager>(std::make_unique<EveryGlyphLocal>()));
    auto tile = t.build(false);
    const auto* bucket = t.bucket(*tile);
    ASSERT_NE(nullptr, bucket) << "no symbol bucket";
    EXPECT_TRUE(bucket->hasTextData());
    EXPECT_TRUE(bucket->hasIconData());
}

// A synchronous source (isUpdateSynchronous) with the glyphs already at hand:
// the glyph answer comes inside parse(), before the image request has gone
// out. The layout must still wait for the images and hold the icon, not lay
// out text only (Viatrail, 2026-10-13: cards drawn as bare text on Android).
TEST(GeoJSONTile, SynchronousUpdateKeepsTheIconWhenTheGlyphsAreAtHand) {
    IconAndTextTile t(std::make_shared<GlyphManager>(std::make_unique<EveryGlyphLocal>()));
    auto tile = t.build(true);
    const auto* bucket = t.bucket(*tile);
    ASSERT_NE(nullptr, bucket) << "no symbol bucket";
    EXPECT_TRUE(bucket->hasTextData());
    EXPECT_TRUE(bucket->hasIconData()) << "the layout dropped the icon";
}

// The same with glyphs fetched from the file source (not at hand), answered
// after parse() returns.
TEST(GeoJSONTile, SynchronousUpdateKeepsTheIconWhenTheGlyphsAreFetched) {
    IconAndTextTile t(fetchedGlyphs());
    auto tile = t.build(true, glyphFileSource());
    const auto* bucket = t.bucket(*tile);
    ASSERT_NE(nullptr, bucket) << "no symbol bucket";
    EXPECT_TRUE(bucket->hasTextData());
    EXPECT_TRUE(bucket->hasIconData()) << "the layout dropped the icon";
}

