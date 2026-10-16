#include <mln/test/fake_file_source.hpp>

#include <mln/actor/scheduler.hpp>
#include <mln/annotation/annotation_manager.hpp>
#include <mln/geometry/dem_data.hpp>
#include <mln/gfx/dynamic_texture_atlas.hpp>
#include <mln/map/transform.hpp>
#include <mln/renderer/buckets/hillshade_bucket.hpp>
#include <mln/renderer/image_manager.hpp>
#include <mln/renderer/tile_parameters.hpp>
#include <mln/style/style.hpp>
#include <mln/text/glyph_manager.hpp>
#include <mln/tile/contour_tile.hpp>
#include <mln/tile/raster_dem_tile.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/tileset.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

using namespace mln;

namespace {

// A scheduler that only queues tasks, so a test decides when, and in which
// order, they run.
class ManualScheduler final : public Scheduler {
public:
    void schedule(std::function<void()>&& fn) override { tasks.push_back(std::move(fn)); }
    void schedule(const util::SimpleIdentity, std::function<void()>&& fn) override { schedule(std::move(fn)); }
    mapbox::base::WeakPtr<Scheduler> makeWeakPtr() override { return weakFactory.makeWeakPtr(); }
    void waitForEmpty(const util::SimpleIdentity) override {}

    std::vector<std::function<void()>> tasks;

private:
    mapbox::base::WeakPtrFactory<Scheduler> weakFactory{this};
};

class ContourTileTest {
public:
    util::SimpleIdentity uniqueID;
    util::RunLoop loop;
    std::shared_ptr<FileSource> fileSource = std::make_shared<FakeFileSource>();
    TransformState transformState;
    std::shared_ptr<ImageManager> imageManager = ImageManager::create();
    std::shared_ptr<GlyphManager> glyphManager = std::make_shared<GlyphManager>();
    gfx::DynamicTextureAtlasPtr dynamicTextureAtlas;
    std::shared_ptr<ManualScheduler> scheduler = std::make_shared<ManualScheduler>();
    TaggedScheduler threadPool{scheduler, uniqueID};
    style::Style style{fileSource, 1, threadPool};
    AnnotationManager annotationManager{style};
    Tileset tileset{{"https://example.com"}, {0, 22}, "none"};

    TileParameters parameters() {
        return {.pixelRatio = 1.0,
                .debugOptions = MapDebugOptions(),
                .transformState = transformState,
                .fileSource = fileSource,
                .mode = MapMode::Continuous,
                .annotationManager = annotationManager.makeWeakPtr(),
                .imageManager = imageManager,
                .glyphManager = glyphManager,
                .prefetchZoomDelta = 0,
                .threadPool = threadPool,
                .dynamicTextureAtlas = dynamicTextureAtlas};
    }
};

} // namespace

// A contour tile is populated again whenever its DEM tile's border is
// backfilled from a newly arrived neighbour. The populations run on a thread
// pool and can finish in either order, so an older result must not replace a
// newer one.
TEST(ContourTile, OlderPopulateResultDoesNotReplaceNewer) {
    ContourTileTest test;
    const OverscaledTileID id(0, 0, 0);
    const auto params = test.parameters();
    RasterDEMTile dem(id, "dem", params, test.tileset);
    dem.onParsed(std::make_unique<HillshadeBucket>(PremultipliedImage({16, 16}), Tileset::RasterEncoding::Mapbox), 0);
    ASSERT_NE(nullptr, dem.getBucket());

    ContourTile tile(id, "contours", params);
    const algorithm::contour::UnitConfig unit;

    auto& tasks = test.scheduler->tasks;
    const auto before = tasks.size();
    tile.populateFromDEM(dem, 10.0, 5, unit);
    ASSERT_EQ(before + 1, tasks.size());
    auto first = std::move(tasks.back());
    tasks.pop_back();
    tile.populateFromDEM(dem, 10.0, 5, unit);
    ASSERT_EQ(before + 1, tasks.size());
    auto second = std::move(tasks.back());
    tasks.pop_back();

    // The second population finishes first, then the first one.
    second();
    first();
    test.loop.runOnce();

    EXPECT_EQ(2u, tile.populatedGeneration());
}

TEST(ContourTile, PopulateResultsInOrderApplyTheNewest) {
    ContourTileTest test;
    const OverscaledTileID id(0, 0, 0);
    const auto params = test.parameters();
    RasterDEMTile dem(id, "dem", params, test.tileset);
    dem.onParsed(std::make_unique<HillshadeBucket>(PremultipliedImage({16, 16}), Tileset::RasterEncoding::Mapbox), 0);

    ContourTile tile(id, "contours", params);
    const algorithm::contour::UnitConfig unit;
    EXPECT_EQ(0u, tile.populatedGeneration());

    auto& tasks = test.scheduler->tasks;
    tile.populateFromDEM(dem, 10.0, 5, unit);
    auto first = std::move(tasks.back());
    tasks.pop_back();
    tile.populateFromDEM(dem, 10.0, 5, unit);
    auto second = std::move(tasks.back());
    tasks.pop_back();

    first();
    second();
    test.loop.runOnce();

    EXPECT_EQ(2u, tile.populatedGeneration());
}

// A snapshot keeps the DEM image as it was: the contour source populates from
// it after the render thread's current messages, by when a backfill may have
// changed the DEM tile, or the DEM tile may be gone.
TEST(ContourTile, ASnapshotKeepsTheImageAsItWas) {
    ContourTileTest test;
    const OverscaledTileID id(1, 0, 0);
    const auto params = test.parameters();
    RasterDEMTile dem(id, "dem", params, test.tileset);
    dem.onParsed(std::make_unique<HillshadeBucket>(PremultipliedImage({16, 16}), Tileset::RasterEncoding::Mapbox), 0);

    const auto taken = ContourTile::snapshot(dem);
    ASSERT_TRUE(taken);
    EXPECT_EQ(16, taken->dim);
    const PremultipliedImage before = taken->image->clone();

    // The neighbour to the east, with every pixel set.
    PremultipliedImage bright({16, 16});
    std::fill(bright.data.get(), bright.data.get() + bright.bytes(), 0xFF);
    const DEMData east(bright, Tileset::RasterEncoding::Mapbox);
    dem.getBucket()->getDEMData().backfillBorder(east, 1, 0);
    ASSERT_NE(0, std::memcmp(dem.getBucket()->getDEMData().getImage()->data.get(), before.data.get(), before.bytes()));

    EXPECT_EQ(0, std::memcmp(taken->image->data.get(), before.data.get(), before.bytes()));
}

// A DEM tile with no data gives no snapshot.
TEST(ContourTile, ADEMTileWithNoDataGivesNoSnapshot) {
    ContourTileTest test;
    const auto params = test.parameters();
    RasterDEMTile dem(OverscaledTileID(0, 0, 0), "dem", params, test.tileset);
    EXPECT_FALSE(ContourTile::snapshot(dem));
}
