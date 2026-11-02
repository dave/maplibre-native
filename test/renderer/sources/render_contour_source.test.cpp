#include <mln/test/fake_file_source.hpp>
#include <mln/test/util.hpp>

#include <mln/actor/scheduler.hpp>
#include <mln/annotation/annotation_manager.hpp>
#include <mln/gfx/dynamic_texture_atlas.hpp>
#include <mln/map/transform.hpp>
#include <mln/renderer/image_manager.hpp>
#include <mln/renderer/query.hpp>
#include <mln/renderer/sources/render_contour_source.hpp>
#include <mln/renderer/sources/render_raster_dem_source.hpp>
#include <mln/renderer/tile_parameters.hpp>
#include <mln/storage/file_source.hpp>
#include <mln/style/sources/contour_source.hpp>
#include <mln/style/sources/contour_source_impl.hpp>
#include <mln/style/sources/tile_source_impl.hpp>
#include <mln/style/style.hpp>
#include <mln/text/glyph_manager.hpp>
#include <mln/util/image.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/tileset.hpp>
#include <mln/util/timer.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <new>
#include <optional>
#include <string>
#include <vector>

using namespace mln;

// Fixture for render-source-level tests of RenderContourSource. Builds the
// minimum scaffolding needed to drive update() without an orchestrator: a
// scheduler, file source, Style (for the AnnotationManager), and a
// TileParameters builder that takes a custom getRenderSource lambda.
//
// The cross-source lookup added for this feature (TileParameters::getRenderSource)
// makes RenderContourSource depend on a sibling RenderRasterDEMSource by ID
// at every update().
class RenderContourSourceTest {
public:
    util::SimpleIdentity uniqueID;
    util::RunLoop loop;
    std::shared_ptr<FileSource> fileSource = std::make_shared<FakeFileSource>();
    Transform transform;
    TransformState transformState;
    std::shared_ptr<ImageManager> imageManager = ImageManager::create();
    std::shared_ptr<GlyphManager> glyphManager = std::make_shared<GlyphManager>();
    gfx::DynamicTextureAtlasPtr dynamicTextureAtlas;
    TaggedScheduler threadPool;
    style::Style style;
    AnnotationManager annotationManager{style};

    Tileset tileset{{"https://example.com/{z}/{x}/{y}.png"}, {0, 12}};
    uint8_t prefetchZoomDelta = 0;

    RenderContourSourceTest()
        : threadPool(Scheduler::GetBackground(), uniqueID),
          style{fileSource, 1, threadPool} {
        transform.resize({512, 512});
        transform.jumpTo(CameraOptions().withCenter(LatLng()).withZoom(0.0));
        transformState = transform.getState();
    }

    ~RenderContourSourceTest() { threadPool.waitForEmpty(); }

    TileParameters makeParameters(std::function<RenderSource*(const std::string&)> getRenderSource = {}) {
        return {.pixelRatio = 1.0,
                .debugOptions = MapDebugOptions(),
                .transformState = transformState,
                .fileSource = fileSource,
                .mode = MapMode::Continuous,
                .annotationManager = annotationManager.makeWeakPtr(),
                .imageManager = imageManager,
                .glyphManager = glyphManager,
                .prefetchZoomDelta = prefetchZoomDelta,
                .threadPool = threadPool,
                .dynamicTextureAtlas = dynamicTextureAtlas,
                .getRenderSource = std::move(getRenderSource)};
    }

    Immutable<style::TileSource::Impl> makeRasterDEMImpl(const std::string& id) {
        // TileSource::Impl(SourceType, id, tileSize) constructs without a
        // tileset; the second-form constructor stamps in the tileset.
        return makeMutable<style::TileSource::Impl>(
            *makeMutable<style::TileSource::Impl>(style::SourceType::RasterDEM, id, util::tileSize_I), tileset);
    }

    Immutable<style::ContourSource::Impl> makeContourImpl(const std::string& id,
                                                          const std::string& upstreamID,
                                                          std::vector<double> intervals = {100.0}) {
        style::ContourSourceOptions options;
        options.sourceID = upstreamID;
        options.intervals.stops = std::move(intervals);
        return makeMutable<style::ContourSource::Impl>(id, std::move(options));
    }
};

namespace {

// A raster-dem source and a contour source that reads it, updated as
// RenderOrchestrator updates them: the DEM source first, both every frame.
class ContourStack {
public:
    RenderContourSourceTest test;
    Immutable<style::TileSource::Impl> demImpl;
    RenderRasterDEMSource dem;
    Immutable<style::ContourSource::Impl> contourImpl;
    RenderContourSource contour;

    explicit ContourStack(std::optional<LatLngBounds> bounds = std::nullopt,
                          std::optional<uint8_t> demPrefetchZoomDelta = std::nullopt)
        : demImpl(makeDEMImpl(test, bounds, demPrefetchZoomDelta)),
          dem(demImpl, test.threadPool),
          contourImpl(test.makeContourImpl("contours", "dem")),
          contour(contourImpl, test.threadPool) {}

    void update() {
        auto params = test.makeParameters(
            [&](const std::string& id) -> RenderSource* { return id == "dem" ? &dem : nullptr; });
        static_cast<RenderSource&>(dem).update(demImpl, {}, /*needsRendering=*/true, /*needsRelayout=*/false, params);
        contour.update(contourImpl, {}, /*needsRendering=*/true, /*needsRelayout=*/false, params);
    }

    // Answers every open DEM tile request.
    void respond(const Response& response) {
        auto& fileSource = static_cast<FakeFileSource&>(*test.fileSource);
        while (fileSource.respond(Resource::Tile, response)) {
        }
    }

    // Runs the loop until the contour source has no tile left to finish. The
    // timer only ends a run that would otherwise never return.
    bool contourFinishes() {
        util::Timer watchdog;
        bool timedOut = false;
        watchdog.start(std::chrono::seconds(30), Duration::zero(), [&] { timedOut = true; });
        while (!contour.isLoaded() && !timedOut) {
            test.loop.runOnce();
            update();
        }
        return contour.isLoaded();
    }

private:
    static Immutable<style::TileSource::Impl> makeDEMImpl(RenderContourSourceTest& test,
                                                          std::optional<LatLngBounds> bounds,
                                                          std::optional<uint8_t> prefetchZoomDelta) {
        Tileset tileset = test.tileset;
        tileset.bounds = bounds;
        auto impl = makeMutable<style::TileSource::Impl>(
            *makeMutable<style::TileSource::Impl>(style::SourceType::RasterDEM, "dem", util::tileSize_I), tileset);
        impl->setPrefetchZoomDelta(prefetchZoomDelta);
        return impl;
    }
};

std::shared_ptr<const std::string> flatDEMTile() {
    PremultipliedImage image({16, 16});
    std::fill_n(image.data.get(), image.bytes(), static_cast<uint8_t>(255));
    return std::make_shared<const std::string>(encodePNG(image));
}

} // namespace

TEST(RenderContourSource, ANewSourceHasNoTilesToWaitFor) {
    RenderContourSourceTest test;
    auto impl = test.makeContourImpl("contours", "dem");
    RenderContourSource source(impl, test.threadPool);
    EXPECT_TRUE(source.isLoaded());
}

// Tile parameters made outside the orchestrator can have no lookup at all. That
// is the same as a DEM source that is not there.
TEST(RenderContourSource, AnEmptyLookupMeansNoDEMSource) {
    RenderContourSourceTest test;
    auto impl = test.makeContourImpl("contours", "dem");
    RenderContourSource source(impl, test.threadPool);

    auto params = test.makeParameters({});
    source.update(impl, {}, /*needsRendering=*/true, /*needsRelayout=*/false, params);

    EXPECT_TRUE(source.isLoaded());
}

// RenderOrchestrator destroys sources via std::unordered_map::erase, which gives
// no guarantee about the order between siblings. If the DEM source goes first,
// the contour source must not touch it, in an update or in its destructor, and
// its tiles go with the DEM source.
TEST(RenderContourSource, TheDEMSourceCanBeDestroyedFirst) {
    RenderContourSourceTest test;
    auto demImpl = test.makeRasterDEMImpl("dem");
    auto upstream = std::make_unique<RenderRasterDEMSource>(demImpl, test.threadPool);
    auto contourImpl = test.makeContourImpl("contours", "dem");
    auto contour = std::make_unique<RenderContourSource>(contourImpl, test.threadPool);

    auto params = test.makeParameters(
        [&](const std::string& id) -> RenderSource* { return id == "dem" ? upstream.get() : nullptr; });
    static_cast<RenderSource&>(*upstream).update(demImpl, {}, /*needsRendering=*/true, /*needsRelayout=*/false, params);
    contour->update(contourImpl, {}, /*needsRendering=*/true, /*needsRelayout=*/false, params);
    ASSERT_FALSE(contour->isLoaded());

    upstream.reset();
    contour->update(contourImpl, {}, /*needsRendering=*/true, /*needsRelayout=*/false, params);
    EXPECT_TRUE(contour->isLoaded());

    contour.reset();
}

// If the contour source goes first, the DEM source goes on loading tiles with no
// listener left to call.
TEST(RenderContourSource, TheContourSourceCanBeDestroyedFirst) {
    RenderContourSourceTest test;
    auto demImpl = test.makeRasterDEMImpl("dem");
    RenderRasterDEMSource upstream(demImpl, test.threadPool);
    auto contourImpl = test.makeContourImpl("contours", "dem");
    auto contour = std::make_unique<RenderContourSource>(contourImpl, test.threadPool);

    auto params = test.makeParameters(
        [&](const std::string& id) -> RenderSource* { return id == "dem" ? &upstream : nullptr; });
    static_cast<RenderSource&>(upstream).update(demImpl, {}, /*needsRendering=*/true, /*needsRelayout=*/false, params);
    contour->update(contourImpl, {}, /*needsRendering=*/true, /*needsRelayout=*/false, params);
    contour.reset();

    Response response;
    response.data = flatDEMTile();
    auto& fileSource = static_cast<FakeFileSource&>(*test.fileSource);
    ASSERT_TRUE(fileSource.respond(Resource::Tile, response));
    while (fileSource.respond(Resource::Tile, response)) {
    }
    util::Timer watchdog;
    bool timedOut = false;
    watchdog.start(std::chrono::seconds(30), Duration::zero(), [&] { timedOut = true; });
    while (!upstream.isLoaded() && !timedOut) {
        test.loop.runOnce();
    }
    EXPECT_TRUE(upstream.isLoaded());
}

// A second DEM source under the same ID replaces the first: the contour source
// listens to it, and its tiles settle from it.
TEST(RenderContourSource, ContourSourceFollowsASecondDEMSource) {
    RenderContourSourceTest test;
    auto demImpl = test.makeRasterDEMImpl("dem");
    auto first = std::make_unique<RenderRasterDEMSource>(demImpl, test.threadPool);
    auto second = std::make_unique<RenderRasterDEMSource>(demImpl, test.threadPool);
    auto contourImpl = test.makeContourImpl("contours", "dem");
    RenderContourSource contour(contourImpl, test.threadPool);

    RenderRasterDEMSource* current = first.get();
    auto params = test.makeParameters(
        [&](const std::string& id) -> RenderSource* { return id == "dem" ? current : nullptr; });
    auto update = [&] {
        static_cast<RenderSource&>(*current).update(
            demImpl, {}, /*needsRendering=*/true, /*needsRelayout=*/false, params);
        contour.update(contourImpl, {}, /*needsRendering=*/true, /*needsRelayout=*/false, params);
    };
    update();

    first.reset();
    current = second.get();
    update();
    ASSERT_FALSE(contour.isLoaded());

    Response response;
    response.data = flatDEMTile();
    auto& fileSource = static_cast<FakeFileSource&>(*test.fileSource);
    while (fileSource.respond(Resource::Tile, response)) {
    }
    util::Timer watchdog;
    bool timedOut = false;
    watchdog.start(std::chrono::seconds(30), Duration::zero(), [&] { timedOut = true; });
    while (!contour.isLoaded() && !timedOut) {
        test.loop.runOnce();
    }
    EXPECT_TRUE(contour.isLoaded());
}

// With the DEM source gone from the style, the contour source has nothing to
// wait for, and holds no tile from the old source.
TEST(RenderContourSource, TheDEMSourceCanLeaveTheStyle) {
    RenderContourSourceTest test;
    auto demImpl = test.makeRasterDEMImpl("dem");
    auto upstream = std::make_unique<RenderRasterDEMSource>(demImpl, test.threadPool);
    auto contourImpl = test.makeContourImpl("contours", "dem");
    RenderContourSource contour(contourImpl, test.threadPool);

    auto params = test.makeParameters(
        [&](const std::string& id) -> RenderSource* { return id == "dem" ? upstream.get() : nullptr; });
    static_cast<RenderSource&>(*upstream).update(demImpl, {}, /*needsRendering=*/true, /*needsRelayout=*/false, params);
    contour.update(contourImpl, {}, /*needsRendering=*/true, /*needsRelayout=*/false, params);
    ASSERT_FALSE(contour.isLoaded());

    upstream.reset();
    contour.update(contourImpl, {}, /*needsRendering=*/true, /*needsRelayout=*/false, params);

    EXPECT_TRUE(contour.isLoaded());
}

TEST(RenderContourSource, ContourTileCompletesWhenItsDEMTileLoads) {
    ContourStack stack;
    stack.update();
    ASSERT_FALSE(stack.contour.isLoaded());

    Response response;
    response.data = flatDEMTile();
    stack.respond(response);

    EXPECT_TRUE(stack.contourFinishes());
}

// A DEM tile with no content is the normal case over the ocean. Before, the
// contour tile never heard of it and held the map's load open for ever.
TEST(RenderContourSource, ContourTileCompletesWhenItsDEMTileHasNoContent) {
    ContourStack stack;
    stack.update();
    ASSERT_FALSE(stack.contour.isLoaded());

    Response response;
    response.noContent = true;
    stack.respond(response);

    EXPECT_TRUE(stack.contourFinishes());
}

TEST(RenderContourSource, ContourTileCompletesWhenItsDEMTileFails) {
    ContourStack stack;
    stack.update();
    ASSERT_FALSE(stack.contour.isLoaded());

    Response response;
    response.error = std::make_unique<Response::Error>(Response::Error::Reason::Server, "no DEM today");
    stack.respond(response);

    EXPECT_TRUE(stack.contourFinishes());
}

// The DEM source asks for no tile outside its bounds, so the contour source
// must not make one: nothing would ever arrive for it.
TEST(RenderContourSource, ThereIsNoContourTileOutsideTheDEMBounds) {
    const auto elsewhere = LatLngBounds::hull(LatLng{60.0, 10.0}, LatLng{61.0, 11.0});
    ContourStack stack(elsewhere);
    stack.test.transform.jumpTo(CameraOptions().withCenter(LatLng{-30.0, -60.0}).withZoom(6.0));
    stack.test.transformState = stack.test.transform.getState();

    stack.update();

    EXPECT_TRUE(stack.dem.isLoaded());
    EXPECT_TRUE(stack.contour.isLoaded());
    EXPECT_TRUE(static_cast<FakeFileSource&>(*stack.test.fileSource).requests.empty());
}

// A contour tile made after its DEM tile settled has nothing to wait for.
TEST(RenderContourSource, ContourTileMadeAfterItsDEMTileHadNoContentCompletes) {
    ContourStack stack;
    auto params = stack.test.makeParameters(
        [&](const std::string& id) -> RenderSource* { return id == "dem" ? &stack.dem : nullptr; });
    static_cast<RenderSource&>(stack.dem).update(
        stack.demImpl, {}, /*needsRendering=*/true, /*needsRelayout=*/false, params);

    Response response;
    response.noContent = true;
    stack.respond(response);

    util::Timer watchdog;
    bool timedOut = false;
    watchdog.start(std::chrono::seconds(30), Duration::zero(), [&] { timedOut = true; });
    while (!stack.dem.isLoaded() && !timedOut) {
        stack.test.loop.runOnce();
    }
    ASSERT_TRUE(stack.dem.isLoaded());

    stack.update();
    EXPECT_TRUE(stack.contourFinishes());
}

// With no DEM source there is nothing to wait for: no tile, not a tile for a
// guessed zoom range that nothing will ever fill.
TEST(RenderContourSource, ThereIsNoContourTileWithoutTheDEMSource) {
    RenderContourSourceTest test;
    auto impl = test.makeContourImpl("contours", "dem");
    RenderContourSource source(impl, test.threadPool);

    auto params = test.makeParameters([](const std::string&) -> RenderSource* { return nullptr; });
    source.update(impl, {}, /*needsRendering=*/true, /*needsRelayout=*/false, params);

    EXPECT_TRUE(source.isLoaded());
}

// While the DEM's tileset loads, its zoom range is not known.
TEST(RenderContourSource, ThereIsNoContourTileBeforeTheDEMTilesetLoads) {
    RenderContourSourceTest test;
    Immutable<style::TileSource::Impl> demImpl = makeMutable<style::TileSource::Impl>(
        style::SourceType::RasterDEM, "dem", util::tileSize_I);
    RenderRasterDEMSource dem(demImpl, test.threadPool);
    auto impl = test.makeContourImpl("contours", "dem");
    RenderContourSource source(impl, test.threadPool);

    auto params = test.makeParameters(
        [&](const std::string& id) -> RenderSource* { return id == "dem" ? &dem : nullptr; });
    source.update(impl, {}, /*needsRendering=*/true, /*needsRelayout=*/false, params);

    EXPECT_TRUE(source.isLoaded());
}

// A DEM source that prefetches no lower zoom has no tile for a contour tile to
// wait for at that zoom, whatever the map's own prefetch setting says.
TEST(RenderContourSource, ContourSourcePrefetchesWhatTheDEMSourcePrefetches) {
    ContourStack stack(std::nullopt, /*demPrefetchZoomDelta=*/0);
    stack.test.prefetchZoomDelta = 4;
    stack.test.transform.jumpTo(CameraOptions().withCenter(LatLng{10.0, 10.0}).withZoom(6.0));
    stack.test.transformState = stack.test.transform.getState();
    stack.update();
    ASSERT_FALSE(stack.contour.isLoaded());

    Response response;
    response.noContent = true;
    stack.respond(response);

    EXPECT_TRUE(stack.contourFinishes());
}

// RenderOrchestrator can destroy a DEM source and make a new one in one update
// (one style change replaces it), and the heap often gives the new one the old
// address. The new source must get a listener as any other new source does, or
// the contour source never hears of its tiles. The new source is built on the
// old one's storage, so the address is the same every time.
TEST(RenderContourSource, ANewDEMSourceAtTheOldAddressIsListenedTo) {
    RenderContourSourceTest test;
    auto demImpl = test.makeRasterDEMImpl("dem");
    auto contourImpl = test.makeContourImpl("contours", "dem");
    RenderContourSource contour(contourImpl, test.threadPool);

    alignas(RenderRasterDEMSource) std::byte storage[sizeof(RenderRasterDEMSource)];
    auto* dem = new (storage) RenderRasterDEMSource(demImpl, test.threadPool);
    auto params = test.makeParameters(
        [&](const std::string& id) -> RenderSource* { return id == "dem" ? dem : nullptr; });
    auto update = [&] {
        static_cast<RenderSource*>(dem)->update(demImpl, {}, /*needsRendering=*/true, /*needsRelayout=*/false, params);
        contour.update(contourImpl, {}, /*needsRendering=*/true, /*needsRelayout=*/false, params);
    };
    update();
    ASSERT_FALSE(contour.isLoaded());

    dem->~RenderRasterDEMSource();
    auto* replacement = new (storage) RenderRasterDEMSource(demImpl, test.threadPool);
    ASSERT_EQ(dem, replacement);
    update();
    ASSERT_FALSE(contour.isLoaded());

    Response response;
    response.data = flatDEMTile();
    auto& fileSource = static_cast<FakeFileSource&>(*test.fileSource);
    while (fileSource.respond(Resource::Tile, response)) {
    }

    util::Timer watchdog;
    bool timedOut = false;
    watchdog.start(std::chrono::seconds(30), Duration::zero(), [&] { timedOut = true; });
    while (!contour.isLoaded() && !timedOut) {
        test.loop.runOnce();
    }
    EXPECT_TRUE(contour.isLoaded());

    dem->~RenderRasterDEMSource();
}

namespace {

// A terrarium DEM tile holding a 1000 m cone, so that it has lines at any
// sensible interval.
std::shared_ptr<const std::string> coneDEMTile() {
    constexpr int dim = 64;
    PremultipliedImage image({dim, dim});
    for (int y = 0; y < dim; y++) {
        for (int x = 0; x < dim; x++) {
            const double value = 1000.0 - (30.0 * std::hypot(x - (dim / 2.0), y - (dim / 2.0))) + 32768.0;
            uint8_t* pixel = image.data.get() + (static_cast<std::size_t>(y * dim + x) * 4);
            pixel[0] = static_cast<uint8_t>(std::floor(value / 256.0));
            pixel[1] = static_cast<uint8_t>(static_cast<int>(std::floor(value)) % 256);
            pixel[2] = static_cast<uint8_t>((value - std::floor(value)) * 256.0);
            pixel[3] = 255;
        }
    }
    return std::make_shared<const std::string>(encodePNG(image));
}

// The contour features of a source at one zoom, over a DEM of that tile size
// and maxzoom 12. (The contour tiles' features come back as the single layer
// of their tile data.)
std::vector<Feature> contourFeatures(double zoom, uint16_t demTileSize, std::vector<double> intervals) {
    RenderContourSourceTest test;
    test.tileset.rasterEncoding = Tileset::RasterEncoding::Terrarium;
    test.transform.jumpTo(CameraOptions().withCenter(LatLng{10.0, 10.0}).withZoom(zoom));
    test.transformState = test.transform.getState();
    Immutable<style::TileSource::Impl> demImpl = makeMutable<style::TileSource::Impl>(
        *makeMutable<style::TileSource::Impl>(style::SourceType::RasterDEM, "dem", demTileSize), test.tileset);
    RenderRasterDEMSource dem(demImpl, test.threadPool);
    auto contourImpl = test.makeContourImpl("contours", "dem", std::move(intervals));
    RenderContourSource contour(contourImpl, test.threadPool);

    auto params = test.makeParameters(
        [&](const std::string& id) -> RenderSource* { return id == "dem" ? &dem : nullptr; });
    auto update = [&] {
        static_cast<RenderSource&>(dem).update(demImpl, {}, /*needsRendering=*/true, /*needsRelayout=*/false, params);
        contour.update(contourImpl, {}, /*needsRendering=*/true, /*needsRelayout=*/false, params);
    };
    update();
    Response response;
    response.data = coneDEMTile();
    auto& fileSource = static_cast<FakeFileSource&>(*test.fileSource);
    while (fileSource.respond(Resource::Tile, response)) {
    }

    util::Timer watchdog;
    bool timedOut = false;
    watchdog.start(std::chrono::seconds(30), Duration::zero(), [&] { timedOut = true; });
    while (!contour.isLoaded() && !timedOut) {
        test.loop.runOnce();
        update();
    }
    EXPECT_TRUE(contour.isLoaded());
    update();

    SourceQueryOptions options;
    options.sourceLayers = std::vector<std::string>{""};
    return static_cast<RenderSource&>(contour).querySourceFeatures(options, nullptr);
}

double intervalOf(const std::vector<Feature>& features) {
    EXPECT_FALSE(features.empty());
    return features.empty() ? 0.0 : numericValue<double>(features.front().properties.at("interval")).value();
}

} // namespace

// The schedule is read at the zoom of the map, not of the DEM tile. The DEM
// stops at maxzoom 12 and, with 256 px tiles, its tile zoom is the map zoom + 1.
TEST(RenderContourSource, TheIntervalFollowsTheMapZoomAboveTheDEMMaxzoom) {
    const std::vector<double> schedule{200.0, 12.0, 100.0, 13.0, 50.0};
    EXPECT_DOUBLE_EQ(50.0, intervalOf(contourFeatures(13.0, 512, schedule)));
    EXPECT_DOUBLE_EQ(50.0, intervalOf(contourFeatures(13.0, 256, schedule)));
}

TEST(RenderContourSource, TheIntervalFollowsTheMapZoomWithSmallDEMTiles) {
    const std::vector<double> schedule{200.0, 12.0, 100.0, 13.0, 50.0};
    // Map zoom 11, DEM tile zoom 12: below the first stop.
    EXPECT_DOUBLE_EQ(200.0, intervalOf(contourFeatures(11.0, 256, schedule)));
    EXPECT_DOUBLE_EQ(100.0, intervalOf(contourFeatures(12.0, 512, schedule)));
}
