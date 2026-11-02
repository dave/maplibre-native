// Programmatic seam-detection test for hillshade rendering at overzoom.
//
// The earlier render-test approach (a fixed expected.png) can detect a
// regression only relative to a previously-captured golden image. This
// test instead asks the rendered output a direct question: do tile
// boundaries produce a visible discontinuity?
//
// Setup. The four z=12 terrain tiles (758,1608), (759,1608),
// (758,1609) and (759,1609) are loaded via a StubFileSource and a
// raster-dem source with maxzoom=12. The camera sits exactly at the
// world corner where those four tiles meet, at display zoom 13. Each
// tile is therefore overzoomed by one level, and the four-tile corner
// projects to the centre of the viewport — pixel (W/2, H/2). The
// horizontal tile boundary runs along screen row H/2; the vertical
// boundary runs along screen column W/2.
//
// Measurement. We compute the mean per-pixel absolute luminance
// difference between the row immediately above and the row immediately
// below the centre line, and the mean of the same difference over many
// control rows that are not aligned with any tile boundary. A seam
// shows up as the centre-row diff being much larger than the control
// mean. We accept the rendering as seam-free when the centre diff is
// no more than 1.25 times the control mean. The same test runs for the
// vertical boundary along column W/2.
//
// A blank image has no seam either, so the test first checks that the
// hillshade was drawn: the fixtures must load, and the image must show
// the terrain's shading.
//
// Test data. The four required terrain.png tiles already live under
// metrics/integration/tiles/ for the render-test integration suite;
// we read them directly from disk via StubFileSource.

#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/fake_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/context.hpp>
#include <mln/gfx/renderer_backend.hpp>
#include <mln/map/map_observer.hpp>
#include <mln/map/map_options.hpp>
#include <mln/style/style.hpp>
#include <mln/util/image.hpp>
#include <mln/util/io.hpp>
#include <mln/util/run_loop.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace mln;

namespace {

// Style centred on the corner where the four z=12 terrain tiles meet,
// rendered at display zoom 13 (one level of overzoom relative to the
// source's maxzoom of 12). Same camera as the
// `hillshade-overzoom-seam` render-test for direct cross-reference.
constexpr const char* kStyleJSON = R"({
  "version": 8,
  "center": [-113.291016, 35.960223],
  "zoom": 13,
  "sources": {
    "terrain": {
      "type": "raster-dem",
      "tiles": ["http://tiles/{z}/{x}/{y}.terrain.png"],
      "maxzoom": 12,
      "tileSize": 256
    }
  },
  "layers": [
    { "id": "bg", "type": "background", "paint": { "background-color": "white" } },
    {
      "id": "hs", "type": "hillshade", "source": "terrain",
      "paint": {
        "hillshade-shadow-color": "#000000",
        "hillshade-highlight-color": "#ffffff",
        "hillshade-accent-color": "#000000",
        "hillshade-exaggeration": 1.0
      }
    }
  ]
})";

// Read the raw terrain tile bytes from the integration-tile fixtures.
std::string readTerrainTile(uint32_t x, uint32_t y) {
    std::string path = "metrics/integration/tiles/12-" + std::to_string(x) + "-" + std::to_string(y) + ".terrain.png";
    return util::read_file(path);
}

// Mean per-pixel absolute luminance difference between two pixel rows
// of the rendered RGBA image. Luminance is the unweighted average of
// the R, G, B channels (the hillshade output is grayscale before any
// shadow/highlight tint, so a simple mean is sufficient).
double meanRowDiff(const PremultipliedImage& img, int y0, int y1) {
    const int w = static_cast<int>(img.size.width);
    const auto* data = img.data.get();
    double sum = 0.0;
    for (int x = 0; x < w; ++x) {
        const std::size_t i0 = (static_cast<std::size_t>(y0) * w + x) * 4;
        const std::size_t i1 = (static_cast<std::size_t>(y1) * w + x) * 4;
        const double lum0 = (data[i0] + data[i0 + 1] + data[i0 + 2]) / 3.0;
        const double lum1 = (data[i1] + data[i1 + 1] + data[i1 + 2]) / 3.0;
        sum += std::abs(lum0 - lum1);
    }
    return sum / w;
}

// A drawn hillshade of these tiles has a mean adjacent-row (and
// -column) luminance diff well above this. A blank or flat image has 0.
constexpr double kMinControlDiff = 1.0;
// A pixel darker than this is not the white background.
constexpr double kShadedLuminance = 240.0;

double meanColDiff(const PremultipliedImage& img, int x0, int x1) {
    const int w = static_cast<int>(img.size.width);
    const int h = static_cast<int>(img.size.height);
    const auto* data = img.data.get();
    double sum = 0.0;
    for (int y = 0; y < h; ++y) {
        const std::size_t i0 = (static_cast<std::size_t>(y) * w + x0) * 4;
        const std::size_t i1 = (static_cast<std::size_t>(y) * w + x1) * 4;
        const double lum0 = (data[i0] + data[i0 + 1] + data[i0 + 2]) / 3.0;
        const double lum1 = (data[i1] + data[i1 + 1] + data[i1 + 2]) / 3.0;
        sum += std::abs(lum0 - lum1);
    }
    return sum / h;
}

class HillshadeSeamTest {
public:
    util::RunLoop loop;
    HeadlessFrontend frontend{Size{256, 256}, 1.0};
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>();
    MapAdapter map{frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()).withPixelRatio(1.0)};

    HillshadeSeamTest() {
        // Read the four z=12 corner tiles before the map asks for them.
        // util::read_file throws when a fixture is missing (for example,
        // when the test runs from the wrong directory), so a missing
        // fixture fails the test instead of rendering a blank image.
        std::map<std::pair<uint32_t, uint32_t>, std::shared_ptr<const std::string>> tiles;
        for (const uint32_t x : {758u, 759u}) {
            for (const uint32_t y : {1608u, 1609u}) {
                tiles[{x, y}] = std::make_shared<const std::string>(readTerrainTile(x, y));
            }
        }

        // Every tile request gets a synchronous response. `std::nullopt`
        // would leave the request pending in StubFileSource's async path,
        // and `render()` would wait for a tile that never resolves.
        fileSource->tileResponse = [tiles = std::move(tiles)](const Resource& res) -> std::optional<Response> {
            static const std::regex urlRe{R"(/(\d+)/(\d+)/(\d+)\.terrain\.png$)"};
            Response r;
            std::smatch m;
            if (!std::regex_search(res.url, m, urlRe)) {
                ADD_FAILURE() << "unexpected tile URL " << res.url;
                r.noContent = true;
                return r;
            }
            const uint32_t z = static_cast<uint32_t>(std::stoul(m[1].str()));
            const uint32_t x = static_cast<uint32_t>(std::stoul(m[2].str()));
            const uint32_t y = static_cast<uint32_t>(std::stoul(m[3].str()));
            // The source has data only for the four z=12 corner tiles. The
            // renderer overzooms them, so other zoom levels have no data.
            if (z != 12) {
                r.noContent = true;
                return r;
            }
            const auto it = tiles.find({x, y});
            if (it == tiles.end()) {
                ADD_FAILURE() << "unexpected z=12 tile " << x << "/" << y;
                r.noContent = true;
                return r;
            }
            r.data = it->second;
            return r;
        };
    }
};

} // namespace

// Re-renders the same scene a few times via the headless frontend so
// neighbour-tile backfilling has finished propagating through the
// prepare pass before we sample the final image. The first render
// often produces a frame in which only some tiles have had their
// borders backfilled; the seam analysis only makes sense once all
// four tiles have been re-prepared with their final borders.
TEST(HillshadeRendering, NoOverzoomCornerSeam) {
    HillshadeSeamTest test;
    test.map.getStyle().loadJSON(kStyleJSON);

    PremultipliedImage img;
    for (int i = 0; i < 4; ++i) {
        img = test.frontend.render(test.map).image;
    }

    ASSERT_FALSE(img.size.isEmpty());
    const int w = static_cast<int>(img.size.width);
    const int h = static_cast<int>(img.size.height);
    ASSERT_EQ(w, 256);
    ASSERT_EQ(h, 256);

    // Build a control baseline as the mean of many non-boundary row
    // pair diffs (and column pair diffs). Averaging across many
    // samples washes out any single row's terrain-aligned coincidence
    // and gives a stable estimate of "how different adjacent rows
    // look in this rendering as a whole." A seam shows up as a single
    // boundary-row diff that's an outlier compared to that mean.
    //
    // Skip a small band around the centre line so we don't accidentally
    // include the seam itself in the control samples.
    const int cy = h / 2;
    const int cx = w / 2;
    const int skip = 8;

    auto meanRowDiffControls = [&]() {
        double sum = 0;
        int count = 0;
        for (int y = 16; y < h - 16; y += 4) {
            if (std::abs(y - cy) < skip) continue;
            sum += meanRowDiff(img, y - 1, y + 1);
            ++count;
        }
        return sum / count;
    };
    auto meanColDiffControls = [&]() {
        double sum = 0;
        int count = 0;
        for (int x = 16; x < w - 16; x += 4) {
            if (std::abs(x - cx) < skip) continue;
            sum += meanColDiff(img, x - 1, x + 1);
            ++count;
        }
        return sum / count;
    };

    const double seamH = meanRowDiff(img, cy - 1, cy + 1);
    const double ctrlH = meanRowDiffControls();
    const double seamV = meanColDiff(img, cx - 1, cx + 1);
    const double ctrlV = meanColDiffControls();

    // A blank or flat image has no seam, so check first that the
    // hillshade was drawn: most pixels of this steep terrain are darker
    // than the white background, and adjacent rows and columns differ.
    std::size_t shadedPixels = 0;
    for (std::size_t i = 0; i < img.bytes(); i += 4) {
        const double lum = (img.data[i] + img.data[i + 1] + img.data[i + 2]) / 3.0;
        if (lum < kShadedLuminance) ++shadedPixels;
    }
    const std::size_t pixelCount = img.bytes() / 4;
    ASSERT_GT(shadedPixels, pixelCount / 2)
        << "the hillshade was not drawn: " << shadedPixels << " of " << pixelCount << " pixels are shaded";
    ASSERT_GT(ctrlH, kMinControlDiff) << "the hillshade has no shading across rows: control mean=" << ctrlH;
    ASSERT_GT(ctrlV, kMinControlDiff) << "the hillshade has no shading across columns: control mean=" << ctrlV;

    // 1.25× headroom over the typical adjacent-row diff is tight
    // enough to detect the pre-fix horizontal seam (which sits
    // around 1.4× of the mean control) and the pre-fix vertical
    // seam (around 1.8×), while leaving comfortable margin for the
    // fixed renderer where the boundary row is actually quieter
    // than the average row (<1.0× of control).
    EXPECT_LE(seamH, 1.25 * ctrlH) << "horizontal hillshade seam at y=" << cy << ": diff=" << seamH
                                   << ", control mean=" << ctrlH;
    EXPECT_LE(seamV, 1.25 * ctrlV) << "vertical hillshade seam at x=" << cx << ": diff=" << seamV
                                   << ", control mean=" << ctrlV;
}

namespace {

/// A continuous map with one raster-dem source of four z1 tiles and a
/// hillshade layer. It renders only when the test asks, and each tile's
/// body arrives only when the test answers its request, so the test
/// chooses the order of tile loads and frames.
class TileByTileHillshadeMap {
public:
    class ParseObserver final : public MapObserver {
    public:
        void onTileAction(TileOperation op, const OverscaledTileID& id, const std::string&) override {
            if (op == TileOperation::EndParse) parsed.insert(id.canonical);
        }
        std::set<CanonicalTileID> parsed;
    };

    static std::vector<CanonicalTileID> tiles() {
        return {CanonicalTileID{1, 0, 0}, CanonicalTileID{1, 1, 0}, CanonicalTileID{1, 0, 1}, CanonicalTileID{1, 1, 1}};
    }

    util::RunLoop loop;
    std::shared_ptr<FakeFileSource> fileSource = std::make_shared<FakeFileSource>();
    ParseObserver observer;
    HeadlessFrontend frontend{Size{500, 500},
                              1.0f,
                              gfx::HeadlessBackend::SwapBehaviour::NoFlush,
                              gfx::ContextMode::Unique,
                              std::nullopt,
                              /*invalidateOnUpdate=*/false};
    MapAdapter map{frontend,
                   observer,
                   fileSource,
                   MapOptions().withMapMode(MapMode::Continuous).withSize(frontend.getSize()).withPixelRatio(1.0)};

    TileByTileHillshadeMap() {
        // 256 px tiles at zoom 0 cover the 512 px world with the four z1
        // tiles, and the 500 px view shows no other copy of the world.
        map.getStyle().loadJSON(R"({
            "version": 8,
            "center": [0, 0],
            "zoom": 0,
            "sources": {"dem": {"type": "raster-dem", "tiles": ["http://tiles/{z}/{x}/{y}.png"],
                                "tileSize": 256, "minzoom": 1, "maxzoom": 1}},
            "layers": [{"id": "hillshade", "type": "hillshade", "source": "dem"}]})");
        // The first frame requests the tiles.
        while (!frontend.getTransformState()) {
            loop.runOnce();
        }
        frame();
    }

    void frame() { frontend.renderFrame(); }

    /// Answers the tile's request and runs the loop until the tile has
    /// parsed (and has been backfilled from, and into, its loaded neighbours).
    void answer(const CanonicalTileID& id) {
        const std::string url = "http://tiles/" + std::to_string(id.z) + "/" + std::to_string(id.x) + "/" +
                                std::to_string(id.y) + ".png";
        const auto it = std::ranges::find_if(fileSource->requests,
                                             [&](const auto* request) { return request->resource.url == url; });
        ASSERT_NE(it, fileSource->requests.end()) << "no request for " << url;
        PremultipliedImage image({8, 8});
        for (std::size_t i = 0; i < image.bytes(); i += 4) {
            image.data[i + 0] = 1;
            image.data[i + 1] = static_cast<uint8_t>(id.x * 64 + id.y * 32);
            image.data[i + 2] = 0;
            image.data[i + 3] = 255;
        }
        Response response;
        response.data = std::make_shared<const std::string>(encodePNG(image));
        // Copy the callback: calling it frees the request.
        const auto callback = (*it)->callback;
        callback(response);
        while (!observer.parsed.contains(id)) {
            loop.runOnce();
        }
    }

    /// Moves the view into the first z1 tile and zooms in. The source stops at z1, so
    /// one overscaled tile, a new tile for the pyramid, covers the view. It answers
    /// that tile's request.
    void zoomIntoOneTile() {
        map.jumpTo(CameraOptions().withCenter(LatLng{45.0, -90.0}).withZoom(3.0));
        frame();
        observer.parsed.clear();
        answer({1, 0, 0});
        frame();
    }

    int frameBuffers() {
        gfx::BackendScope scope{*frontend.getBackend()};
        return frontend.getBackend()->getContext().renderingStats().numFrameBuffers;
    }
};

} // namespace

// Each hillshade tile renders its prepare pass into a render target. A
// neighbour that loads later backfills the tile's border, and the tile
// prepares again into a new render target. The old one must be freed:
// before, it stayed in the layer's and the orchestrator's lists (and was
// drawn every frame) until the layer stopped rendering.
TEST(HillshadeRendering, APrepareAgainFreesTheOldRenderTarget) {
    // The four tiles load in four frames: every tile but the last prepares
    // again when its neighbours load.
    TileByTileHillshadeMap oneByOne;
    const int noTiles = oneByOne.frameBuffers();
    for (const auto& id : TileByTileHillshadeMap::tiles()) {
        oneByOne.answer(id);
        oneByOne.frame();
    }
    oneByOne.frame();

    // The four tiles load before one frame: each tile prepares once.
    TileByTileHillshadeMap allAtOnce;
    for (const auto& id : TileByTileHillshadeMap::tiles()) {
        allAtOnce.answer(id);
    }
    allAtOnce.frame();
    allAtOnce.frame();

    EXPECT_EQ(noTiles + 4, allAtOnce.frameBuffers());
    EXPECT_EQ(noTiles + 4, oneByOne.frameBuffers()) << "one render target for each tile";
}

// A tile that leaves the view stops being prepared, and with no cache to keep it, its
// render target is freed. Before, the layer kept it, and drew it every frame, until the
// layer stopped rendering.
TEST(HillshadeRendering, ATileThatLeavesFreesItsRenderTarget) {
    TileByTileHillshadeMap test;
    test.frontend.getRenderer()->setTileCacheEnabled(false);
    const int noTiles = test.frameBuffers();
    for (const auto& id : TileByTileHillshadeMap::tiles()) {
        test.answer(id);
    }
    test.frame();
    test.frame();
    ASSERT_EQ(noTiles + 4, test.frameBuffers());

    test.zoomIntoOneTile();
    test.frame();
    test.frame();

    EXPECT_EQ(noTiles + 1, test.frameBuffers()) << "one render target for the one tile in view";
}

// A tile that returns from the cache has lost its place among the render targets,
// so it prepares again and draws.
TEST(HillshadeRendering, ATileBackFromTheCacheHasARenderTarget) {
    TileByTileHillshadeMap test;
    const int noTiles = test.frameBuffers();
    for (const auto& id : TileByTileHillshadeMap::tiles()) {
        test.answer(id);
    }
    test.frame();
    test.frame();
    const int loaded = test.frameBuffers();
    ASSERT_EQ(noTiles + 4, loaded);

    test.zoomIntoOneTile();
    test.frame();
    test.frame();

    test.map.jumpTo(CameraOptions().withCenter(LatLng{0.0, 0.0}).withZoom(0.0));
    test.frame();
    test.frame();

    // The four tiles are back, each with one render target to draw. Any more would be
    // a target that is in no tile's bucket; the zoomed tile's is in the cache.
    EXPECT_LE(test.frameBuffers(), noTiles + 5);
    EXPECT_GE(test.frameBuffers(), noTiles + 4);
}
