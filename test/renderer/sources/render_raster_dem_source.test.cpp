#include <mln/test/fake_file_source.hpp>
#include <mln/test/util.hpp>

#include <mln/actor/scheduler.hpp>
#include <mln/annotation/annotation_manager.hpp>
#include <mln/geometry/dem_data.hpp>
#include <mln/gfx/dynamic_texture_atlas.hpp>
#include <mln/map/transform.hpp>
#include <mln/renderer/buckets/hillshade_bucket.hpp>
#include <mln/renderer/image_manager.hpp>
#include <mln/renderer/sources/render_raster_dem_source.hpp>
#include <mln/renderer/tile_parameters.hpp>
#include <mln/style/sources/tile_source_impl.hpp>
#include <mln/style/style.hpp>
#include <mln/text/glyph_manager.hpp>
#include <mln/tile/raster_dem_tile.hpp>
#include <mln/util/image.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/tileset.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace mln;

namespace {

/// A file source that answers cache-only requests, as the default
/// platform's does: each tile then asks the cache first and the network
/// second, so one tile can parse two bodies.
class CacheThenNetworkFileSource : public FakeFileSource {
public:
    bool supportsCacheOnlyRequests() const override { return true; }

    /// Answers the open request for `url` that uses `method`. False when
    /// there is none.
    bool respond(const std::string& url, Resource::LoadingMethod method, const Response& response) {
        const auto it = std::ranges::find_if(requests, [&](const FakeFileRequest* request) {
            return request->resource.url == url && request->resource.loadingMethod == method;
        });
        if (it == requests.end()) {
            return false;
        }
        // Copy the callback: calling it frees the request.
        const Callback callback = (*it)->callback;
        callback(response);
        return true;
    }
};

/// A Mapbox-encoded DEM tile of one height everywhere. The height is
/// `-10000 + 25.6 * level` metres.
std::shared_ptr<const std::string> flatDEMTile(uint8_t level) {
    constexpr uint32_t dim = 8;
    PremultipliedImage image({dim, dim});
    for (std::size_t i = 0; i < image.bytes(); i += 4) {
        image.data[i + 0] = 0;
        image.data[i + 1] = level;
        image.data[i + 2] = 0;
        image.data[i + 3] = 255;
    }
    return std::make_shared<const std::string>(encodePNG(image));
}

int32_t heightOf(uint8_t level) {
    const std::array<uint8_t, 4> rgba{0, level, 0, 255};
    return DEMData::decode(rgba.data(), DEMData::unpackVector(Tileset::RasterEncoding::Mapbox));
}

std::string urlOf(const CanonicalTileID& id) {
    return "https://example.com/" + std::to_string(id.z) + "/" + std::to_string(id.x) + "/" + std::to_string(id.y) +
           ".png";
}

/// Drives a RenderRasterDEMSource without a renderer: one update over the
/// four z1 tiles, then the test answers each tile's requests.
class RenderRasterDEMSourceTest {
public:
    util::SimpleIdentity uniqueID;
    util::RunLoop loop;
    std::shared_ptr<CacheThenNetworkFileSource> fileSource = std::make_shared<CacheThenNetworkFileSource>();
    TransformState transformState;
    std::shared_ptr<ImageManager> imageManager = ImageManager::create();
    std::shared_ptr<GlyphManager> glyphManager = std::make_shared<GlyphManager>();
    gfx::DynamicTextureAtlasPtr dynamicTextureAtlas;
    TaggedScheduler threadPool{Scheduler::GetBackground(), uniqueID};
    style::Style style{fileSource, 1, threadPool};
    AnnotationManager annotationManager{style};

    Tileset tileset{{"https://example.com/{z}/{x}/{y}.png"}, {1, 1}};
    Immutable<style::TileSource::Impl> impl = makeMutable<style::TileSource::Impl>(
        *makeMutable<style::TileSource::Impl>(style::SourceType::RasterDEM, "dem", util::tileSize_I), tileset);
    RenderRasterDEMSource source{impl, threadPool};

    RenderRasterDEMSourceTest() {
        // The four z1 tiles (512 px each), and no tile of another world copy:
        // the view is a little smaller than the 1024 px world.
        Transform transform;
        transform.resize({1000, 1000});
        transform.jumpTo(CameraOptions().withCenter(LatLng()).withZoom(1.0));
        transformState = transform.getState();

        const TileParameters parameters{.pixelRatio = 1.0,
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
        static_cast<RenderSource&>(source).update(
            impl, {}, /*needsRendering=*/true, /*needsRelayout=*/false, parameters);
    }

    ~RenderRasterDEMSourceTest() { threadPool.waitForEmpty(); }

    const RasterDEMTile* tile(const CanonicalTileID& id) const {
        const RasterDEMTile* found = source.getTile(OverscaledTileID(id.z, 0, id));
        return found != nullptr && found->isRenderable() ? found : nullptr;
    }

    const DEMData& dem(const CanonicalTileID& id) const { return tile(id)->getBucket()->getDEMData(); }

    /// Runs the loop until `done()`. Each turn waits for the next event.
    template <typename Done>
    void runUntil(Done done) {
        while (!done()) {
            loop.runOnce();
        }
    }
};

/// The heights of a tile's border column `x` (for example -1, the west
/// border, or dim, the east border).
std::vector<int32_t> borderColumn(const DEMData& dem, int32_t x) {
    std::vector<int32_t> column;
    for (int32_t y = 0; y < dem.dim; ++y) {
        column.push_back(dem.get(x, y));
    }
    return column;
}

/// The heights of a tile's border row `y` (for example -1, the north
/// border).
std::vector<int32_t> borderRow(const DEMData& dem, int32_t y) {
    std::vector<int32_t> row;
    for (int32_t x = 0; x < dem.dim; ++x) {
        row.push_back(dem.get(x, y));
    }
    return row;
}

} // namespace

// A tile that parses a second, different body (the network answer after the
// cache's) must pass its new edge to the neighbours that already hold its old
// one. Before, only the re-parsed tile's own border was refilled: each
// neighbour kept the old edge, and the hillshade drew a seam on the
// neighbour's side of the boundary.
TEST(RenderRasterDEMSource, AReparsedTilesNewEdgeReachesItsNeighbours) {
    RenderRasterDEMSourceTest test;
    const CanonicalTileID west{1, 0, 0};
    const CanonicalTileID east{1, 1, 0};
    const CanonicalTileID south{1, 0, 1};
    const CanonicalTileID tiles[] = {west, east, south, {1, 1, 1}};

    constexpr uint8_t westCached = 10;
    constexpr uint8_t westFresh = 30;
    constexpr uint8_t eastLevel = 20;
    constexpr uint8_t southLevel = 40;

    // Each tile asks the cache first. Answer every cache request.
    for (const auto& id : tiles) {
        const uint8_t level = id == west ? westCached : id == east ? eastLevel : southLevel;
        Response cached;
        cached.data = flatDEMTile(level);
        ASSERT_TRUE(test.fileSource->respond(urlOf(id), Resource::LoadingMethod::CacheOnly, cached))
            << "no cache request for " << urlOf(id);
    }
    test.runUntil([&] { return std::ranges::all_of(tiles, [&](const auto& id) { return test.tile(id); }); });

    const auto dim = test.dem(west).dim;
    ASSERT_EQ(std::vector<int32_t>(dim, heightOf(westCached)), borderColumn(test.dem(east), -1));
    ASSERT_EQ(std::vector<int32_t>(dim, heightOf(eastLevel)), borderColumn(test.dem(west), dim));
    ASSERT_EQ(std::vector<int32_t>(dim, heightOf(westCached)), borderRow(test.dem(south), -1));

    // The network answers: the east tile's body has not changed, the west
    // tile's has.
    for (const auto& id : tiles) {
        if (id == west) continue;
        Response notModified;
        notModified.notModified = true;
        ASSERT_TRUE(test.fileSource->respond(urlOf(id), Resource::LoadingMethod::NetworkOnly, notModified))
            << "no network request for " << urlOf(id);
    }
    const HillshadeBucket* const cachedBucket = test.tile(west)->getBucket();
    Response fresh;
    fresh.data = flatDEMTile(westFresh);
    ASSERT_TRUE(test.fileSource->respond(urlOf(west), Resource::LoadingMethod::NetworkOnly, fresh));
    test.runUntil([&] { return test.tile(west)->getBucket() != cachedBucket; });

    // The re-parsed tile's own border holds its neighbour's edge.
    EXPECT_EQ(std::vector<int32_t>(dim, heightOf(eastLevel)), borderColumn(test.dem(west), dim));
    // The neighbours' borders hold the re-parsed tile's new edge.
    EXPECT_EQ(std::vector<int32_t>(dim, heightOf(westFresh)), borderColumn(test.dem(east), -1));
    EXPECT_EQ(std::vector<int32_t>(dim, heightOf(westFresh)), borderRow(test.dem(south), -1));
}
