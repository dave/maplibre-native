// Contour lines from real DEM tiles (test/fixtures/contour, see its README).

#include <mln/test/map_adapter.hpp>
#include <mln/test/stub_file_source.hpp>

#include <mln/geometry/dem_data.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/map_options.hpp>
#include <mln/style/style.hpp>
#include <mln/tile/contour_tile.hpp>
#include <mln/util/constants.hpp>
#include <mln/util/image.hpp>
#include <mln/util/io.hpp>
#include <mln/util/run_loop.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The fixtures are WebP DEM tiles. WebP support is not enabled in Qt by
// default (see Image.WebPTile in test/util/image.test.cpp).
#if !defined(__QT__)

using namespace mln;

namespace {

const std::string kFixtures = "test/fixtures/contour/";

PremultipliedImage loadTile(int z, int x, int y) {
    return decodeImage(
        util::read_file(kFixtures + std::to_string(z) + "_" + std::to_string(x) + "_" + std::to_string(y) + ".webp"));
}

struct Line {
    double elevation;
    mapbox::geometry::line_string<std::int16_t> points;
};

// The contour lines of one fixture tile, as a contour source builds them: from
// its DEM tile with the borders backfilled from the fixture tiles around it.
std::vector<Line> tileLines(int x, int y, double interval) {
    DEMData dem(loadTile(11, x, y), Tileset::RasterEncoding::Terrarium);
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            const int nx = x + dx;
            const int ny = y + dy;
            if ((dx == 0 && dy == 0) || nx < 608 || nx > 610 || ny < 1340 || ny > 1342) continue;
            const DEMData neighbour(loadTile(11, nx, ny), Tileset::RasterEncoding::Terrarium);
            dem.backfillBorder(neighbour, static_cast<int8_t>(dx), static_cast<int8_t>(dy));
        }
    }
    const ContourTile::DEMSnapshot snapshot{
        .image = std::make_shared<const PremultipliedImage>(dem.getImage()->clone()),
        .dim = dem.dim,
        .unpack = dem.getUnpackVector()};
    std::vector<Line> result;
    for (const auto& feature :
         ContourTile::buildFeatures(snapshot, {.intervalDisplayUnits = interval, .majorMultiplier = 5, .unit = {}})) {
        result.push_back({static_cast<double>(feature.properties.at("ele").get<std::int64_t>()),
                          feature.geometry.get<mapbox::geometry::line_string<std::int16_t>>()});
    }
    return result;
}

} // namespace

namespace {

std::string contourStyle() {
    // The app's contour source and its z11 band layers, in opaque black so
    // that any moved line shows.
    auto band = [](const std::string& id, int minzoom, const std::string& filter) {
        return R"({"id": ")" + id +
               R"(", "type": "line", "source": "contours", "source-layer": "contours", "minzoom": )" +
               std::to_string(minzoom) + R"(, "filter": )" + filter +
               R"(, "paint": {"line-color": "black", "line-width": ["interpolate", ["linear"], ["zoom"], 9, 0.4, 15, 0.8]}})";
    };
    return R"({"version": 8, "sources": {
        "dem": {"type": "raster-dem", "tiles": ["http://dem/{z}/{x}/{y}"], "encoding": "terrarium", "tileSize": 512, "maxzoom": 12},
        "contours": {"type": "contour", "source": "dem", "intervals": [200, 10, 100, 11, 20, 12, 10], "unit": "meters",
                     "majorMultiplier": [1, 11, 5, 12, 10]}},
      "layers": [{"id": "background", "type": "background", "paint": {"background-color": "white"}},)" +
           band("band_200", 9, R"(["all", ["!=", ["get", "ele"], 0], ["==", ["%", ["get", "ele"], 200], 0]])") + "," +
           band(
               "band_100",
               10,
               R"(["all", ["!=", ["get", "ele"], 0], ["==", ["%", ["get", "ele"], 100], 0], ["!=", ["%", ["get", "ele"], 200], 0]])") +
           "," +
           band("band_20",
                11,
                R"(["all", ["==", ["%", ["get", "ele"], 20], 0], ["!=", ["%", ["get", "ele"], 100], 0]])") +
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

// Where a line leaves a tile, the same line starts in the neighbour, at the same
// point on the shared edge. The two tiles build their lines on their own, so this
// holds only if each reads the same heights across the edge.
TEST(ContourLines, LinesMeetAcrossATileBoundary) {
    constexpr std::int16_t extent = util::EXTENT;
    // A line end on one edge of a tile: its elevation and where along the edge.
    using End = std::pair<double, std::int16_t>;
    const auto endsOnEdge = [](const std::vector<Line>& lines, bool vertical, std::int16_t edge) {
        std::vector<End> ends;
        for (const auto& line : lines) {
            if (line.points.front() == line.points.back()) continue; // A ring has no ends.
            for (const auto& point : {line.points.front(), line.points.back()}) {
                if ((vertical ? point.x : point.y) == edge) {
                    ends.emplace_back(line.elevation, vertical ? point.y : point.x);
                }
            }
        }
        return ends;
    };

    // The boundaries in the block whose tiles have both their shared borders.
    struct Boundary {
        int ax, ay, bx, by;
        bool vertical;
    };
    std::size_t matched = 0;
    std::size_t unmatched = 0;
    for (const auto& boundary : {Boundary{608, 1341, 609, 1341, true},
                                 Boundary{609, 1341, 610, 1341, true},
                                 Boundary{609, 1340, 609, 1341, false},
                                 Boundary{609, 1341, 609, 1342, false}}) {
        for (const double interval : {100.0, 20.0, 10.0}) {
            const auto first = endsOnEdge(tileLines(boundary.ax, boundary.ay, interval), boundary.vertical, extent);
            const auto second = endsOnEdge(tileLines(boundary.bx, boundary.by, interval), boundary.vertical, 0);
            for (const auto& [elevation, along] : first) {
                const auto match = std::ranges::find_if(second, [&](const End& end) {
                    return end.first == elevation && std::abs(end.second - along) <= 1;
                });
                (match == second.end() ? unmatched : matched)++;
            }
        }
    }
    EXPECT_GT(matched, 50u);
    EXPECT_EQ(0u, unmatched);
}

// Nine real tiles draw contour lines at the app's zooms.
TEST(ContourLines, RealTilesDrawLines) {
    for (const double zoom : {11.0, 11.4}) {
        const auto image = renderContours(zoom);
        std::size_t dark = 0;
        for (std::size_t i = 0; i < image.bytes(); i += 4) {
            if (image.data[i] < 128) dark++;
        }
        EXPECT_GT(dark, image.bytes() / 4 / 1000) << zoom;
    }
}

#endif // !defined(__QT__)
