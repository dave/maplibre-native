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
#include <mln/util/feature.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/tileset.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
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
    const ContourTile::LineOptions lineOptions{.intervalDisplayUnits = 10.0, .majorMultiplier = 5, .unit = {}};

    auto& tasks = test.scheduler->tasks;
    const auto before = tasks.size();
    tile.populateFromDEM(dem, lineOptions);
    ASSERT_EQ(before + 1, tasks.size());
    auto first = std::move(tasks.back());
    tasks.pop_back();
    tile.populateFromDEM(dem, lineOptions);
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
    const ContourTile::LineOptions lineOptions{.intervalDisplayUnits = 10.0, .majorMultiplier = 5, .unit = {}};
    EXPECT_EQ(0u, tile.populatedGeneration());

    auto& tasks = test.scheduler->tasks;
    tile.populateFromDEM(dem, lineOptions);
    auto first = std::move(tasks.back());
    tasks.pop_back();
    tile.populateFromDEM(dem, lineOptions);
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

namespace {

// A snapshot whose samples hold the heights a function gives, by buffered column,
// in the terrarium encoding. Its tile covers 16 samples.
template <typename Height>
ContourTile::DEMSnapshot terrariumSnapshot(Height height) {
    constexpr int dim = 16;
    constexpr int width = dim + (2 * DEMData::border);
    PremultipliedImage image({width, width});
    for (int y = 0; y < width; y++) {
        for (int x = 0; x < width; x++) {
            const double value = height(x, y) + 32768.0;
            uint8_t* pixel = image.data.get() + (static_cast<std::size_t>(y * width + x) * 4);
            pixel[0] = static_cast<uint8_t>(std::floor(value / 256.0));
            pixel[1] = static_cast<uint8_t>(static_cast<int>(std::floor(value)) % 256);
            pixel[2] = static_cast<uint8_t>((value - std::floor(value)) * 256.0);
            pixel[3] = 255;
        }
    }
    return {.image = std::make_shared<const PremultipliedImage>(std::move(image)),
            .dim = dim,
            .unpack = DEMData::unpackVector(Tileset::RasterEncoding::Terrarium)};
}

using ContourLine = mapbox::geometry::line_string<std::int16_t>;

} // namespace

// A height keeps its fraction: the 100 m line crosses between 99.75 m and
// 100.75 m a quarter of the way along, and whole metres would have no line there.
TEST(ContourTile, HeightsKeepTheirFraction) {
    constexpr int firstHigh = 10;
    const auto snapshot = terrariumSnapshot([](int x, int) { return x < firstHigh ? 99.75 : 100.75; });

    const auto features = ContourTile::buildFeatures(snapshot,
                                                     {.intervalDisplayUnits = 100.0, .majorMultiplier = 5, .unit = {}});

    ASSERT_EQ(1u, features.size());
    EXPECT_EQ(100, features[0].properties.at("ele").get<std::int64_t>());
    // A buffered column x is a tile sample at (x - border + 0.5) cells; a cell is
    // EXTENT / dim = 512 units. The crossing is a quarter of a cell after column 9.
    const double expected = (firstHigh - 1 - DEMData::border + 0.5 + 0.25) * 512.0;
    for (const auto& point : features[0].geometry.get<ContourLine>()) {
        EXPECT_NEAR(expected, point.x, 1.0);
    }
}

// The fraction is kept below sea level too: truncation moved heights toward zero.
TEST(ContourTile, NegativeHeightsKeepTheirFraction) {
    constexpr int firstHigh = 10;
    const auto snapshot = terrariumSnapshot([](int x, int) { return x < firstHigh ? -100.25 : -99.25; });

    const auto features = ContourTile::buildFeatures(snapshot,
                                                     {.intervalDisplayUnits = 100.0, .majorMultiplier = 5, .unit = {}});

    ASSERT_EQ(1u, features.size());
    EXPECT_EQ(-100, features[0].properties.at("ele").get<std::int64_t>());
    const double expected = (firstHigh - 1 - DEMData::border + 0.5 + 0.25) * 512.0;
    for (const auto& point : features[0].geometry.get<ContourLine>()) {
        EXPECT_NEAR(expected, point.x, 1.0);
    }
}

// A fractional interval keeps its value: `interval` is 0.5, not 1, the lines have
// their own `ele`, and a major line is every Nth line.
TEST(ContourTile, AFractionalIntervalKeepsItsValue) {
    const auto snapshot = terrariumSnapshot([](int x, int) { return 100.0 + ((x - 10) * 0.2); });

    const auto features = ContourTile::buildFeatures(snapshot,
                                                     {.intervalDisplayUnits = 0.5, .majorMultiplier = 2, .unit = {}});

    ASSERT_GT(features.size(), 3u);
    std::set<double> elevations;
    for (const auto& feature : features) {
        const double elevation = numericValue<double>(feature.properties.at("ele")).value();
        elevations.insert(elevation);
        EXPECT_DOUBLE_EQ(0.5, numericValue<double>(feature.properties.at("interval")).value());
        EXPECT_DOUBLE_EQ(0.0, std::fmod(elevation, 0.5)) << elevation;
        EXPECT_EQ(std::fmod(elevation, 1.0) == 0.0, feature.properties.at("major").get<bool>()) << elevation;
    }
    EXPECT_EQ(features.size(), elevations.size()) << "each line has its own elevation";
}

// A whole interval stays whole numbers, as a filter on `ele` expects.
TEST(ContourTile, AWholeIntervalHasWholeValues) {
    const auto snapshot = terrariumSnapshot([](int x, int) { return 100.0 + (x - 10) * 20.0; });

    const auto features = ContourTile::buildFeatures(snapshot,
                                                     {.intervalDisplayUnits = 50.0, .majorMultiplier = 2, .unit = {}});

    ASSERT_GT(features.size(), 3u);
    for (const auto& feature : features) {
        const auto elevation = feature.properties.at("ele").get<std::int64_t>();
        EXPECT_EQ(50, feature.properties.at("interval").get<std::int64_t>());
        EXPECT_EQ(elevation % 100 == 0, feature.properties.at("major").get<bool>()) << elevation;
    }
}
