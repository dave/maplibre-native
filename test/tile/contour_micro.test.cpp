// SCRATCH (scratch/contour-profile): one contour tile's steps, timed on one
// thread, from real DEM tiles with all eight neighbours backfilled. Not for
// commit.

#include <mln/test/fake_file_source.hpp>
#include <mln/test/stub_geometry_tile_feature.hpp>

#include <mln/actor/scheduler.hpp>
#include <mln/algorithm/contour/isolines.hpp>
#include <mln/annotation/annotation_manager.hpp>
#include <mln/geometry/dem_data.hpp>
#include <mln/gfx/dynamic_texture_atlas.hpp>
#include <mln/map/transform.hpp>
#include <mln/renderer/buckets/hillshade_bucket.hpp>
#include <mln/renderer/buckets/line_bucket.hpp>
#include <mln/renderer/image_manager.hpp>
#include <mln/renderer/tile_parameters.hpp>
#include <mln/style/style.hpp>
#include <mln/text/glyph_manager.hpp>
#include <mln/tile/contour_probe.hpp>
#include <mln/tile/contour_tile.hpp>
#include <mln/tile/raster_dem_tile.hpp>
#include <mln/util/image.hpp>
#include <mln/util/io.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/tileset.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

using namespace mln;

namespace {

class InlineScheduler final : public Scheduler {
public:
    void schedule(std::function<void()>&& fn) override { tasks.push_back(std::move(fn)); }
    void schedule(const util::SimpleIdentity, std::function<void()>&& fn) override { schedule(std::move(fn)); }
    mapbox::base::WeakPtr<Scheduler> makeWeakPtr() override { return weakFactory.makeWeakPtr(); }
    void waitForEmpty(const util::SimpleIdentity) override {}
    void runAll() {
        while (!tasks.empty()) {
            auto t = std::move(tasks.front());
            tasks.erase(tasks.begin());
            t();
        }
    }
    std::vector<std::function<void()>> tasks;

private:
    mapbox::base::WeakPtrFactory<Scheduler> weakFactory{this};
};

class Harness {
public:
    util::SimpleIdentity uniqueID;
    util::RunLoop loop;
    std::shared_ptr<FileSource> fileSource = std::make_shared<FakeFileSource>();
    TransformState transformState;
    std::shared_ptr<ImageManager> imageManager = ImageManager::create();
    std::shared_ptr<GlyphManager> glyphManager = std::make_shared<GlyphManager>();
    gfx::DynamicTextureAtlasPtr dynamicTextureAtlas;
    std::shared_ptr<InlineScheduler> scheduler = std::make_shared<InlineScheduler>();
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

PremultipliedImage load(int z, int x, int y) {
    const std::string path = std::string(std::getenv("CONTOUR_DEM_DIR")) + "/" + std::to_string(z) + "_" +
                             std::to_string(x) + "_" + std::to_string(y) + ".webp";
    return decodeImage(util::read_file(path));
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

} // namespace

// The faster marching squares gives the same lines, in the same order, as
// today's, on real tiles at each interval the app uses.
TEST(ContourMicro, FastMarchIsIdentical) {
    struct Case {
        const char* name;
        int z, x, y;
    };
    const Case cases[] = {{"gpt", 11, 609, 1341}, {"alps", 11, 1067, 728}, {"gpt", 12, 1218, 2682},
                          {"alps", 12, 2134, 1456}};
    const int runs = std::getenv("CONTOUR_RUNS") ? std::atoi(std::getenv("CONTOUR_RUNS")) : 5;
    for (const auto& c : cases) {
        DEMData centre(load(c.z, c.x, c.y), Tileset::RasterEncoding::Terrarium);
        for (int dx = -1; dx <= 1; dx++) {
            for (int dy = -1; dy <= 1; dy++) {
                if (!dx && !dy) continue;
                DEMData n(load(c.z, c.x + dx, c.y + dy), Tileset::RasterEncoding::Terrarium);
                centre.backfillBorder(n, int8_t(dx), int8_t(dy));
            }
        }
        const int border = DEMData::border;
        const int width = centre.dim + 2 * border;
        std::vector<std::int16_t> heights;
        for (int y = -border; y < centre.dim + border; y++)
            for (int x = -border; x < centre.dim + border; x++)
                heights.push_back(static_cast<std::int16_t>(centre.get(x, y)));
        for (double interval : {200.0, 100.0, 20.0, 10.0}) {
            algorithm::contour::ContourThresholds t;
            t.interval = interval;
            t.extent = static_cast<int>(std::lround(8192.0 * (width - 1) / centre.dim));
            std::vector<double> oldMs, fastMs;
            std::vector<algorithm::contour::ContourLineString> a, b;
            for (int r = 0; r < runs; r++) {
                auto t0 = std::chrono::steady_clock::now();
                a = algorithm::contour::generateContours(heights, width, width, t);
                auto t1 = std::chrono::steady_clock::now();
                b = algorithm::contour::generateContoursFast(heights, width, width, t);
                auto t2 = std::chrono::steady_clock::now();
                oldMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
                fastMs.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
            }
            ASSERT_EQ(a.size(), b.size()) << c.name << " " << interval;
            std::size_t points = 0;
            for (std::size_t i = 0; i < a.size(); i++) {
                ASSERT_EQ(a[i].elevation, b[i].elevation) << c.name << " line " << i;
                ASSERT_EQ(a[i].points, b[i].points) << c.name << " line " << i;
                points += a[i].points.size() / 2;
            }
            ASSERT_GT(a.size(), 0u);
            std::printf("%s %d/%d/%d interval %.0f: %zu lines, %zu points, identical; march %.2f -> %.2f ms (median of %d)\n",
                        c.name,
                        c.z,
                        c.x,
                        c.y,
                        interval,
                        a.size(),
                        points,
                        median(oldMs),
                        median(fastMs),
                        runs);
        }
    }
}

TEST(ContourMicro, Steps) {
    struct Case {
        const char* name;
        int z, x, y;
        double interval;
    };
    const Case cases[] = {{"gpt", 11, 609, 1341, 20.0}, {"alps", 11, 1067, 728, 20.0}};
    const int runs = std::getenv("CONTOUR_RUNS") ? std::atoi(std::getenv("CONTOUR_RUNS")) : 5;
    for (const auto& c : cases) {
        Harness h;
        const auto params = h.parameters();
        const OverscaledTileID id(uint8_t(c.z), 0, uint8_t(c.z), uint32_t(c.x), uint32_t(c.y));
        RasterDEMTile dem(id, "dem", params, h.tileset);
        dem.onParsed(std::make_unique<HillshadeBucket>(load(c.z, c.x, c.y), Tileset::RasterEncoding::Terrarium), 0);
        DEMData& centre = dem.getBucket()->getDEMData();
        for (int dx = -1; dx <= 1; dx++) {
            for (int dy = -1; dy <= 1; dy++) {
                if (!dx && !dy) continue;
                DEMData n(load(c.z, c.x + dx, c.y + dy), Tileset::RasterEncoding::Terrarium);
                centre.backfillBorder(n, int8_t(dx), int8_t(dy));
            }
        }
        ContourTile tile(id, "contours", params);
        const algorithm::contour::UnitConfig unit;
        std::vector<double> snap, march, shift, clip, dp, ch1, ch2, post, build, decode;
        contour_probe::Stats last;
        for (int r = 0; r < runs; r++) {
            contour_probe::take();
            tile.populateFromDEM(dem, c.interval, 5, unit);
            h.scheduler->runAll();
            h.loop.runOnce();
            last = contour_probe::take();
            snap.push_back(last.snapshotMs);
            decode.push_back(last.decodeMs);
            march.push_back(last.marchMs);
            shift.push_back(last.shiftMs);
            clip.push_back(last.clipMs);
            dp.push_back(last.dpMs);
            ch1.push_back(last.ch1Ms);
            ch2.push_back(last.ch2Ms);
            post.push_back(last.postMs);
            build.push_back(last.buildMs);
        }
        std::printf(
            "%s %d/%d/%d (median of %d, one thread): snapshot %.2f (render thread) | decode %.2f, march %.2f, "
            "shift %.2f, clip %.2f, dp %.2f, chaikin1 %.2f, chaikin2 %.2f, post %.2f, build %.2f ms\n",
            c.name,
            c.z,
            c.x,
            c.y,
            runs,
            median(snap),
            median(decode),
            median(march),
            median(shift),
            median(clip),
            median(dp),
            median(ch1),
            median(ch2),
            median(post),
            median(build));
        std::printf(
            "  points: march %zu in %zu lines, clip %zu, dp %zu, chaikin1 %zu, chaikin2 %zu, post %zu; features %zu\n",
            last.marchPoints,
            last.marchLines,
            last.clipPoints,
            last.dpPoints,
            last.ch1Points,
            last.ch2Points,
            last.postPoints,
            last.features);

        // The worker's line layout of the z11 band layers (band_200, band_100, band_20).
        std::lock_guard<std::mutex> lock(contour_probe::mutex);
        const auto key = std::to_string(c.z) + "/" + std::to_string(c.x) + "/" + std::to_string(c.y);
        const auto& features = contour_probe::lastFeatures.at(key);
        std::vector<std::function<bool(std::int64_t)>> bands = {
            [](std::int64_t e) { return e != 0 && e % 200 == 0; },
            [](std::int64_t e) { return e != 0 && e % 100 == 0 && e % 200 != 0; },
            [](std::int64_t e) { return e % 20 == 0 && e % 100 != 0; },
        };
        std::vector<double> layoutMs;
        std::size_t vertices = 0, triangles = 0, inPoints = 0;
        for (int r = 0; r < runs; r++) {
            vertices = triangles = inPoints = 0;
            const auto t0 = std::chrono::steady_clock::now();
            for (const auto& keep : bands) {
                LineBucket::PossiblyEvaluatedLayoutProperties layout;
                LineBucket bucket{layout, {}, float(c.z), 1};
                std::size_t n = 0;
                for (const auto& f : features) {
                    if (!keep(f.properties.at("ele").get<std::int64_t>())) continue;
                    const auto& ls = f.geometry.get<mapbox::geometry::line_string<std::int16_t>>();
                    GeometryCoordinates coords;
                    coords.reserve(ls.size());
                    for (const auto& p : ls) coords.emplace_back(p.x, p.y);
                    GeometryCollection geom{std::move(coords)};
                    inPoints += ls.size();
                    bucket.addFeature(StubGeometryTileFeature{FeatureType::LineString, geom},
                                      geom,
                                      {},
                                      PatternLayerMap(),
                                      n++,
                                      CanonicalTileID(0, 0, 0));
                }
                vertices += bucket.vertices.elements();
                triangles += bucket.triangles.elements();
            }
            layoutMs.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        std::printf("  line layout of 3 bands: %.2f ms (median), input points %zu, vertices %zu, triangles %zu\n",
                    median(layoutMs),
                    inPoints,
                    vertices,
                    triangles);
    }
}
