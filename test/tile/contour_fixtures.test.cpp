// Contour lines from real DEM tiles (test/fixtures/contour, see its README).

#include "../algorithm/contour/isolines_reference.hpp"

#include <mln/test/map_adapter.hpp>
#include <mln/test/stub_file_source.hpp>

#include <mln/algorithm/contour/isolines.hpp>
#include <mln/geometry/dem_data.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/map_options.hpp>
#include <mln/style/style.hpp>
#include <mln/util/image.hpp>
#include <mln/util/io.hpp>
#include <mln/util/run_loop.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace mln;

namespace {

const std::string kFixtures = "test/fixtures/contour/";

PremultipliedImage loadTile(int z, int x, int y) {
    return decodeImage(
        util::read_file(kFixtures + std::to_string(z) + "_" + std::to_string(x) + "_" + std::to_string(y) + ".webp"));
}

// The buffered height grid the contour tile hands to the marching squares,
// with the borders backfilled from whichever neighbours are given.
std::vector<std::int16_t> heights(DEMData& dem) {
    const int border = DEMData::border;
    std::vector<std::int16_t> out;
    for (int y = -border; y < dem.dim + border; y++) {
        for (int x = -border; x < dem.dim + border; x++) {
            out.push_back(static_cast<std::int16_t>(dem.get(x, y)));
        }
    }
    return out;
}

} // namespace

// The marching squares gives the same lines, with the same points, in the same
// order, as the version before it was made faster, on real tiles at every
// spacing the app uses.
TEST(ContourLines, TheMarchingSquaresMatchesTheReferenceOnRealTiles) {
    struct Case {
        int z, x, y;
        bool neighbours;
    };
    // A Patagonian ice-field tile with all eight neighbours backfilled, and an
    // alpine one with its own edges as borders.
    for (const Case c : {Case{11, 609, 1341, true}, Case{11, 1067, 728, false}}) {
        DEMData dem(loadTile(c.z, c.x, c.y), Tileset::RasterEncoding::Terrarium);
        if (c.neighbours) {
            for (int dx = -1; dx <= 1; dx++) {
                for (int dy = -1; dy <= 1; dy++) {
                    if (dx == 0 && dy == 0) continue;
                    const DEMData neighbour(loadTile(c.z, c.x + dx, c.y + dy), Tileset::RasterEncoding::Terrarium);
                    dem.backfillBorder(neighbour, static_cast<int8_t>(dx), static_cast<int8_t>(dy));
                }
            }
        }
        const auto grid = heights(dem);
        const int width = dem.dim + 2 * DEMData::border;
        for (const double interval : {200.0, 100.0, 20.0, 10.0}) {
            algorithm::contour::ContourThresholds thresholds;
            thresholds.interval = interval;
            thresholds.extent = static_cast<int>(std::lround(8192.0 * (width - 1) / dem.dim));
            const auto expected = algorithm::contour::generateContoursReference(grid, width, width, thresholds);
            const auto actual = algorithm::contour::generateContours(grid, width, width, thresholds);
            const std::string label = std::to_string(c.x) + "/" + std::to_string(c.y) + " at " +
                                      std::to_string(static_cast<int>(interval)) + " m";
            ASSERT_GT(expected.size(), 10u) << label;
            ASSERT_EQ(expected.size(), actual.size()) << label;
            for (std::size_t i = 0; i < expected.size(); i++) {
                ASSERT_EQ(expected[i].elevation, actual[i].elevation) << label << ", line " << i;
                ASSERT_EQ(expected[i].points, actual[i].points) << label << ", line " << i;
            }
        }
    }
}

namespace {

std::string contourStyle() {
    // The app's contour source and its z11 band layers, in opaque black so
    // that any moved line shows.
    auto band = [](const std::string& id, int minzoom, const std::string& filter) {
        return R"({"id": ")" + id + R"(", "type": "line", "source": "contours", "source-layer": "contours", "minzoom": )" +
               std::to_string(minzoom) + R"(, "filter": )" + filter +
               R"(, "paint": {"line-color": "black", "line-width": ["interpolate", ["linear"], ["zoom"], 9, 0.4, 15, 0.8]}})";
    };
    return R"({"version": 8, "sources": {
        "dem": {"type": "raster-dem", "tiles": ["http://dem/{z}/{x}/{y}"], "encoding": "terrarium", "tileSize": 512, "maxzoom": 12},
        "contours": {"type": "contour", "source": "dem", "intervals": [200, 10, 100, 11, 20, 12, 10], "unit": "meters",
                     "majorMultiplier": [1, 11, 5, 12, 10]}},
      "layers": [{"id": "background", "type": "background", "paint": {"background-color": "white"}},)" +
           band("band_200", 9, R"(["all", ["!=", ["get", "ele"], 0], ["==", ["%", ["get", "ele"], 200], 0]])") + "," +
           band("band_100",
                10,
                R"(["all", ["!=", ["get", "ele"], 0], ["==", ["%", ["get", "ele"], 100], 0], ["!=", ["%", ["get", "ele"], 200], 0]])") +
           "," +
           band("band_20", 11, R"(["all", ["==", ["%", ["get", "ele"], 20], 0], ["!=", ["%", ["get", "ele"], 100], 0]])") +
           "]}";
}

PremultipliedImage renderContours(double zoom) {
    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>();
    fileSource->tileResponse = [](const Resource& resource) -> std::optional<Response> {
        // http://dem/z/x/y; a tile outside the fixtures throws, and fails the test.
        std::string name = resource.url.substr(std::string("http://dem/").size());
        for (auto& ch : name) {
            if (ch == '/') ch = '_';
        }
        Response response;
        response.data = std::make_shared<std::string>(util::read_file(kFixtures + name + ".webp"));
        return response;
    };
    HeadlessFrontend frontend{Size{320, 320}, 2.0f};
    MapAdapter map{frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()).withPixelRatio(2.0f)};
    map.getStyle().loadJSON(contourStyle());
    // The corner that 11/609/1341 shares with 610/1341, 609/1342 and 610/1342,
    // so the view holds four tiles and their seams.
    const double n = std::pow(2.0, 11);
    const double lon = 610.0 / n * 360.0 - 180.0;
    const double lat = std::atan(std::sinh(M_PI * (1.0 - 2.0 * 1342.0 / n))) * 180.0 / M_PI;
    map.jumpTo(CameraOptions().withCenter(LatLng{lat, lon}).withZoom(zoom));
    return frontend.render(map).image;
}

} // namespace

// The contour lines of nine real tiles draw exactly as they did before the
// contour work was made cheaper (expected images from fork 52b261a0e374).
// MLN_CONTOUR_UPDATE=1 writes new expected images.
TEST(ContourLines, TheRenderMatchesTheExpectedImage) {
    for (const auto& [zoom, name] : {std::pair{11.0, "z11"}, std::pair{11.4, "z11.4"}}) {
        const auto actual = renderContours(zoom);
        const std::string path = kFixtures + "expected-" + name + ".png";
        if (std::getenv("MLN_CONTOUR_UPDATE")) {
            util::write_file(path, encodePNG(actual));
            continue;
        }
        const auto expected = decodeImage(util::read_file(path));
        ASSERT_EQ(expected.size, actual.size) << name;
        std::size_t differing = 0;
        std::size_t dark = 0;
        for (std::size_t i = 0; i < actual.bytes(); i += 4) {
            if (std::memcmp(expected.data.get() + i, actual.data.get() + i, 4) != 0) differing++;
            if (actual.data[i] < 128) dark++;
        }
        // The image holds contour lines, not a blank map.
        EXPECT_GT(dark, actual.bytes() / 4 / 1000) << name;
        EXPECT_EQ(0u, differing) << name << ": " << differing << " pixels differ";
    }
}
