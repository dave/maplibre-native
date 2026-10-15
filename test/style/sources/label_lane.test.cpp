// The label lane (GeoJSONOptions::labelLane): a GeoJSON source whose off-thread
// work runs on a lane of its own, not on the shared worker pool, so that its
// tiles are laid out while basemap tiles hold the pool. These tests check WHERE
// the work runs and WHAT waits for what, never how long anything takes: each
// wait ends on an event, and its 30 s bound only turns a missing event into a
// failure instead of a hang.

#include <mln/test/map_adapter.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/stub_style_observer.hpp>
#include <mln/test/util.hpp>

#include <mln/annotation/annotation_manager.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/map_observer.hpp>
#include <mln/map/map_options.hpp>
#include <mln/map/transform.hpp>
#include <mln/renderer/image_manager.hpp>
#include <mln/renderer/sources/render_geojson_source.hpp>
#include <mln/renderer/tile_parameters.hpp>
#include <mln/style/conversion/geojson.hpp>
#include <mln/style/conversion/geojson_options.hpp>
#include <mln/style/conversion/json.hpp>
#include <mln/style/layers/circle_layer.hpp>
#include <mln/style/layers/circle_layer_impl.hpp>
#include <mln/style/sources/geojson_source.hpp>
#include <mln/style/sources/geojson_source_impl.hpp>
#include <mln/style/style.hpp>
#include <mln/text/glyph_manager.hpp>
#include <mln/tile/geojson_tile.hpp>
#include <mln/util/io.hpp>
#include <mln/util/label_steps.hpp>
#include <mln/util/logging.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/timer.hpp>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

using namespace mln;
using namespace mln::style;

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

/** A latch that one or more threads wait on until it is opened. */
class Gate {
public:
    void open() {
        std::scoped_lock lock(mutex);
        opened = true;
        cv.notify_all();
    }
    void wait() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return opened; });
    }

private:
    std::mutex mutex;
    std::condition_variable cv;
    bool opened = false;
};

/**
 * Holds every thread of the shared worker pool (`Scheduler::GetBackground()`,
 * a ThreadPool of 4) in a task of its own until released, and confirms all
 * four are held before it returns.
 */
class PoolBlock {
public:
    static constexpr std::size_t kThreads = 4;

    PoolBlock() {
        for (std::size_t i = 0; i < kThreads; ++i) {
            pool->schedule([this] {
                {
                    std::scoped_lock lock(mutex);
                    ++held;
                    cv.notify_all();
                }
                gate.wait();
            });
        }
        std::unique_lock lock(mutex);
        allHeld = cv.wait_for(lock, kBound, [&] { return held == kThreads; });
    }

    ~PoolBlock() {
        release();
        pool->waitForEmpty();
    }

    void release() { gate.open(); }

    const std::shared_ptr<Scheduler> pool = Scheduler::GetBackground();
    bool allHeld = false;

private:
    Gate gate;
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t held = 0;
};

Immutable<GeoJSONOptions> laneOptions() {
    GeoJSONOptions options;
    options.labelLane = true;
    return makeMutable<GeoJSONOptions>(std::move(options));
}

Immutable<GeoJSONOptions> syncOptions() {
    GeoJSONOptions options;
    options.synchronousUpdate = true;
    return makeMutable<GeoJSONOptions>(std::move(options));
}

const std::string kPoints = R"({"type": "FeatureCollection", "features": [
    {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [-20, 20]}},
    {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [20, 20]}},
    {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [-20, -20]}},
    {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [20, -20]}}
]})";

GeoJSON points() {
    conversion::Error error;
    auto geoJSON = conversion::parseGeoJSON(kPoints, error);
    assert(geoJSON);
    return *geoJSON;
}

/** One tile's world: tile parameters on the shared pool, for a tile made by hand. */
class TileWorld {
public:
    util::SimpleIdentity uniqueID;
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>();
    TransformState transformState;
    style::Style style{fileSource, 1, {Scheduler::GetBackground(), uniqueID}};
    AnnotationManager annotationManager{style};
    std::shared_ptr<ImageManager> imageManager = ImageManager::create();
    std::shared_ptr<GlyphManager> glyphManager = std::make_shared<GlyphManager>();
    CircleLayer layer{"circles", "source"};
    Immutable<LayerProperties> layerProperties = makeMutable<CircleLayerProperties>(
        staticImmutableCast<CircleLayer::Impl>(layer.baseImpl));

    TileParameters parameters(bool lane) {
        return {.pixelRatio = 1.0,
                .debugOptions = MapDebugOptions(),
                .transformState = transformState,
                .fileSource = fileSource,
                .mode = MapMode::Continuous,
                .annotationManager = annotationManager.makeWeakPtr(),
                .imageManager = imageManager,
                .glyphManager = glyphManager,
                .prefetchZoomDelta = 0,
                .threadPool = {Scheduler::GetBackground(), uniqueID},
                .labelLane = lane ? Scheduler::GetLabelLane() : nullptr};
    }

    /** Parses `kPoints` on the source's parse scheduler, as the Android binding does, and sets it. */
    bool parseAndSet(GeoJSONSource& source) {
        bool set = false;
        source.getParseScheduler()->scheduleAndReplyValue(
            util::SimpleIdentity::Empty,
            [slicer = source.getSliceScheduler(), options = source.impl().getOptions(), id = source.getID()] {
                return GeoJSONData::parse(kPoints, slicer, options, id);
            },
            [&](GeoJSONData::ParseResult parsed) {
                source.setGeoJSONData(std::get<std::shared_ptr<GeoJSONData>>(std::move(parsed)));
                set = true;
            });
        return runUntil(loop, [&] { return set; }, "the body's parse");
    }
};

bool hasStep(const std::vector<util::LabelStep>& steps, const std::string& sourceID, const std::string& name) {
    return std::ranges::any_of(steps, [&](const auto& step) { return step.sourceID == sourceID && step.name == name; });
}

/** The recorder, on for one test, and off and emptied after it. */
class Recording {
public:
    Recording() {
        (void)util::LabelSteps::take();
        util::LabelSteps::setRecording(true);
    }
    ~Recording() {
        util::LabelSteps::setRecording(false);
        (void)util::LabelSteps::take();
    }
    /** Every step so far, kept here, as `take()` empties the recorder. */
    const std::vector<util::LabelStep>& steps() {
        auto taken = util::LabelSteps::take();
        all.insert(all.end(), taken.begin(), taken.end());
        return all;
    }

private:
    std::vector<util::LabelStep> all;
};

} // namespace

// The options and the refusals.

TEST(LabelLane, ASourceRefusesTheLaneWithASynchronousUpdate) {
    GeoJSONOptions both;
    both.labelLane = true;
    both.synchronousUpdate = true;
    EXPECT_THROW(GeoJSONSource("source", makeMutable<GeoJSONOptions>(std::move(both))), std::invalid_argument);
    EXPECT_NO_THROW(GeoJSONSource("source", laneOptions()));
    EXPECT_NO_THROW(GeoJSONSource("source", syncOptions()));
}

TEST(LabelLane, TheStyleRefusesTheLaneWithASynchronousUpdate) {
    conversion::Error error;
    auto both = conversion::convertJSON<GeoJSONOptions>(R"({"labelLane": true, "synchronousUpdate": true})", error);
    EXPECT_FALSE(both);
    EXPECT_EQ("GeoJSON source labelLane cannot be set with synchronousUpdate", error.message);

    error = {};
    auto lane = conversion::convertJSON<GeoJSONOptions>(R"({"labelLane": true})", error);
    ASSERT_TRUE(lane) << error.message;
    EXPECT_TRUE(lane->labelLane);

    // A style source with options that do not convert fails: it is not made
    // with the default options.
    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>();
    style::Style style{fileSource, 1, {Scheduler::GetBackground(), util::SimpleIdentity{}}};
    Log::setObserver(std::make_unique<Log::NullObserver>());
    style.loadJSON(R"({"version": 8, "sources": {"labels": {"type": "geojson", "data": "http://body",
        "labelLane": true, "synchronousUpdate": true}}, "layers": []})");
    EXPECT_EQ(nullptr, style.getSource("labels"));
}

TEST(LabelLane, ALaneSourceRefusesASynchronousOverride) {
    GeoJSONSource lane("source", laneOptions());
    EXPECT_THROW(lane.setOverrideSynchronousUpdate(true), std::logic_error);
    EXPECT_FALSE(lane.isUpdateSynchronous());
    EXPECT_NO_THROW(lane.setOverrideSynchronousUpdate(false));

    GeoJSONSource plain("source");
    plain.setOverrideSynchronousUpdate(true);
    EXPECT_TRUE(plain.isUpdateSynchronous());
}

TEST(LabelLane, ALaneSourceParsesOnTheLaneAndCutsOnAThreadOfItsOwn) {
    GeoJSONSource lane("lane", laneOptions());
    GeoJSONSource other("other", laneOptions());
    GeoJSONSource plain("plain");
    ASSERT_NE(nullptr, Scheduler::GetLabelLane());
    EXPECT_EQ(Scheduler::GetLabelLane(), lane.getParseScheduler());
    EXPECT_NE(Scheduler::GetBackground(), lane.getParseScheduler());
    EXPECT_NE(lane.getSliceScheduler(), other.getSliceScheduler());
    EXPECT_EQ(Scheduler::GetBackground(), plain.getParseScheduler());
}

TEST(LabelLane, ABodyThatDoesNotParseAnswersAnError) {
    auto parsed = GeoJSONData::parse("{not json", Scheduler::GetSequenced(), laneOptions(), "source");
    ASSERT_TRUE(std::holds_alternative<std::string>(parsed));
    EXPECT_NE(std::string::npos, std::get<std::string>(parsed).find("Failed to parse GeoJSON data"));
}

TEST(LabelLane, ABodyThatCannotBeMadeIsASourceError) {
    util::RunLoop loop;
    StubStyleObserver observer;
    std::string reported;
    observer.sourceError = [&](Source& source, std::exception_ptr error) {
        EXPECT_EQ("source", source.getID());
        reported = util::toString(error);
    };
    GeoJSONSource source("source", laneOptions());
    source.setObserver(&observer);
    source.setGeoJSONError(7, "Failed to parse GeoJSON data: bad");
    EXPECT_EQ("GeoJSON body 7: Failed to parse GeoJSON data: bad", reported);
}

TEST(LabelLane, AURLBodyThatDoesNotParseIsASourceError) {
    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>();
    fileSource->sourceResponse = [](const Resource&) {
        Response response;
        response.data = std::make_shared<std::string>("{not json");
        return response;
    };
    StubStyleObserver observer;
    std::string reported;
    bool loaded = false;
    observer.sourceError = [&](Source&, std::exception_ptr error) {
        reported = util::toString(error);
    };
    observer.sourceLoaded = [&](Source&) {
        loaded = true;
    };
    GeoJSONSource source("source", laneOptions());
    source.setObserver(&observer);
    source.setURL("http://body");
    source.loadDescription(*fileSource);
    ASSERT_TRUE(runUntil(loop, [&] { return loaded; }, "the body"));
    EXPECT_NE(std::string::npos, reported.find("Failed to parse GeoJSON data")) << reported;
}

// The queue test: with every thread of the shared pool held, a lane source's
// body is parsed, cut and laid out.
TEST(LabelLane, ALaneBodyIsLaidOutWhileThePoolIsBlocked) {
    TileWorld world;
    GeoJSONSource source("source", laneOptions());
    PoolBlock block;
    ASSERT_TRUE(block.allHeld) << "the pool's four threads were not all held";

    ASSERT_TRUE(world.parseAndSet(source));
    GeoJSONTile tile(OverscaledTileID(0, 0, 0), "source", world.parameters(true), source.impl().getData().lock());
    tile.setLayers({world.layerProperties}, nullptr);
    EXPECT_TRUE(runUntil(world.loop, [&] { return tile.isComplete(); }, "the lane tile's layout"));
}

// The control: without the option, the tile's worker waits for the pool.
TEST(LabelLane, APoolBodyWaitsForTheBlockedPool) {
    TileWorld world;
    Recording recording;
    GeoJSONSource source("source");
    ASSERT_TRUE(world.parseAndSet(source)); // The body's parse is on the pool: before the block.
    PoolBlock block;
    ASSERT_TRUE(block.allHeld) << "the pool's four threads were not all held";

    GeoJSONTile tile(OverscaledTileID(0, 0, 0), "source", world.parameters(false), source.impl().getData().lock());
    tile.setLayers({world.layerProperties}, nullptr);
    // The cut (on the sequenced ring, not the pool) reaches the tile, which
    // sends it to its worker.
    ASSERT_TRUE(
        runUntil(world.loop, [&] { return hasStep(recording.steps(), "source", "hop-data"); }, "the cut tile's data"));
    EXPECT_FALSE(tile.isComplete());
    EXPECT_FALSE(hasStep(recording.steps(), "source", "tile-parse"));

    block.release();
    EXPECT_TRUE(runUntil(world.loop, [&] { return tile.isComplete(); }, "the tile's layout after the release"));
}

// The loaded signal, on the render source: once per generation, after the
// last tile in view is laid out, and never for an older generation.
TEST(LabelLane, TheLaidOutSignalComesOnceForEachBodyAfterItsLastTile) {
    TileWorld world;
    Log::setObserver(std::make_unique<Log::NullObserver>());
    Transform transform;
    transform.resize({512, 512});
    transform.jumpTo(CameraOptions().withCenter(LatLng()).withZoom(1.5));
    world.transformState = transform.getState();

    GeoJSONSource source("source", laneOptions());
    EXPECT_EQ(0u, source.getDataGeneration());
    EXPECT_EQ(1u, source.setGeoJSON(points()));
    EXPECT_EQ(1u, source.getDataGeneration());
    RenderGeoJSONSource renderSource{staticImmutableCast<GeoJSONSource::Impl>(source.baseImpl),
                                     world.parameters(true).threadPool};
    const std::vector<Immutable<LayerProperties>> layers{world.layerProperties};

    Recording recording;
    std::vector<uint64_t> reported;
    // Updates as the orchestrator does each frame, until a report.
    const auto updateUntilReported = [&](const std::string& what) {
        return runUntil(
            world.loop,
            [&] {
                renderSource.update(source.baseImpl, layers, true, false, world.parameters(true));
                const bool allComplete = renderSource.isLoaded();
                if (auto generation = renderSource.takeLaidOutGeneration()) {
                    reported.push_back(*generation);
                    EXPECT_TRUE(allComplete) << "reported with a tile still being laid out";
                    return true;
                }
                return false;
            },
            what);
    };
    const auto layoutsSoFar = [&] {
        const auto& steps = recording.steps();
        return std::ranges::count_if(steps, [](const auto& step) { return step.name == "hop-layout"; });
    };

    // The first body: no tile was complete before, so an update that sees
    // every tile complete must report it.
    bool completeUnreported = false;
    ASSERT_TRUE(runUntil(
        world.loop,
        [&] {
            renderSource.update(source.baseImpl, layers, true, false, world.parameters(true));
            const bool allComplete = renderSource.isLoaded();
            if (auto generation = renderSource.takeLaidOutGeneration()) {
                reported.push_back(*generation);
                EXPECT_TRUE(allComplete) << "reported with a tile still being laid out";
                return true;
            }
            completeUnreported = completeUnreported || allComplete;
            return false;
        },
        "generation 1"));
    EXPECT_FALSE(completeUnreported) << "every tile was laid out and nothing was reported";
    EXPECT_EQ(std::vector<uint64_t>{1}, reported);
    // The view holds the four tiles round (0, 0) at zoom 1.
    for (const auto& [x, y] : std::vector<std::pair<uint32_t, uint32_t>>{{0, 0}, {0, 1}, {1, 0}, {1, 1}}) {
        EXPECT_NE(nullptr, renderSource.getRenderedTile(UnwrappedTileID(1, x, y))) << x << "/" << y;
    }

    // Once: the next update reports nothing.
    renderSource.update(source.baseImpl, layers, true, false, world.parameters(true));
    EXPECT_FALSE(renderSource.takeLaidOutGeneration());

    // Two bodies before the next frame: only the newer is reported.
    EXPECT_EQ(2u, source.setGeoJSON(points()));
    EXPECT_EQ(3u, source.setGeoJSON(points()));
    ASSERT_TRUE(updateUntilReported("generation 3"));
    EXPECT_EQ((std::vector<uint64_t>{1, 3}), reported);

    // A body whose four tiles are laid out, then replaced before an update
    // sees it: neither it nor the newer one is reported for those layouts.
    EXPECT_EQ(4u, source.setGeoJSON(points()));
    renderSource.update(source.baseImpl, layers, true, false, world.parameters(true));
    const auto layoutsBefore = layoutsSoFar();
    ASSERT_TRUE(
        runUntil(world.loop, [&] { return layoutsSoFar() >= layoutsBefore + 4; }, "generation 4's four layouts"));
    EXPECT_EQ(5u, source.setGeoJSON(points()));
    renderSource.update(source.baseImpl, layers, true, false, world.parameters(true));
    EXPECT_FALSE(renderSource.takeLaidOutGeneration()) << "reported with the tiles still holding generation 4";
    ASSERT_TRUE(updateUntilReported("generation 5"));
    EXPECT_EQ((std::vector<uint64_t>{1, 3, 5}), reported);
}

TEST(LabelLane, AURLBodyHasTheGenerationItsSetterAnswered) {
    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>();
    fileSource->sourceResponse = [](const Resource&) {
        Response response;
        response.data = std::make_shared<std::string>(kPoints);
        return response;
    };
    StubStyleObserver observer;
    bool loaded = false;
    observer.sourceLoaded = [&](Source&) {
        loaded = true;
    };
    GeoJSONSource source("source", laneOptions());
    source.setObserver(&observer);
    EXPECT_EQ(1u, source.setGeoJSON(points()));
    EXPECT_EQ(2u, source.setURL("http://body"));
    EXPECT_EQ(1u, source.getDataGeneration());
    source.loadDescription(*fileSource);
    ASSERT_TRUE(runUntil(loop, [&] { return loaded; }, "the URL body"));
    EXPECT_EQ(2u, source.getDataGeneration());
}

// The map: the orchestrator's wiring, the step recorder, and the lifetime.

namespace {

constexpr const char* kTextLayers = R"(
    {"id": "labels", "type": "symbol", "source": "labels",
     "layout": {"text-field": "ab", "text-font": ["Test Stack"], "text-allow-overlap": true}},
    {"id": "plain", "type": "symbol", "source": "plain",
     "layout": {"text-field": "ab", "text-font": ["Test Stack"], "text-allow-overlap": true}})";

std::string styleJSON(bool withLabels) {
    return std::string(R"({"version": 8, "center": [0, 0], "zoom": 1.5,
        "glyphs": "http://glyphs/{fontstack}/{range}.pbf",
        "sources": {
          "labels": {"type": "geojson", "data": "http://bodies/labels.json", "labelLane": true},
          "plain": {"type": "geojson", "data": "http://bodies/plain.json"}},
        "layers": [)") +
           (withLabels ? kTextLayers : "") + "]}";
}

class LaidOutObserver final : public MapObserver {
public:
    void onSourceLaidOut(const std::string& sourceID, uint64_t generation) override {
        laidOut.emplace_back(sourceID, generation);
    }
    void onDidFinishRenderingFrame(const RenderFrameStatus&) override { ++frames; }
    bool has(const std::string& sourceID, uint64_t generation) const {
        return std::ranges::find(laidOut, std::make_pair(sourceID, generation)) != laidOut.end();
    }
    std::vector<std::pair<std::string, uint64_t>> laidOut;
    int frames = 0;
};

/** A continuous headless map with a lane source ("labels") and a plain one ("plain"). */
class LaneMap {
public:
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>();
    LaidOutObserver observer;
    std::unique_ptr<HeadlessFrontend> frontend = std::make_unique<HeadlessFrontend>(Size{256, 256}, 1.0f);
    std::unique_ptr<MapAdapter> map = std::make_unique<MapAdapter>(
        *frontend,
        observer,
        fileSource,
        MapOptions().withMapMode(MapMode::Continuous).withSize(frontend->getSize()).withPixelRatio(1.0));

    LaneMap() {
        fileSource->sourceResponse = [](const Resource&) {
            Response response;
            response.data = std::make_shared<std::string>(kPoints);
            return response;
        };
        fileSource->glyphsResponse = [](const Resource&) {
            Response response;
            response.data = std::make_shared<std::string>(util::read_file("test/fixtures/resources/glyphs.pbf"));
            return response;
        };
        map->getStyle().loadJSON(styleJSON(true));
    }

    GeoJSONSource& labels() { return *map->getStyle().getSource("labels")->as<GeoJSONSource>(); }

    /** Destroys the map, then the renderer and its context (whose debug assert checks every buffer and texture is
     * freed). */
    void destroy() {
        map.reset();
        frontend.reset();
    }
};

} // namespace

// The thread test: every recorded step of a lane source runs on the lane,
// none on the render thread; a plain source's run on the pool.
TEST(LabelLane, EveryStepOfALaneSourceRunsOnTheLane) {
    Recording recording;
    LaneMap test;
    ASSERT_TRUE(runUntil(
        test.loop,
        [&] { return test.observer.has("labels", 1) && test.observer.has("plain", 1); },
        "both sources laid out"));

    std::set<std::string> laneSteps;
    std::set<std::string> plainSteps;
    for (const auto& step : recording.steps()) {
        if (step.hop) continue;
        if (step.sourceID == "labels") {
            laneSteps.insert(step.name);
            // Darwin names a thread "org.maplibre.mbgl." and the name given.
            EXPECT_NE(std::string::npos, step.thread.find("Label lane")) << step.name << " ran on " << step.thread;
        } else if (step.sourceID == "plain") {
            plainSteps.insert(step.name);
            EXPECT_EQ(std::string::npos, step.thread.find("Label lane")) << step.name << " ran on " << step.thread;
        }
    }
    EXPECT_EQ((std::set<std::string>{"parse-url", "index", "slice", "tile-parse", "tile-layout"}), laneSteps);
    EXPECT_EQ((std::set<std::string>{"parse-url", "index", "slice", "tile-parse", "tile-layout"}), plainSteps);
}

// The teardown test: a lane tile's release frees its buckets and textures on
// the render thread. With its render-thread jobs on any queue but the one the
// context drains, the context's debug assert (`~Context`: every buffer and
// texture freed) fails when the map is destroyed.
TEST(LabelLane, ALaneTileFreesItsBuffersOnTheRenderThread) {
    LaneMap test;
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", 1); }, "the lane source laid out"));

    test.map->getStyle().removeLayer("labels");
    test.map->getStyle().removeSource("labels");
    const int framesBefore = test.observer.frames;
    ASSERT_TRUE(
        runUntil(test.loop, [&] { return test.observer.frames > framesBefore; }, "a frame after the lane source went"));
    test.destroy();
}

// The map is destroyed while lane work is queued: the lane is held by a task
// of its own, and the tiles' worker messages wait behind it.
TEST(LabelLane, DestroyingTheMapWithLaneWorkInFlightIsSafe) {
    Recording recording;
    LaneMap test;
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", 1); }, "the lane source laid out"));

    Gate held;
    Gate release;
    Scheduler::GetLabelLane()->schedule([&] {
        held.open();
        release.wait();
    });
    held.wait();
    const std::size_t stepsBefore = recording.steps().size();
    test.labels().setGeoJSON(points());
    // The cut tiles reach their tiles and go to the held lane.
    ASSERT_TRUE(runUntil(
        test.loop,
        [&] {
            const auto& steps = recording.steps();
            return std::any_of(steps.begin() + static_cast<std::ptrdiff_t>(stepsBefore),
                               steps.end(),
                               [](const auto& s) { return s.sourceID == "labels" && s.name == "hop-data"; });
        },
        "the new body's tiles sent to their workers"));

    // The release comes from another thread: the destructor waits for the lane.
    Gate destroying;
    std::thread releaser([&] {
        destroying.wait();
        release.open();
    });
    destroying.open();
    test.destroy();
    releaser.join();
}
