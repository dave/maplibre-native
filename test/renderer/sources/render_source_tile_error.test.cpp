#include <mln/test/map_adapter.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/util.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/map_observer.hpp>
#include <mln/map/map_options.hpp>
#include <mln/style/style.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/timer.hpp>

#include <functional>
#include <optional>
#include <string>

using namespace mln;

namespace {

constexpr auto kBound = std::chrono::seconds(30);

/** Runs the loop until `done()`. False, with a failure, if it has not come within the bound. */
bool runUntil(util::RunLoop& loop, const std::function<bool()>& done, const std::string& what) {
    bool late = false;
    util::Timer watchdog;
    watchdog.start(kBound, Duration::zero(), [&] { late = true; });
    while (!done() && !late) {
        loop.runOnce();
    }
    EXPECT_FALSE(late) << what << ": not within " << kBound.count() << " s";
    return !late;
}

class FrameCounter final : public MapObserver {
public:
    void onDidFinishRenderingFrame(const RenderFrameStatus&) override { ++frames; }
    int frames = 0;
};

/// One vector source with one tile in view, whose answer the test holds
/// until it chooses, on a continuous map that renders only when invalidated.
class OneTileMap {
public:
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>();
    FrameCounter observer;
    HeadlessFrontend frontend{Size{256, 256}, 1.0f};
    MapAdapter map{frontend,
                   observer,
                   fileSource,
                   MapOptions().withMapMode(MapMode::Continuous).withSize(frontend.getSize()).withPixelRatio(1.0)};

    /// Times the stub asked for the tile's answer (once per millisecond while it is held).
    int asked = 0;
    /// The answer to give once, when set; the stub then holds again.
    std::optional<Response> answer;
    bool answered = false;

    OneTileMap() {
        fileSource->tileResponse = [this](const Resource&) -> std::optional<Response> {
            ++asked;
            if (!answer) return std::nullopt;
            auto given = std::move(answer);
            answer.reset();
            answered = true;
            return given;
        };
        map.getStyle().loadJSON(R"({
            "version": 8,
            "sources": {"items": {"type": "vector", "tiles": ["http://tiles/{z}/{x}/{y}.pbf"], "maxzoom": 0}},
            "layers": [{"id": "fill", "type": "fill", "source": "items", "source-layer": "water"}]})");
        map.jumpTo(CameraOptions().withCenter(LatLng{0, 0}).withZoom(0));
    }

    /// The tile is requested and the map has stopped rendering: the stub has
    /// asked for the held answer 20 more times with no frame in between.
    bool still() {
        if (!runUntil(loop, [&] { return asked > 0 && observer.frames > 0; }, "the tile's request and a frame")) {
            return false;
        }
        int frames = observer.frames;
        int from = asked;
        return runUntil(
            loop,
            [&] {
                if (observer.frames != frames) {
                    frames = observer.frames;
                    from = asked;
                }
                return asked - from >= 20;
            },
            "a still map");
    }

    /// Gives `response` to the held tile request, and says whether a frame
    /// follows it.
    bool frameAfter(Response response) {
        answer = std::move(response);
        if (!runUntil(loop, [&] { return answered; }, "the answer")) return false;
        const int before = observer.frames;
        return runUntil(loop, [&] { return observer.frames > before; }, "a frame after the tile's answer");
    }
};

} // namespace

// A tile that ends in an error is complete, so the map has something new to
// say (it is loaded, and idle): the error must ask for a frame, as a tile that
// loads does. Before, `RenderOrchestrator::onTileError` only logged, and a
// still map whose last tile failed rendered nothing more: no frame, no
// "fully rendered", no idle (Viatrail's 2026-10-09 stall: 47 frames, then 3.6
// s of silence, until the app's 5 s cap).
TEST(RenderSourceTileError, AStillMapWhoseLastTileFailsToLoadGetsAFrame) {
    OneTileMap test;
    ASSERT_TRUE(test.still());
    Response response;
    response.error = std::make_unique<Response::Error>(Response::Error::Reason::Server, "a failed tile");
    EXPECT_TRUE(test.frameAfter(std::move(response)));
}

// The same for a tile whose body does not parse (the worker's error).
TEST(RenderSourceTileError, AStillMapWhoseLastTileFailsToParseGetsAFrame) {
    OneTileMap test;
    ASSERT_TRUE(test.still());
    Response response;
    response.data = std::make_shared<std::string>("not a vector tile");
    EXPECT_TRUE(test.frameAfter(std::move(response)));
}

// The control: a tile that loads gets its frame (`onTileChanged`).
TEST(RenderSourceTileError, AStillMapWhoseLastTileLoadsGetsAFrame) {
    OneTileMap test;
    ASSERT_TRUE(test.still());
    Response response;
    response.noContent = true;
    EXPECT_TRUE(test.frameAfter(std::move(response)));
}
