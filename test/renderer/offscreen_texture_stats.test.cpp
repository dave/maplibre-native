#include <mln/test/util.hpp>
#include <mln/test/map_adapter.hpp>
#include <mln/test/stub_file_source.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/map_observer.hpp>
#include <mln/map/map_options.hpp>
#include <mln/style/style.hpp>
#include <mln/util/run_loop.hpp>

using namespace mln;

// `RenderingStats::numFrameBuffers` is the number of live offscreen frame
// buffers. A headless frontend renders into one, so after a frame of a map
// with no render targets the count is one.
TEST(OffscreenTexture, AHeadlessFrameCountsItsFrameBuffer) {
    util::RunLoop loop;
    HeadlessFrontend frontend{Size{16, 16}, 1.0f};
    MapAdapter map{frontend,
                   MapObserver::nullObserver(),
                   std::make_shared<StubFileSource>(),
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()).withPixelRatio(1.0)};
    map.getStyle().loadJSON(R"({
        "version": 8,
        "sources": {},
        "layers": [{"id": "background", "type": "background", "paint": {"background-color": "white"}}]})");

    EXPECT_EQ(1, frontend.render(map).stats.numFrameBuffers);
}
