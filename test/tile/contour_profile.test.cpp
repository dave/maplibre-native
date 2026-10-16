// SCRATCH (scratch/contour-profile): profiles real contour tiles and diffs
// the lossless levers' renders. Not for commit.

#include <mln/test/map_adapter.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/stub_geometry_tile_feature.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/map_options.hpp>
#include <mln/renderer/buckets/line_bucket.hpp>
#include <mln/style/style.hpp>
#include <mln/tile/contour_probe.hpp>
#include <mln/util/image.hpp>
#include <mln/util/io.hpp>
#include <mln/util/label_steps.hpp>
#include <mln/util/run_loop.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace mln;

namespace {

std::string demDir() {
    const char* dir = std::getenv("CONTOUR_DEM_DIR");
    if (!dir) throw std::runtime_error("set CONTOUR_DEM_DIR");
    return dir;
}

std::string styleJSON() {
    // The app's contour source and band layers (metres), on a white background.
    auto band = [](const std::string& id, int minzoom, const std::string& filter, double w0, double w1) {
        return R"({"id": ")" + id + R"(", "type": "line", "source": "contours", "source-layer": "contours", "minzoom": )" +
               std::to_string(minzoom) + R"(, "filter": )" + filter +
               R"(, "paint": {"line-color": "black", "line-width": ["interpolate", ["linear"], ["zoom"], 9, )" +
               std::to_string(w0) + ", 15, " + std::to_string(w1) + R"(], "line-opacity": 1}})";
    };
    return R"({"version": 8, "sources": {
        "dem": {"type": "raster-dem", "tiles": ["http://dem/{z}/{x}/{y}"], "encoding": "terrarium", "tileSize": 512, "maxzoom": 12},
        "contours": {"type": "contour", "source": "dem", "intervals": [200, 10, 100, 11, 20, 12, 10], "unit": "meters",
                     "majorMultiplier": [1, 11, 5, 12, 10]}},
      "layers": [{"id": "background", "type": "background", "paint": {"background-color": "white"}},)" +
           band("band_200", 9, R"(["all", ["!=", ["get", "ele"], 0], ["==", ["%", ["get", "ele"], 200], 0]])", 0.4, 0.8) +
           "," +
           band("band_100",
                10,
                R"(["all", ["!=", ["get", "ele"], 0], ["==", ["%", ["get", "ele"], 100], 0], ["!=", ["%", ["get", "ele"], 200], 0]])",
                0.4,
                0.8) +
           "," +
           band("band_20",
                11,
                R"(["all", ["==", ["%", ["get", "ele"], 20], 0], ["!=", ["%", ["get", "ele"], 100], 0]])",
                0.4,
                0.6) +
           "," +
           band("band_10",
                12,
                R"(["all", ["==", ["%", ["get", "ele"], 10], 0], ["!=", ["%", ["get", "ele"], 20], 0]])",
                0.4,
                0.6) +
           "]}";
}

LatLng tileCentre(int z, int x, int y) {
    const double n = std::pow(2.0, z);
    const double lon = (x + 0.5) / n * 360.0 - 180.0;
    const double lat = std::atan(std::sinh(M_PI * (1.0 - 2.0 * (y + 0.5) / n))) * 180.0 / M_PI;
    return {lat, lon};
}

struct RenderResult {
    PremultipliedImage image;
    double parseMs = 0;
    double layoutMs = 0;
    int parses = 0;
};

RenderResult renderAt(const LatLng& centre, double zoom, Size size, float ratio) {
    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>();
    fileSource->tileResponse = [](const Resource& resource) -> std::optional<Response> {
        // http://dem/z/x/y
        const std::string prefix = "http://dem/";
        std::string rest = resource.url.substr(prefix.size());
        for (auto& c : rest)
            if (c == '/') c = '_';
        Response response;
        const std::string path = demDir() + "/" + rest + ".webp";
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) {
            throw std::runtime_error("missing DEM tile " + path);
        }
        std::fclose(f);
        response.data = std::make_shared<std::string>(util::read_file(path));
        return response;
    };
    HeadlessFrontend frontend{size, ratio};
    MapAdapter map{frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()).withPixelRatio(ratio)};
    map.getStyle().loadJSON(styleJSON());
    map.jumpTo(CameraOptions().withCenter(centre).withZoom(zoom));
    util::LabelSteps::setRecording(true);
    util::LabelSteps::take();
    RenderResult result;
    result.image = frontend.render(map).image;
    util::LabelSteps::setRecording(false);
    for (const auto& step : util::LabelSteps::take()) {
        if (step.sourceID != "contours") continue;
        const double ms = std::chrono::duration<double, std::milli>(step.end - step.start).count();
        if (step.name == "tile-parse") {
            result.parseMs += ms;
            result.parses++;
        }
        if (step.name == "tile-layout") result.layoutMs += ms;
    }
    return result;
}

struct Diff {
    std::size_t differing = 0, over8 = 0, over16 = 0, over32 = 0;
    int max = 0;
};

Diff diff(const PremultipliedImage& a, const PremultipliedImage& b) {
    EXPECT_EQ(a.size, b.size);
    Diff d;
    const std::size_t n = a.bytes() / 4;
    for (std::size_t i = 0; i < n; i++) {
        int m = 0;
        for (int c = 0; c < 4; c++) m = std::max(m, std::abs(int(a.data[i * 4 + c]) - int(b.data[i * 4 + c])));
        if (m > 0) d.differing++;
        if (m > 8) d.over8++;
        if (m > 16) d.over16++;
        if (m > 32) d.over32++;
        d.max = std::max(d.max, m);
    }
    return d;
}

void printStats(const char* label, const contour_probe::Stats& s) {
    std::printf(
        "%s: populates=%d jobsRun=%d skipped=%d applied=%d dropped=%d\n"
        "  render-thread snapshot %.2f ms total (%.2f per populate)\n"
        "  worker: decode %.2f, march %.2f, shift %.2f, clip %.2f, dp %.2f, chaikin1 %.2f, chaikin2 %.2f, post %.2f, "
        "build %.2f ms (sum over jobs)\n"
        "  points: march %zu (lines %zu), clip %zu, dp %zu, chaikin1 %zu, chaikin2 %zu, post %zu; features %zu\n",
        label,
        s.populates,
        s.jobsRun,
        s.jobsSkipped,
        s.applied,
        s.dropped,
        s.snapshotMs,
        s.populates ? s.snapshotMs / s.populates : 0.0,
        s.decodeMs,
        s.marchMs,
        s.shiftMs,
        s.clipMs,
        s.dpMs,
        s.ch1Ms,
        s.ch2Ms,
        s.postMs,
        s.buildMs,
        s.marchPoints,
        s.marchLines,
        s.clipPoints,
        s.dpPoints,
        s.ch1Points,
        s.ch2Points,
        s.postPoints,
        s.features);
}

// The line layout of the band layers for one tile's features, as the worker's
// parse builds it (default line layout, no data-driven paint).
void layoutBands(const std::string& key, int z) {
    std::lock_guard<std::mutex> lock(contour_probe::mutex);
    auto it = contour_probe::lastFeatures.find(key);
    ASSERT_NE(it, contour_probe::lastFeatures.end()) << key;
    struct Band {
        const char* name;
        int minzoom;
        std::function<bool(std::int64_t)> keep;
    };
    const std::vector<Band> bands = {
        {"band_200", 9, [](std::int64_t e) { return e != 0 && e % 200 == 0; }},
        {"band_100", 10, [](std::int64_t e) { return e != 0 && e % 100 == 0 && e % 200 != 0; }},
        {"band_20", 11, [](std::int64_t e) { return e % 20 == 0 && e % 100 != 0; }},
        {"band_10", 12, [](std::int64_t e) { return e % 10 == 0 && e % 20 != 0; }},
    };
    std::size_t totalV = 0, totalT = 0;
    double totalMs = 0;
    for (const auto& band : bands) {
        if (band.minzoom > z) continue;
        LineBucket::PossiblyEvaluatedLayoutProperties layout;
        LineBucket bucket{layout, {}, float(z), 1};
        std::size_t n = 0, pts = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (const auto& f : it->second) {
            const auto ele = f.properties.at("ele").get<std::int64_t>();
            if (!band.keep(ele)) continue;
            const auto& ls = f.geometry.get<mapbox::geometry::line_string<std::int16_t>>();
            GeometryCoordinates coords;
            coords.reserve(ls.size());
            for (const auto& p : ls) coords.emplace_back(p.x, p.y);
            GeometryCollection geom{std::move(coords)};
            pts += ls.size();
            bucket.addFeature(StubGeometryTileFeature{FeatureType::LineString, geom},
                              geom,
                              {},
                              PatternLayerMap(),
                              n++,
                              CanonicalTileID(0, 0, 0));
        }
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  layout %s: features %zu, input points %zu, vertices %zu, triangles %zu, %.2f ms\n",
                    band.name,
                    n,
                    pts,
                    bucket.vertices.elements(),
                    bucket.triangles.elements(),
                    ms);
        totalV += bucket.vertices.elements();
        totalT += bucket.triangles.elements();
        totalMs += ms;
    }
    std::printf("  layout total: vertices %zu, triangles %zu, %.2f ms\n", totalV, totalT, totalMs);
}

struct Area {
    const char* name;
    int x, y; // z11 tile
};
const Area areas[] = {{"gpt", 609, 1341}, {"alps", 1067, 728}};

struct View {
    const char* name;
    double zoom;
    Size size;
    int z;  // canonical z of the centre's contour tile
};
const View views[] = {
    {"z11", 11.0, {700, 700}, 11},
    {"z12", 12.0, {900, 900}, 12},
    {"z14", 14.0, {700, 700}, 12},
    {"z16", 16.0, {700, 700}, 12},
};

struct Config {
    const char* name;
    double postEpsilon;
    bool raw;
    bool skip;
    bool coalesce = false;
    bool fast = false;
};

void apply(const Config& c) {
    contour_probe::postEpsilon = c.postEpsilon;
    contour_probe::rawSnapshot = c.raw;
    contour_probe::skipStale = c.skip;
    contour_probe::coalesce = c.coalesce;
    contour_probe::fastMarch = c.fast;
}

} // namespace

TEST(ContourProfile, Profile) {
    const char* configsEnv = std::getenv("CONTOUR_CONFIGS"); // e.g. "0,0.5,1,2,4"
    std::vector<Config> configs = {{"base", 0, false, false}};
    if (configsEnv) {
        std::string s = configsEnv;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t comma = s.find(',', pos);
            std::string tok = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            if (tok == "raw")
                configs.push_back({"raw", 0, true, false});
            else if (tok == "skip")
                configs.push_back({"skip", 0, false, true});
            else if (tok == "coalesce")
                configs.push_back({"coalesce", 0, false, false, true});
            else if (tok == "cs")
                configs.push_back({"coalesce+skip", 0, false, true, true});
            else if (tok == "fast")
                configs.push_back({"fast", 0, false, false, false, true});
            else if (tok == "all")
                configs.push_back({"fast+coalesce+skip+raw", 0, true, true, true, true});
            else
                configs.push_back({strdup(("post" + tok).c_str()), std::stod(tok), false, false});
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    const int repeats = std::getenv("CONTOUR_REPEATS") ? std::atoi(std::getenv("CONTOUR_REPEATS")) : 1;
    for (const auto& area : areas) {
        for (const auto& view : views) {
            const int cz = view.z;
            const int cx = cz == 11 ? area.x : area.x * 2;
            const int cy = cz == 11 ? area.y : area.y * 2;
            // The centre of the z11 tile.
            const LatLng centre = tileCentre(11, area.x, area.y);
            const std::string key = std::to_string(cz) + "/" + std::to_string(cx) + "/" + std::to_string(cy);
            PremultipliedImage baseImage;
            for (const auto& config : configs) {
                apply(config);
                for (int r = 0; r < repeats; r++) {
                    contour_probe::take();
                    {
                        std::lock_guard<std::mutex> lock(contour_probe::mutex);
                        contour_probe::lastFeatures.clear();
                    }
                    auto result = renderAt(centre, view.zoom, view.size, 3.0f);
                    const auto stats = contour_probe::take();
                    std::printf("\n=== %s %s %s run %d: contour tile-parse %.1f ms over %d parses, tile-layout %.1f ms\n",
                                area.name,
                                view.name,
                                config.name,
                                r,
                                result.parseMs,
                                result.parses,
                                result.layoutMs);
                    printStats(config.name, stats);
                    if (r == 0) {
                        layoutBands(key, cz);
                        const std::string out = std::string(std::getenv("CONTOUR_OUT_DIR") ? std::getenv("CONTOUR_OUT_DIR") : "/tmp") +
                                                "/" + area.name + "-" + view.name + "-" + config.name + ".png";
                        util::write_file(out, encodePNG(result.image));
                        if (std::string(config.name) == "base") {
                            baseImage = result.image.clone();
                        } else {
                            const auto d = diff(baseImage, result.image);
                            std::printf("  DIFF vs base: differing %zu, >8 %zu, >16 %zu, >32 %zu, max %d (of %zu px)\n",
                                        d.differing,
                                        d.over8,
                                        d.over16,
                                        d.over32,
                                        d.max,
                                        result.image.bytes() / 4);
                        }
                    }
                }
            }
        }
    }
    apply({"base", 0, false, false});
}

// Two base renders must be identical, or the diff measures noise.
TEST(ContourProfile, BaseIsDeterministic) {
    apply({"base", 0, false, false});
    for (const auto& area : areas) {
        const LatLng centre = tileCentre(11, area.x, area.y);
        auto a = renderAt(centre, 12.0, {900, 900}, 3.0f);
        auto b = renderAt(centre, 12.0, {900, 900}, 3.0f);
        const auto d = diff(a.image, b.image);
        std::printf("%s determinism: differing %zu, max %d\n", area.name, d.differing, d.max);
        EXPECT_EQ(0u, d.differing);
    }
}
