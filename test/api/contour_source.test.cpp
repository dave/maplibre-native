#include <mln/test/map_adapter.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/stub_map_observer.hpp>
#include <mln/test/util.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/map_options.hpp>
#include <mln/style/sources/raster_dem_source.hpp>
#include <mln/style/style.hpp>
#include <mln/util/image.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/timer.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>

using namespace mln;

namespace {

// A cone of 1000 m on a terrarium tile, so that it has contour lines.
std::string coneTile() {
    constexpr int dim = 64;
    PremultipliedImage image({dim, dim});
    for (int y = 0; y < dim; y++) {
        for (int x = 0; x < dim; x++) {
            const double height = 1000.0 - 30.0 * std::hypot(x - dim / 2.0, y - dim / 2.0);
            const double value = height + 32768.0;
            uint8_t* pixel = image.data.get() + static_cast<std::size_t>(y * dim + x) * 4;
            pixel[0] = static_cast<uint8_t>(std::floor(value / 256.0));
            pixel[1] = static_cast<uint8_t>(std::floor(value)) % 256;
            pixel[2] = static_cast<uint8_t>((value - std::floor(value)) * 256.0);
            pixel[3] = 255;
        }
    }
    return encodePNG(image);
}

std::string contourStyle(const std::string& demProperties = "") {
    return R"({
        "version": 8,
        "sources": {
            "dem": {"type": "raster-dem", "encoding": "terrarium", "tileSize": 512, "maxzoom": 2,
                    "tiles": ["dem://{z}/{x}/{y}"])" +
           demProperties + R"(},
            "contours": {"type": "contour", "source": "dem", "intervals": [100]}
        },
        "layers": [
            {"id": "background", "type": "background", "paint": {"background-color": "white"}},
            {"id": "lines", "type": "line", "source": "contours", "paint": {"line-color": "black"}}
        ]
    })";
}

class ContourMap {
public:
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>();
    StubMapObserver observer;
    HeadlessFrontend frontend{{256, 256}, 1.0f};
    MapAdapter map;
    // Ends a run that would otherwise never return: a hang is this test's
    // failure, and a hung test takes the whole run with it.
    util::Timer watchdog;

    explicit ContourMap(MapMode mode)
        : map(frontend, observer, fileSource, MapOptions().withMapMode(mode).withSize(frontend.getSize())) {
        watchdog.start(std::chrono::minutes(2), Duration::zero(), [] {
            ADD_FAILURE() << "The map did not finish loading";
            std::abort();
        });
        map.jumpTo(CameraOptions().withCenter(LatLng{0.0, 0.0}).withZoom(2.0));
    }
};

std::size_t darkPixels(const PremultipliedImage& image) {
    std::size_t dark = 0;
    for (std::size_t i = 0; i < image.bytes(); i += 4) {
        if (image.data[i] < 128) dark++;
    }
    return dark;
}

} // namespace

// Tiles over the ocean have no content. A still render must finish, and the
// tiles that do have data must draw.
TEST(ContourSource, AStillRenderFinishesWhenSomeDEMTilesHaveNoContent) {
    ContourMap test(MapMode::Static);
    const std::string cone = coneTile();
    test.fileSource->tileResponse = [&](const Resource& resource) -> std::optional<Response> {
        Response response;
        if (resource.tileData->x == 1 && resource.tileData->y == 1) {
            response.data = std::make_shared<std::string>(cone);
        } else {
            response.noContent = true;
        }
        return response;
    };
    test.map.getStyle().loadJSON(contourStyle());

    const auto image = test.frontend.render(test.map).image;

    EXPECT_GT(darkPixels(image), 0u);
}

// A view outside the DEM's bounds: the DEM source asks for no tiles.
TEST(ContourSource, AStillRenderFinishesOutsideTheDEMBounds) {
    ContourMap test(MapMode::Static);
    int served = 0;
    test.fileSource->tileResponse = [&](const Resource&) -> std::optional<Response> {
        served++;
        Response response;
        response.noContent = true;
        return response;
    };
    test.map.jumpTo(CameraOptions().withCenter(LatLng{-30.0, -60.0}).withZoom(2.0));
    test.map.getStyle().loadJSON(contourStyle(R"(, "bounds": [100, 60, 110, 70])"));

    const auto image = test.frontend.render(test.map).image;

    EXPECT_EQ(0, served);
    EXPECT_EQ(0u, darkPixels(image));
}

// A failed tile fails a still render by design (Map::Impl::onResourceError), so
// the failure case is a continuous map, which must still finish loading.
TEST(ContourSource, AContinuousMapFinishesLoadingWhenADEMTileFails) {
    ContourMap test(MapMode::Continuous);
    test.fileSource->tileResponse = [&](const Resource&) -> std::optional<Response> {
        Response response;
        response.error = std::make_unique<Response::Error>(Response::Error::Reason::Server, "no DEM today");
        return response;
    };
    test.observer.didFinishLoadingMapCallback = [&] {
        test.loop.stop();
    };
    test.map.getStyle().loadJSON(contourStyle());

    test.loop.run();

    SUCCEED();
}

// Offline, a DEM tile answers with no content and its contour tile completes with no
// lines. Back online, the app asks the raster-dem source for its empty tiles again. A
// contour source has no tile API of its own: its lines come from the DEM tile that
// lands, with no pan or zoom.
TEST(ContourSource, LinesAppearWhenAnEmptyDEMTileLandsAfterAReload) {
    ContourMap test(MapMode::Continuous);
    const std::string cone = coneTile();
    bool online = false;
    int emptyAnswers = 0;
    test.fileSource->tileResponse = [&](const Resource&) -> std::optional<Response> {
        Response response;
        if (online) {
            response.data = std::make_shared<std::string>(cone);
        } else {
            response.noContent = true;
            emptyAnswers++;
        }
        return response;
    };
    test.observer.didFinishLoadingMapCallback = [&] {
        test.loop.stop();
    };
    test.map.getStyle().loadJSON(contourStyle());
    test.loop.run();
    ASSERT_GT(emptyAnswers, 0);
    EXPECT_EQ(0u, darkPixels(test.frontend.readStillImage())) << "no DEM data, no lines";

    online = true;
    test.map.getStyle().getSource("dem")->as<style::RasterDEMSource>()->reloadEmptyTiles();

    // Each frame that follows looks for the lines. The watchdog ends a run with none.
    bool drawn = false;
    test.observer.didFinishRenderingFrameCallback = [&](MapObserver::RenderFrameStatus) {
        if (!drawn && darkPixels(test.frontend.readStillImage()) > 0) {
            drawn = true;
            test.loop.stop();
        }
    };
    test.loop.run();

    EXPECT_TRUE(drawn);
}
