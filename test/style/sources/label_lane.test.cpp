// The label lane (GeoJSONOptions::labelLane): a GeoJSON source whose off-thread
// work runs on a lane of its own, not on the shared worker pool, so that its
// tiles are laid out while basemap tiles hold the pool. These tests check WHERE
// the work runs and WHAT waits for what, never how long anything takes: each
// wait ends on an event, and its 30 s bound only turns a missing event into a
// failure instead of a hang.

#include <iostream>
#include <mln/test/fake_file_source.hpp>
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
#include <mln/renderer/query.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/renderer/sources/render_geojson_source.hpp>
#include <mln/renderer/tile_parameters.hpp>
#include <mln/style/conversion/geojson.hpp>
#include <mln/style/conversion/geojson_options.hpp>
#include <mln/style/conversion/filter.hpp>
#include <mln/style/conversion/json.hpp>
#include <mln/style/layers/circle_layer.hpp>
#include <mln/style/layers/circle_layer_impl.hpp>
#include <mln/style/sources/geojson_source.hpp>
#include <mln/style/sources/geojson_source_impl.hpp>
#include <mln/style/style.hpp>
#include <mln/style/transition_options.hpp>
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
#include <random>
#include <set>
#include <stdexcept>
#include <thread>

using namespace mln;
using namespace mln::style;

namespace {

constexpr auto kBound = std::chrono::seconds(30);

/** Runs the loop until `done()`. False, with a failure, if it has not come within the bound. */
#if defined(__APPLE__)
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void*);
#endif

bool runUntil(util::RunLoop& loop, const std::function<bool()>& done, const std::string& what) {
    bool late = false;
    util::Timer watchdog;
    watchdog.start(kBound, Duration::zero(), [&] { late = true; });
    while (!done() && !late) {
        // On Apple, inside an autorelease pool: the headless Metal backend's
        // command buffers are autoreleased, and with no pool drained between
        // frames the 65th frame waits for ever in -[MTLCommandQueue
        // commandBuffer], so the bound above never fires.
#if defined(__APPLE__)
        void* pool = objc_autoreleasePoolPush();
        loop.runOnce();
        objc_autoreleasePoolPop(pool);
#else
        loop.runOnce();
#endif
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
                .dynamicTextureAtlas = nullptr,
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

// Supercluster cuts its tiles on the render thread: a lane source with
// clusters would be a lane in part only.
TEST(LabelLane, ASourceRefusesTheLaneWithClusters) {
    GeoJSONOptions both;
    both.labelLane = true;
    both.cluster = true;
    EXPECT_THROW(GeoJSONSource("source", makeMutable<GeoJSONOptions>(std::move(both))), std::invalid_argument);
    GeoJSONOptions clusters;
    clusters.cluster = true;
    EXPECT_NO_THROW(GeoJSONSource("source", makeMutable<GeoJSONOptions>(std::move(clusters))));

    conversion::Error error;
    EXPECT_FALSE(conversion::convertJSON<GeoJSONOptions>(R"({"labelLane": true, "cluster": true})", error));
    EXPECT_EQ("GeoJSON source labelLane cannot be set with cluster", error.message);
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

// The orchestrator keeps a render source across a style rebuild when the id
// and type match, and the new style's source counts its generations from low
// again. What the render source reported belongs to the old source object.
TEST(LabelLane, ASourceOfANewStyleWithTheSameIDIsReportedFromItsFirstLayout) {
    TileWorld world;
    Log::setObserver(std::make_unique<Log::NullObserver>());
    Transform transform;
    transform.resize({512, 512});
    transform.jumpTo(CameraOptions().withCenter(LatLng()).withZoom(1.5));
    world.transformState = transform.getState();

    GeoJSONSource oldSource("source", laneOptions());
    EXPECT_EQ(1u, oldSource.setGeoJSON(points()));
    EXPECT_EQ(2u, oldSource.setGeoJSON(points()));
    EXPECT_EQ(3u, oldSource.setGeoJSON(points()));
    RenderGeoJSONSource renderSource{staticImmutableCast<GeoJSONSource::Impl>(oldSource.baseImpl),
                                     world.parameters(true).threadPool};
    const std::vector<Immutable<LayerProperties>> layers{world.layerProperties};

    std::vector<uint64_t> reported;
    const auto updateUntilReported = [&](const GeoJSONSource& source, const std::string& what) {
        return runUntil(
            world.loop,
            [&] {
                renderSource.update(source.baseImpl, layers, true, false, world.parameters(true));
                if (auto generation = renderSource.takeLaidOutGeneration()) {
                    reported.push_back(*generation);
                    return true;
                }
                return false;
            },
            what);
    };
    ASSERT_TRUE(updateUntilReported(oldSource, "the old style's generation 3"));
    EXPECT_EQ(std::vector<uint64_t>{3}, reported);

    // The style is rebuilt: a new source with the same id, generation 1.
    GeoJSONSource newSource("source", laneOptions());
    EXPECT_EQ(1u, newSource.setGeoJSON(points()));
    ASSERT_TRUE(updateUntilReported(newSource, "the new style's generation 1"));
    EXPECT_EQ((std::vector<uint64_t>{3, 1}), reported);

    // Within the new source the order holds: an older number is not reported.
    renderSource.update(newSource.baseImpl, layers, true, false, world.parameters(true));
    EXPECT_FALSE(renderSource.takeLaidOutGeneration());
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
    void onDidFinishRenderingFrame(const RenderFrameStatus& status) override {
        ++frames;
        fullFrames = status.mode == RenderMode::Full ? fullFrames + 1 : 0;
    }
    /// Consecutive fully rendered frames: every tile in view is drawn.
    int fullFrames = 0;
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

    explicit LaneMap(const std::string& style = styleJSON(true)) {
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
        map->getStyle().loadJSON(style);
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

// A URL answered twice (a revalidation): the same bytes again are no new
// body; different bytes are, with a new generation, so a loaded check never
// waits on a body it can no longer see.
TEST(LabelLane, AURLBodyAnsweredAgainIsANewBodyOnlyIfItDiffers) {
    util::RunLoop loop;
    FakeFileSource fileSource;
    StubStyleObserver observer;
    int loads = 0;
    observer.sourceLoaded = [&](Source&) {
        ++loads;
    };
    GeoJSONSource source("source", laneOptions());
    source.setObserver(&observer);
    EXPECT_EQ(1u, source.setURL("http://body"));
    source.loadDescription(fileSource);
    const auto answer = [&](const std::string& body) {
        Response response;
        response.data = std::make_shared<std::string>(body);
        return fileSource.respond(Resource::Kind::Source, response);
    };

    ASSERT_TRUE(answer(kPoints));
    ASSERT_TRUE(runUntil(loop, [&] { return loads == 1; }, "the first body"));
    EXPECT_EQ(1u, source.getDataGeneration());
    const auto firstData = source.impl().getData().lock();

    // The same bytes, in a new string: ignored when the answer comes, before
    // any parse is scheduled.
    ASSERT_TRUE(answer(kPoints));
    EXPECT_EQ(1u, source.getDataGeneration());
    EXPECT_EQ(firstData, source.impl().getData().lock());

    const std::string other = R"({"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [5, 5]}}]})";
    ASSERT_TRUE(answer(other));
    ASSERT_TRUE(runUntil(loop, [&] { return loads == 2; }, "the second, different body"));
    EXPECT_EQ(2u, source.getDataGeneration());
    EXPECT_NE(firstData, source.impl().getData().lock());
}

// A URL request cancelled by inline data leaves the source ready to take
// another URL: the style reloads on the description change, and the second
// URL must be asked for. (The app empties a slot while its URL loads, then
// gives the slot a new URL.)
TEST(LabelLane, AURLAfterInlineDataCancelledTheLoadingURLIsRequested) {
    util::RunLoop loop;
    FakeFileSource fileSource;
    StubStyleObserver observer;
    int loads = 0;
    observer.sourceLoaded = [&](Source&) {
        ++loads;
    };
    // What the style does with a description change.
    observer.sourceDescriptionChanged = [&](Source& changed) {
        changed.loadDescription(fileSource);
    };
    GeoJSONSource source("source", laneOptions());
    source.setObserver(&observer);
    // The first load is the style's own, on adding the source.
    source.loadDescription(fileSource);
    EXPECT_EQ(1u, source.setURL("http://body/a"));
    // A is in flight, unanswered.
    source.loadDescription(fileSource);
    ASSERT_EQ(1u, fileSource.requests.size());
    EXPECT_EQ("http://body/a", fileSource.requests.front()->resource.url);

    // Inline data cancels A's request.
    source.setGeoJSON(GeoJSON{FeatureCollection{}});
    EXPECT_TRUE(fileSource.requests.empty());

    // B must be requested.
    source.setURL("http://body/b");
    ASSERT_EQ(1u, fileSource.requests.size()) << "the second URL was never requested";
    EXPECT_EQ("http://body/b", fileSource.requests.front()->resource.url);

    Response response;
    response.data = std::make_shared<std::string>(kPoints);
    ASSERT_TRUE(fileSource.respond(Resource::Kind::Source, response));
    ASSERT_TRUE(runUntil(loop, [&] { return loads == 1; }, "the second URL's body"));
}

// The class of bug behind the test above, by random sequences: setURL, inline
// data, the file source answering, failing or staying silent, and the style's
// loadDescription, in any order. After the last setURL with no inline data
// since, the source must have that URL requested or its body landed: never a
// URL that is neither loaded nor requested. A failure prints the seed.
TEST(LabelLane, ASourceNeverEndsWithAURLThatIsNeitherRequestedNorLoaded) {
    util::RunLoop loop;
    constexpr unsigned kSequences = 3000;
    unsigned failures = 0;
    for (unsigned seed = 0; seed < kSequences; ++seed) {
        std::mt19937 rng(seed);
        FakeFileSource fileSource;
        StubStyleObserver observer;
        // What the style does with a description change.
        observer.sourceDescriptionChanged = [&](Source& changed) {
            changed.loadDescription(fileSource);
        };
        GeoJSONSource source("source", (rng() & 1) ? laneOptions() : makeMutable<GeoJSONOptions>());
        source.setObserver(&observer);
        // The style loads a source when it adds it.
        source.loadDescription(fileSource);

        std::string lastURL; // the newest setURL, empty once inline data followed it
        std::string landedURL;
        std::string trace;
        unsigned urls = 0;
        const unsigned steps = 1 + rng() % 8;
        for (unsigned step = 0; step < steps; ++step) {
            switch (rng() % 6) {
                case 0:
                    lastURL = "http://body/" + std::to_string(++urls);
                    source.setURL(lastURL);
                    trace += " setURL(" + std::to_string(urls) + ")";
                    break;
                case 1:
                    source.setGeoJSON(GeoJSON{FeatureCollection{}});
                    lastURL.clear();
                    trace += " setGeoJSON";
                    break;
                case 2: {
                    Response response;
                    response.data = std::make_shared<std::string>(kPoints);
                    if (!fileSource.requests.empty()) landedURL = fileSource.requests.front()->resource.url;
                    fileSource.respond(Resource::Kind::Source, response);
                    trace += " answer";
                    break;
                }
                case 3: {
                    Response response;
                    response.error = std::make_unique<Response::Error>(Response::Error::Reason::Connection, "down");
                    fileSource.respond(Resource::Kind::Source, response);
                    trace += " fail";
                    break;
                }
                case 4:
                    trace += " silence";
                    break;
                default:
                    source.loadDescription(fileSource);
                    trace += " loadDescription";
                    break;
            }
        }
        if (lastURL.empty()) continue;
        const bool requested = std::ranges::any_of(
            fileSource.requests, [&](const auto* request) { return request->resource.url == lastURL; });
        if (!requested && landedURL != lastURL) {
            if (++failures <= 5) ADD_FAILURE() << "seed " << seed << ": " << lastURL << " neither requested nor loaded after:" << trace;
        }
        loop.runOnce();
    }
    EXPECT_EQ(0u, failures) << "of " << kSequences << " sequences";
}

namespace {

std::optional<Filter> filterFrom(const std::string& json) {
    conversion::Error error;
    auto filter = conversion::convertJSON<Filter>(json, error);
    EXPECT_TRUE(filter) << error.message;
    return filter;
}

} // namespace

// A layer's filter or layout change moves its GeoJSON source's generation
// at once, for the app to read; a paint change, or a change on another
// source's layer, does not.
TEST(LabelLane, AFilterOrLayoutChangeMovesTheSourcesGeneration) {
    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>();
    Log::setObserver(std::make_unique<Log::NullObserver>());
    style::Style style{fileSource, 1, {Scheduler::GetBackground(), util::SimpleIdentity{}}};
    style.loadJSON(R"({"version": 8, "sources": {
        "labels": {"type": "geojson", "data": {"type": "FeatureCollection", "features": []}, "labelLane": true},
        "other": {"type": "geojson", "data": {"type": "FeatureCollection", "features": []}}},
      "layers": [
        {"id": "labels", "type": "circle", "source": "labels"},
        {"id": "other", "type": "circle", "source": "other"}]})");
    auto& labels = *style.getSource("labels")->as<GeoJSONSource>();
    auto& labelsLayer = static_cast<CircleLayer&>(*style.getLayer("labels"));
    auto& otherLayer = static_cast<CircleLayer&>(*style.getLayer("other"));
    const uint64_t start = labels.getDataGeneration();
    ASSERT_EQ(1u, start);

    labelsLayer.setFilter(*filterFrom(R"(["==", ["get", "form"], "tag"])"));
    EXPECT_EQ(start + 1, labels.getDataGeneration()) << "a filter change";

    labelsLayer.setCircleColor(Color::red());
    EXPECT_EQ(start + 1, labels.getDataGeneration()) << "a paint change moved it";

    labelsLayer.setCircleSortKey(PropertyValue<float>(2.0f));
    EXPECT_EQ(start + 2, labels.getDataGeneration()) << "a layout change";

    otherLayer.setFilter(*filterFrom(R"(["==", ["get", "form"], "badge"])"));
    EXPECT_EQ(start + 2, labels.getDataGeneration()) << "another source's layer moved it";
}

// A body made off the calling thread lands after a layer change took a later
// generation: it takes a new one, so the generation never goes back.
TEST(LabelLane, ABodyThatLandsAfterALayerChangeTakesANewGeneration) {
    util::RunLoop loop;
    GeoJSONSource source("source", laneOptions());
    const uint64_t asked = source.nextDataGeneration();
    const uint64_t layers = source.markLayersChanged();
    ASSERT_GT(layers, asked);
    EXPECT_EQ(layers, source.getLatestGeneration());
    // Pending: the source holds no generation as new as the body's yet.
    EXPECT_LT(source.getDataGeneration(), asked);
    source.setGeoJSONData(GeoJSONData::create(points(), source.getSliceScheduler(), laneOptions(), "source"), asked);
    // The body covers the layer change given out while it was pending.
    EXPECT_EQ(layers, source.getDataGeneration());
}

// ios-names' case: a layer change while a body is pending (a URL load, a
// body converting) is not reported as that body: the tiles hold the old one.
// The old body with the new layers reports nothing; the body, when it lands,
// is reported at the layer change's generation.
TEST(LabelLane, ALayerChangeWhileABodyIsPendingIsNotReportedAsThatBody) {
    TileWorld world;
    Log::setObserver(std::make_unique<Log::NullObserver>());
    Transform transform;
    transform.resize({512, 512});
    transform.jumpTo(CameraOptions().withCenter(LatLng()).withZoom(1.5));
    world.transformState = transform.getState();

    GeoJSONSource source("source", laneOptions());
    source.setGeoJSON(points());
    RenderGeoJSONSource renderSource{staticImmutableCast<GeoJSONSource::Impl>(source.baseImpl),
                                     world.parameters(true).threadPool};
    std::vector<Immutable<LayerProperties>> layers{world.layerProperties};
    std::vector<uint64_t> reported;
    const auto update = [&](bool relayout) {
        renderSource.update(source.baseImpl, layers, true, relayout, world.parameters(true));
        const bool allComplete = renderSource.isLoaded();
        if (auto generation = renderSource.takeLaidOutGeneration()) {
            reported.push_back(*generation);
            EXPECT_TRUE(allComplete) << "reported with a tile still being laid out";
            return true;
        }
        return false;
    };
    ASSERT_TRUE(runUntil(world.loop, [&] { return update(false); }, "the first body"));

    const uint64_t pending = source.nextDataGeneration();
    CircleLayer layer("circles", "source");
    layer.setFilter(*filterFrom(R"(["has", "a"])"));
    layers = {makeMutable<CircleLayerProperties>(staticImmutableCast<CircleLayer::Impl>(layer.baseImpl))};
    const uint64_t layerChange = source.markLayersChanged();
    ASSERT_GT(layerChange, pending);
    EXPECT_EQ(layerChange, source.getLatestGeneration());

    // The old body relaid out with the new layers: wait for every tile, then
    // check nothing was reported.
    update(true);
    ASSERT_TRUE(runUntil(
        world.loop,
        [&] {
            update(false);
            return renderSource.isLoaded();
        },
        "the old body with the new layers"));
    update(false);
    EXPECT_EQ(std::vector<uint64_t>{1}, reported) << "a generation was reported with the old body";

    source.setGeoJSONData(GeoJSONData::create(points(), source.getSliceScheduler(), laneOptions(), "source"), pending);
    ASSERT_TRUE(runUntil(world.loop, [&] { return update(false); }, "the pending body"));
    EXPECT_EQ((std::vector<uint64_t>{1, layerChange}), reported);
}

// An older body that lands while a newer one is pending keeps its own,
// lower, generation, so a wait for the newer one does not take it.
TEST(LabelLane, AnOlderBodyLandingKeepsAGenerationBelowThePendingOne) {
    util::RunLoop loop;
    GeoJSONSource source("source", laneOptions());
    const uint64_t older = source.nextDataGeneration();
    const uint64_t newer = source.nextDataGeneration();
    source.setGeoJSONData(GeoJSONData::create(points(), source.getSliceScheduler(), laneOptions(), "source"), older);
    EXPECT_EQ(older, source.getDataGeneration());
    EXPECT_LT(source.getDataGeneration(), newer);
    source.setGeoJSONData(GeoJSONData::create(points(), source.getSliceScheduler(), laneOptions(), "source"), newer);
    EXPECT_EQ(newer, source.getDataGeneration());
}

// The laid-out signal for a layer change: reported once, after the last tile
// is laid out with the new layers; with a body set too, at the newer
// generation only.
TEST(LabelLane, ALayerChangeIsReportedLaidOutOnceAfterItsLastTile) {
    TileWorld world;
    Log::setObserver(std::make_unique<Log::NullObserver>());
    Transform transform;
    transform.resize({512, 512});
    transform.jumpTo(CameraOptions().withCenter(LatLng()).withZoom(1.5));
    world.transformState = transform.getState();

    GeoJSONSource source("source", laneOptions());
    source.setGeoJSON(points());
    RenderGeoJSONSource renderSource{staticImmutableCast<GeoJSONSource::Impl>(source.baseImpl),
                                     world.parameters(true).threadPool};
    std::vector<Immutable<LayerProperties>> layers{world.layerProperties};

    std::vector<uint64_t> reported;
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
    ASSERT_TRUE(updateUntilReported("the body"));
    ASSERT_EQ(std::vector<uint64_t>{1}, reported);

    // New layers (a filter), as the style sends them with the source's new
    // generation: the update that takes them relays the tiles out.
    const auto newLayers = [&](const std::string& filter) {
        CircleLayer layer("circles", "source");
        layer.setFilter(*filterFrom(filter));
        layers = {makeMutable<CircleLayerProperties>(staticImmutableCast<CircleLayer::Impl>(layer.baseImpl))};
    };
    newLayers(R"(["has", "a"])");
    const uint64_t layerGeneration = source.markLayersChanged();
    renderSource.update(source.baseImpl, layers, true, true, world.parameters(true));
    EXPECT_FALSE(renderSource.isLoaded()) << "the new layers did not relay the tiles out";
    EXPECT_FALSE(renderSource.takeLaidOutGeneration()) << "reported before the tiles hold the new layers";
    ASSERT_TRUE(updateUntilReported("the layer change"));
    EXPECT_EQ((std::vector<uint64_t>{1, layerGeneration}), reported);
    renderSource.update(source.baseImpl, layers, true, false, world.parameters(true));
    EXPECT_FALSE(renderSource.takeLaidOutGeneration()) << "reported twice";

    // A body and a layer change before the next frame: the newer only.
    const uint64_t body = source.setGeoJSON(points());
    newLayers(R"(["has", "b"])");
    const uint64_t both = source.markLayersChanged();
    ASSERT_GT(both, body);
    renderSource.update(source.baseImpl, layers, true, true, world.parameters(true));
    ASSERT_TRUE(updateUntilReported("the body and the layer change"));
    EXPECT_EQ((std::vector<uint64_t>{1, layerGeneration, both}), reported);
}

// On a map: a filter set on a lane source's layer is reported laid out at the
// generation the source answers right after it.
TEST(LabelLane, AFilterChangeOnTheMapIsReportedLaidOut) {
    LaneMap test;
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", 1); }, "the lane source laid out"));
    test.map->getStyle().getLayer("labels")->setFilter(*filterFrom(R"(["==", ["get", "kind"], "none"])"));
    const uint64_t generation = test.labels().getDataGeneration();
    ASSERT_EQ(2u, generation);
    ASSERT_TRUE(
        runUntil(test.loop, [&] { return test.observer.has("labels", generation); }, "the filter change laid out"));
    EXPECT_EQ(1, std::ranges::count(test.observer.laidOut, std::make_pair(std::string("labels"), generation)));
}

namespace {

// Circles on the lane source: a rendered query reads the tiles' layout (their
// feature index), with no symbol placement in between.
std::string circleStyleJSON() {
    return R"({"version": 8, "center": [0, 0], "zoom": 1.5,
        "sources": {"labels": {"type": "geojson", "data": "http://bodies/labels.json", "labelLane": true}},
        "layers": [{"id": "circles", "type": "circle", "source": "labels", "paint": {"circle-radius": 8}}]})";
}

/** The circles the last frame drew. Read before the loop turns again, it is that frame's layout. */
std::size_t drawnCircles(LaneMap& test) {
    RenderedQueryOptions options;
    options.layerIDs = std::vector<std::string>{"circles"};
    return test.frontend->getRenderer()->queryRenderedFeatures(ScreenBox{{0, 0}, {256, 256}}, options).size();
}

/** Runs the loop until one more frame is drawn. */
bool runOneFrame(LaneMap& test, const std::string& what) {
    const int before = test.observer.frames;
    return runUntil(test.loop, [&] { return test.observer.frames > before; }, what);
}

/** Holds both lane threads in tasks of their own until released; confirms both are held. */
class LaneBlock {
public:
    static constexpr int kThreads = 2;
    LaneBlock() {
        for (int i = 0; i < kThreads; ++i) {
            Scheduler::GetLabelLane()->schedule([this] {
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
    ~LaneBlock() { release(); }
    void release() { gate.open(); }
    bool allHeld = false;

private:
    Gate gate;
    std::mutex mutex;
    std::condition_variable cv;
    int held = 0;
};

} // namespace

// A filter change on a lane source is drawn in the very frame that takes it,
// as on a synchronous source: the app's synchronous overlay, set in the same
// turn, lands in that frame too.
TEST(LabelLane, AFilterChangeIsDrawnInTheNextFrame) {
    LaneMap test(circleStyleJSON());
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", 1); }, "the lane source laid out"));
    ASSERT_TRUE(runOneFrame(test, "a frame with the body"));
    ASSERT_EQ(4u, drawnCircles(test)) << "the body's four points are not drawn";

    test.map->getStyle().getLayer("circles")->setFilter(*filterFrom(R"(["==", ["get", "kind"], "keep"])"));
    ASSERT_TRUE(runOneFrame(test, "the frame after the filter"));
    EXPECT_EQ(0u, drawnCircles(test)) << "the filter was not drawn in the frame that took it";
}

// The same with a body held on the lane (both lane threads held): the
// render thread lays the filter out with the body the tiles hold, without
// waiting for the lane, and the held body lands later, at the generation
// that covers the filter too.
TEST(LabelLane, AFilterChangeIsDrawnInTheNextFrameWithABodyHeldOnTheLane) {
    LaneMap test(circleStyleJSON());
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", 1); }, "the lane source laid out"));
    ASSERT_TRUE(runOneFrame(test, "a frame with the body"));
    ASSERT_EQ(4u, drawnCircles(test));

    LaneBlock block;
    ASSERT_TRUE(block.allHeld) << "the lane's two threads were not both held";
    // A new body, parsed on the lane as the Android binding does: held.
    auto& source = test.labels();
    const uint64_t body = source.nextDataGeneration();
    const std::string kept = R"({"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {"kind": "keep"}, "geometry": {"type": "Point", "coordinates": [0, 0]}}]})";
    source.getParseScheduler()->scheduleAndReplyValue(
        util::SimpleIdentity::Empty,
        [kept, slicer = source.getSliceScheduler(), options = source.impl().getOptions()] {
            return GeoJSONData::parse(kept, slicer, options, "labels");
        },
        [&test, body](GeoJSONData::ParseResult parsed) {
            test.labels().setGeoJSONData(std::get<std::shared_ptr<GeoJSONData>>(std::move(parsed)), body);
        });

    test.map->getStyle().getLayer("circles")->setFilter(*filterFrom(R"(["==", ["get", "kind"], "keep"])"));
    const uint64_t latest = source.getLatestGeneration();
    ASSERT_GT(latest, body);
    ASSERT_TRUE(runOneFrame(test, "the frame after the filter, with the lane held"));
    EXPECT_EQ(0u, drawnCircles(test)) << "the filter was not drawn on the old body in the frame that took it";
    EXPECT_FALSE(test.observer.has("labels", body)) << "reported the held body's generation";
    EXPECT_FALSE(test.observer.has("labels", latest)) << "reported the latest generation with the old body";

    block.release();
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", latest); }, "the held body laid out"));
    ASSERT_TRUE(runOneFrame(test, "a frame with the held body"));
    EXPECT_EQ(1u, drawnCircles(test)) << "the held body with the filter";
}

// A body that has reached the tiles, whose layout waits on the held lane: the
// filter is laid out inline with that newest body, and drawn in the next
// frame; and every frame after, until the lane's work is done, keeps it.
TEST(LabelLane, AFilterChangeIsLaidOutWithTheNewestBodyTheTilesHold) {
    Recording recording;
    LaneMap test(circleStyleJSON());
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", 1); }, "the lane source laid out"));
    ASSERT_TRUE(runOneFrame(test, "a frame with the body"));
    ASSERT_EQ(4u, drawnCircles(test));

    LaneBlock block;
    ASSERT_TRUE(block.allHeld) << "the lane's two threads were not both held";
    const std::string kept = R"({"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {"kind": "keep"}, "geometry": {"type": "Point", "coordinates": [0, 0]}},
        {"type": "Feature", "properties": {"kind": "drop"}, "geometry": {"type": "Point", "coordinates": [10, 10]}}]})";
    conversion::Error error;
    const std::size_t stepsBefore = recording.steps().size();
    test.labels().setGeoJSON(*conversion::parseGeoJSON(kept, error));
    // The new body's cut reaches the tiles, and their workers' layouts wait
    // on the held lane.
    ASSERT_TRUE(runUntil(
        test.loop,
        [&] {
            const auto& steps = recording.steps();
            return std::any_of(steps.begin() + static_cast<std::ptrdiff_t>(stepsBefore),
                               steps.end(),
                               [](const auto& s) { return s.sourceID == "labels" && s.name == "hop-data"; });
        },
        "the new body's tiles sent to their workers"));

    test.map->getStyle().getLayer("circles")->setFilter(*filterFrom(R"(["==", ["get", "kind"], "keep"])"));
    const uint64_t latest = test.labels().getLatestGeneration();
    ASSERT_TRUE(runOneFrame(test, "the frame after the filter, with the lane held"));
    EXPECT_EQ(1u, drawnCircles(test)) << "not the newest body with the filter";

    EXPECT_TRUE(test.observer.has("labels", latest)) << "the inline layout of the newest body was not reported";

    // The lane's layouts, for the old layers and the new, come after: once
    // the lane has done them, the frame they bring still draws the inline
    // layout. (Which of them a frame sees is timing; the rule that drops the
    // older one is GeoJSONTile.ALayoutOlderThanTheOneTakenIsDropped.)
    block.release();
    Scheduler::GetLabelLane()->waitForEmpty(test.frontend->getThreadPool().tag);
    ASSERT_TRUE(runOneFrame(test, "the frame the lane's layouts bring"));
    EXPECT_EQ(1u, drawnCircles(test)) << "a frame drew a layout older than the inline one";
}

// The landing limit (`GeoJSONSource::setLandingLimit`): a body above it is
// cut and laid out, and reported laid out, but the tiles keep drawing the
// newest body at or below it; raising the limit lands it in the frame that
// takes the change.

namespace {

const std::string kOnePoint = R"({"type": "FeatureCollection", "features": [
    {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [0, 0]}}]})";
const std::string kTwoPoints = R"({"type": "FeatureCollection", "features": [
    {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [-20, 20]}},
    {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [20, -20]}}]})";

GeoJSON body(const std::string& text) {
    conversion::Error error;
    auto geoJSON = conversion::parseGeoJSON(text, error);
    assert(geoJSON);
    return *geoJSON;
}

/** The symbols of "labels" the last frame placed (a symbol query reads the collision index). */
std::size_t placedLabels(LaneMap& test) {
    RenderedQueryOptions options;
    options.layerIDs = std::vector<std::string>{"labels"};
    return test.frontend->getRenderer()->queryRenderedFeatures(ScreenBox{{0, 0}, {256, 256}}, options).size();
}

/** Placement transitions off, as the Viatrail app runs (its labels have no fade). */
void placementTransitionsOff(LaneMap& test) {
    TransitionOptions transitions = test.map->getStyle().getTransitionOptions();
    transitions.enablePlacementTransitions = false;
    test.map->getStyle().setTransitionOptions(transitions);
}

} // namespace

TEST(LabelLane, ABodyAboveTheLimitIsLaidOutButDrawnOnlyInTheFrameThatRaisesIt) {
    LaneMap test(circleStyleJSON());
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", 1); }, "the lane source laid out"));
    test.map->triggerRepaint(); // the laid-out frame may be the last one
    ASSERT_TRUE(runOneFrame(test, "a frame with the body"));
    ASSERT_EQ(4u, drawnCircles(test));

    test.labels().setLandingLimit(test.labels().getLatestGeneration());
    const uint64_t one = test.labels().setGeoJSON(body(kOnePoint));
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", one); }, "the held body laid out"));
    for (int i = 0; i < 3; ++i) {
        test.map->triggerRepaint(); // a still map draws no frame of its own
        ASSERT_TRUE(runOneFrame(test, "a frame while held"));
        EXPECT_EQ(4u, drawnCircles(test)) << "a held body was drawn, frame " << i;
    }

    test.labels().setLandingLimit(one);
    ASSERT_TRUE(runOneFrame(test, "the frame that takes the release"));
    EXPECT_EQ(1u, drawnCircles(test)) << "the released body was not drawn in the frame that took the release";
}

// A fast stream (audit C2): two bodies held; raising the limit to the first
// lands the first, not the newest, and the newest stays held.
TEST(LabelLane, RaisingTheLimitLandsTheBodyAtItNotANewerOne) {
    LaneMap test(circleStyleJSON());
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", 1); }, "the lane source laid out"));
    test.map->triggerRepaint(); // the laid-out frame may be the last one
    ASSERT_TRUE(runOneFrame(test, "a frame with the body"));
    ASSERT_EQ(4u, drawnCircles(test));

    test.labels().setLandingLimit(test.labels().getLatestGeneration());
    const uint64_t one = test.labels().setGeoJSON(body(kOnePoint));
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", one); }, "the first held body laid out"));
    const uint64_t two = test.labels().setGeoJSON(body(kTwoPoints));
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", two); }, "the second held body laid out"));
    test.map->triggerRepaint();
    ASSERT_TRUE(runOneFrame(test, "a frame with both held"));
    EXPECT_EQ(4u, drawnCircles(test));

    test.labels().setLandingLimit(one);
    ASSERT_TRUE(runOneFrame(test, "the frame that lands the first"));
    EXPECT_EQ(1u, drawnCircles(test)) << "the limit landed a body other than the one at it";

    test.labels().setLandingLimit(std::nullopt);
    ASSERT_TRUE(runOneFrame(test, "the frame that lands the newest"));
    EXPECT_EQ(2u, drawnCircles(test)) << "clearing the limit did not land the newest body";
}

// A tile made during a hold (a pan; audit I1) draws the landed body, not
// nothing: the pan goes where no tile was ever loaded, so no parent tile can
// stand in for it.
TEST(LabelLane, ATileMadeDuringAHoldDrawsTheLandedBody) {
    // Zoom 5 from the start: only tiles near (0, 0) are ever loaded.
    std::string style = circleStyleJSON();
    style.replace(style.find("\"zoom\": 1.5"), std::string("\"zoom\": 1.5").size(), "\"zoom\": 5");
    LaneMap test(style);
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", 1); }, "the lane source laid out"));
    // The landed body: four points far east, off the screen.
    const std::string east = R"({"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [134, 1]}},
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [136, 1]}},
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [134, -1]}},
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [136, -1]}}]})";
    const uint64_t landedBody = test.labels().setGeoJSON(body(east));
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", landedBody); }, "the east body laid out"));
    // Held: one point, also far east.
    test.labels().setLandingLimit(landedBody);
    const std::string eastOne = R"({"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [135, 0]}}]})";
    test.labels().setGeoJSON(body(eastOne));
    // The pan: new tiles, made while the hold is on, where no tile (and no
    // parent tile) was ever loaded.
    test.map->jumpTo(CameraOptions().withCenter(LatLng{0, 135}));
    bool drewHeld = false;
    ASSERT_TRUE(runUntil(
        test.loop,
        [&] {
            const std::size_t drawn = drawnCircles(test);
            drewHeld = drewHeld || drawn == 1;
            return drawn == 4;
        },
        "the new tiles draw the landed body"));
    EXPECT_FALSE(drewHeld) << "a new tile drew the held body before the release";
    // They lay out the held body after the landed one: let the lane finish.
    Scheduler::GetLabelLane()->waitForEmpty(test.frontend->getThreadPool().tag);
    test.map->triggerRepaint();
    ASSERT_TRUE(runOneFrame(test, "a frame after the lane's work"));
    EXPECT_EQ(4u, drawnCircles(test)) << "the new tiles drew the held body before the release";
    test.labels().setLandingLimit(std::nullopt);
    ASSERT_TRUE(runOneFrame(test, "the frame that takes the release"));
    EXPECT_EQ(1u, drawnCircles(test)) << "the new tiles' held body did not land with the release";
}

// A limit below the landed body (R1, issue #166): a tile made then lays out
// the landed body, which the limit keeps aside. That layout must not block
// the newest body: the tile lays it out too, the source is reported laid out
// at its generation, and clearing the limit draws it.
TEST(LabelLane, ATileMadeWithTheLimitBelowTheLandedBodyIsReportedAtTheNewestBody) {
    std::string style = circleStyleJSON();
    style.replace(style.find("\"zoom\": 1.5"), std::string("\"zoom\": 1.5").size(), "\"zoom\": 5");
    LaneMap test(style);
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", 1); }, "the lane source laid out"));
    const std::string east = R"({"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [134, 1]}},
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [136, 1]}},
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [134, -1]}},
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [136, -1]}}]})";
    const uint64_t landedBody = test.labels().setGeoJSON(body(east));
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", landedBody); }, "the east body laid out"));
    // The limit goes below the landed body.
    test.labels().setLandingLimit(landedBody - 1);
    const std::string eastOne = R"({"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [135, 0]}}]})";
    const uint64_t newest = test.labels().setGeoJSON(body(eastOne));
    // The pan: new tiles, made with the limit below the landed body.
    test.map->jumpTo(CameraOptions().withCenter(LatLng{0, 135}));
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", newest); }, "the newest body reported"));
    test.labels().setLandingLimit(std::nullopt);
    ASSERT_TRUE(runOneFrame(test, "the frame that takes the release"));
    EXPECT_EQ(1u, drawnCircles(test)) << "the new tiles' newest body did not land with the release";
}

// A new source object under the same id (a style rebuild) counts its
// generations from low again. The body the old object landed is not the new
// object's landed body: a tile made under the new object's hold must not lay
// it out, and releasing the hold must draw the new object's newest body.
TEST(LabelLane, ANewSourceUnderTheSameIDDoesNotDrawTheOldSourcesLandedBody) {
    std::string style = circleStyleJSON();
    style.replace(style.find("\"zoom\": 1.5"), std::string("\"zoom\": 1.5").size(), "\"zoom\": 5");
    LaneMap test(style);
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", 1); }, "the lane source laid out"));
    // The old object lands four points far east at generation 3.
    const std::string east = R"({"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [134, 1]}},
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [136, 1]}},
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [134, -1]}},
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [136, -1]}}]})";
    test.labels().setGeoJSON(body(east));
    const uint64_t oldLanded = test.labels().setGeoJSON(body(east));
    ASSERT_EQ(3u, oldLanded);
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", oldLanded); }, "the old body laid out"));

    // The rebuild: a new object under the same id, held from its first body.
    auto& mapStyle = test.map->getStyle();
    mapStyle.removeLayer("circles");
    mapStyle.removeSource("labels");
    auto fresh = std::make_unique<GeoJSONSource>("labels", laneOptions());
    fresh->setLandingLimit(0);
    fresh->setGeoJSON(body(R"({"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [135, 0]}}]})"));
    const uint64_t newest = fresh->setGeoJSON(body(R"({"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [134.5, 0]}},
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [135.5, 0]}}]})"));
    ASSERT_EQ(2u, newest);
    mapStyle.addSource(std::move(fresh));
    mapStyle.addLayer(std::make_unique<CircleLayer>("circles", "labels"));
    const std::size_t reportsBefore = test.observer.laidOut.size();

    // The pan: new tiles, made under the new object's hold.
    test.map->jumpTo(CameraOptions().withCenter(LatLng{0, 135}));
    ASSERT_TRUE(runUntil(
        test.loop,
        [&] {
            return std::find(test.observer.laidOut.begin() + static_cast<std::ptrdiff_t>(reportsBefore),
                             test.observer.laidOut.end(),
                             std::make_pair(std::string("labels"), newest)) != test.observer.laidOut.end();
        },
        "the new object's newest body reported"));
    Scheduler::GetLabelLane()->waitForEmpty(test.frontend->getThreadPool().tag);
    test.map->triggerRepaint();
    ASSERT_TRUE(runOneFrame(test, "a frame while held"));
    EXPECT_EQ(0u, drawnCircles(test)) << "a held tile drew a body before the release";

    test.labels().setLandingLimit(std::nullopt);
    ASSERT_TRUE(runOneFrame(test, "the frame that takes the release"));
    EXPECT_EQ(2u, drawnCircles(test)) << "the release did not draw the new object's newest body";
}

// The same, in the tiles the old object left (no pan).
TEST(LabelLane, ANewSourceUnderTheSameIDLandsItsBodyInTheTilesTheOldOneLeft) {
    std::string style = circleStyleJSON();
    style.replace(style.find("\"zoom\": 1.5"), std::string("\"zoom\": 1.5").size(), "\"zoom\": 5");
    style.replace(style.find("\"center\": [0, 0]"), std::string("\"center\": [0, 0]").size(), "\"center\": [135, 0]");
    LaneMap test(style);
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", 1); }, "the lane source laid out"));
    // The old object lands four points far east at generation 3.
    const std::string east = R"({"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [134, 1]}},
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [136, 1]}},
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [134, -1]}},
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [136, -1]}}]})";
    test.labels().setGeoJSON(body(east));
    const uint64_t oldLanded = test.labels().setGeoJSON(body(east));
    ASSERT_EQ(3u, oldLanded);
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", oldLanded); }, "the old body laid out"));
    test.map->triggerRepaint();
    ASSERT_TRUE(runOneFrame(test, "a frame with the old body"));
    ASSERT_EQ(4u, drawnCircles(test));

    // The rebuild: a new object under the same id, held from its first body.
    auto& mapStyle = test.map->getStyle();
    mapStyle.removeLayer("circles");
    mapStyle.removeSource("labels");
    auto fresh = std::make_unique<GeoJSONSource>("labels", laneOptions());
    fresh->setLandingLimit(0);
    fresh->setGeoJSON(body(R"({"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [135, 0]}}]})"));
    const uint64_t newest = fresh->setGeoJSON(body(R"({"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [134.5, 0]}},
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [135.5, 0]}}]})"));
    ASSERT_EQ(2u, newest);
    mapStyle.addSource(std::move(fresh));
    mapStyle.addLayer(std::make_unique<CircleLayer>("circles", "labels"));
    const std::size_t reportsBefore = test.observer.laidOut.size();

    // No pan: the tiles the old object laid out take the new bodies.
    ASSERT_TRUE(runUntil(
        test.loop,
        [&] {
            return std::find(test.observer.laidOut.begin() + static_cast<std::ptrdiff_t>(reportsBefore),
                             test.observer.laidOut.end(),
                             std::make_pair(std::string("labels"), newest)) != test.observer.laidOut.end();
        },
        "the new object's newest body reported"));
    Scheduler::GetLabelLane()->waitForEmpty(test.frontend->getThreadPool().tag);
    test.map->triggerRepaint();
    ASSERT_TRUE(runOneFrame(test, "a frame while held"));
    // A held tile keeps drawing the layout it drew (no blank frame), as in a
    // hold within one object.
    EXPECT_EQ(4u, drawnCircles(test)) << "a held tile did not keep the layout it drew";

    test.labels().setLandingLimit(std::nullopt);
    ASSERT_TRUE(runOneFrame(test, "the frame that takes the release"));
    EXPECT_EQ(2u, drawnCircles(test)) << "the release did not draw the new object's newest body";
}


// The old object holds a body above its limit, then a rebuild makes a new
// object under the same id, held from its first body (audit of
// 0be25ff3a7b2). The old object's held body was never released: it must not
// be drawn during the new object's hold, and the release draws the new body.
TEST(LabelLane, ARebuildDoesNotDrawTheOldObjectsHeldBody) {
    std::string style = circleStyleJSON();
    style.replace(style.find("\"zoom\": 1.5"), std::string("\"zoom\": 1.5").size(), "\"zoom\": 5");
    style.replace(style.find("\"center\": [0, 0]"), std::string("\"center\": [0, 0]").size(), "\"center\": [135, 0]");
    LaneMap test(style);
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", 1); }, "the lane source laid out"));
    const std::string east = R"({"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [134, 1]}},
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [136, 1]}},
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [134, -1]}},
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [136, -1]}}]})";
    const uint64_t landed = test.labels().setGeoJSON(body(east));
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", landed); }, "the old body laid out"));
    test.map->triggerRepaint();
    ASSERT_TRUE(runOneFrame(test, "a frame with the old body"));
    ASSERT_EQ(4u, drawnCircles(test));

    // The old object holds a one-point body above its limit.
    test.labels().setLandingLimit(landed);
    const uint64_t held = test.labels().setGeoJSON(body(R"({"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [135, 0]}}]})"));
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", held); }, "the held body laid out"));
    Scheduler::GetLabelLane()->waitForEmpty(test.frontend->getThreadPool().tag);
    test.map->triggerRepaint();
    ASSERT_TRUE(runOneFrame(test, "a frame in the old hold"));
    ASSERT_EQ(4u, drawnCircles(test)) << "the old hold did not hold";

    // The rebuild.
    auto& mapStyle = test.map->getStyle();
    mapStyle.removeLayer("circles");
    mapStyle.removeSource("labels");
    auto fresh = std::make_unique<GeoJSONSource>("labels", laneOptions());
    fresh->setLandingLimit(0);
    fresh->setGeoJSON(body(R"({"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [135, 0.5]}}]})"));
    const uint64_t newest = fresh->setGeoJSON(body(R"({"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [134.5, 0]}},
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [135.5, 0]}}]})"));
    mapStyle.addSource(std::move(fresh));
    mapStyle.addLayer(std::make_unique<CircleLayer>("circles", "labels"));
    const std::size_t reportsBefore = test.observer.laidOut.size();
    ASSERT_TRUE(runUntil(
        test.loop,
        [&] {
            return std::find(test.observer.laidOut.begin() + static_cast<std::ptrdiff_t>(reportsBefore),
                             test.observer.laidOut.end(),
                             std::make_pair(std::string("labels"), newest)) != test.observer.laidOut.end();
        },
        "the new object's newest body reported"));
    Scheduler::GetLabelLane()->waitForEmpty(test.frontend->getThreadPool().tag);
    test.map->triggerRepaint();
    ASSERT_TRUE(runOneFrame(test, "a frame in the new hold"));
    EXPECT_EQ(4u, drawnCircles(test)) << "the new hold drew something other than the drawn layout (1 = the old held body)";

    test.labels().setLandingLimit(std::nullopt);
    ASSERT_TRUE(runOneFrame(test, "the release"));
    EXPECT_EQ(2u, drawnCircles(test));
}


// Rebuilds under the same id (audits of 0be25ff3a7b2 and 251b26064057): the
// drawn body and any body the old object released carry over; a body it
// held never lands.
namespace {

const char* kFour = R"({"type": "FeatureCollection", "features": [
    {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [134, 1]}},
    {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [136, 1]}},
    {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [134, -1]}},
    {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [136, -1]}}]})";
const char* kOne = R"({"type": "FeatureCollection", "features": [
    {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [135, 0]}}]})";
const char* kTwo = R"({"type": "FeatureCollection", "features": [
    {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [134.5, 0]}},
    {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [135.5, 0]}}]})";
const char* kThree = R"({"type": "FeatureCollection", "features": [
    {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [134.5, 0.5]}},
    {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [135, 0.5]}},
    {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [135.5, 0.5]}}]})";

std::string rebuildStyle() {
    std::string style = circleStyleJSON();
    style.replace(style.find("\"zoom\": 1.5"), std::string("\"zoom\": 1.5").size(), "\"zoom\": 5");
    style.replace(style.find("\"center\": [0, 0]"), std::string("\"center\": [0, 0]").size(), "\"center\": [135, 0]");
    return style;
}

// Old object: 4 drawn, then (if hold) a one-point body held above its limit.
// waitHeld false leaves the held body in flight.
void oldObjectState(LaneMap& test, bool hold, bool waitHeld) {
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", 1); }, "the lane source laid out"));
    const uint64_t landed = test.labels().setGeoJSON(body(kFour));
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", landed); }, "old body"));
    test.map->triggerRepaint();
    ASSERT_TRUE(runOneFrame(test, "frame old"));
    ASSERT_EQ(4u, drawnCircles(test));
    if (!hold) return;
    test.labels().setLandingLimit(landed);
    const uint64_t held = test.labels().setGeoJSON(body(kOne));
    if (!waitHeld) {
        test.map->triggerRepaint();
        ASSERT_TRUE(runOneFrame(test, "frame with the held body in flight"));
        return;
    }
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", held); }, "held body"));
    Scheduler::GetLabelLane()->waitForEmpty(test.frontend->getThreadPool().tag);
    test.map->triggerRepaint();
    ASSERT_TRUE(runOneFrame(test, "frame old hold"));
    ASSERT_EQ(4u, drawnCircles(test));
}

// A rebuild: returns the new object's newest generation; sets reportsBefore.
uint64_t rebuildUnderTheSameID(LaneMap& test, std::optional<uint64_t> limit, const char* last, std::size_t& reportsBefore) {
    auto& mapStyle = test.map->getStyle();
    mapStyle.removeLayer("circles");
    mapStyle.removeSource("labels");
    auto fresh = std::make_unique<GeoJSONSource>("labels", laneOptions());
    if (limit) fresh->setLandingLimit(*limit);
    fresh->setGeoJSON(body(R"({"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [135, 0.25]}}]})"));
    const uint64_t newest = fresh->setGeoJSON(body(last));
    mapStyle.addSource(std::move(fresh));
    mapStyle.addLayer(std::make_unique<CircleLayer>("circles", "labels"));
    reportsBefore = test.observer.laidOut.size();
    return newest;
}

bool reportedSince(LaneMap& test, uint64_t newest, std::size_t reportsBefore, const std::string& what) {
    return runUntil(
        test.loop,
        [&] {
            return std::find(test.observer.laidOut.begin() + static_cast<std::ptrdiff_t>(reportsBefore),
                             test.observer.laidOut.end(),
                             std::make_pair(std::string("labels"), newest)) != test.observer.laidOut.end();
        },
        what);
}

std::size_t heldFrameCircles(LaneMap& test) {
    Scheduler::GetLabelLane()->waitForEmpty(test.frontend->getThreadPool().tag);
    test.map->triggerRepaint();
    EXPECT_TRUE(runOneFrame(test, "held frame"));
    return drawnCircles(test);
}
} // namespace

// Two rebuilds in a row, each held, the old object held too.
TEST(LabelLane, TwoRebuildsInARowDrawOnlyTheDrawnBodyUntilTheRelease) {
    LaneMap test(rebuildStyle());
    oldObjectState(test, true, true);
    std::size_t before = 0;
    const uint64_t a = rebuildUnderTheSameID(test, 0, kTwo, before);
    ASSERT_TRUE(reportedSince(test, a, before, "rebuild A reported"));
    EXPECT_EQ(4u, heldFrameCircles(test)) << "rebuild A hold";
    const uint64_t b = rebuildUnderTheSameID(test, 0, kThree, before);
    ASSERT_TRUE(reportedSince(test, b, before, "rebuild B reported"));
    EXPECT_EQ(4u, heldFrameCircles(test)) << "rebuild B hold (2 = A's held body)";
    test.labels().setLandingLimit(std::nullopt);
    ASSERT_TRUE(runOneFrame(test, "release"));
    EXPECT_EQ(3u, drawnCircles(test));
}

// Two rebuilds with no frame between them.
TEST(LabelLane, TwoRebuildsInOneTurnDrawOnlyTheDrawnBodyUntilTheRelease) {
    LaneMap test(rebuildStyle());
    oldObjectState(test, true, true);
    std::size_t before = 0;
    rebuildUnderTheSameID(test, 0, kTwo, before);
    const uint64_t b = rebuildUnderTheSameID(test, 0, kThree, before);
    ASSERT_TRUE(reportedSince(test, b, before, "rebuild B reported"));
    EXPECT_EQ(4u, heldFrameCircles(test));
    test.labels().setLandingLimit(std::nullopt);
    ASSERT_TRUE(runOneFrame(test, "release"));
    EXPECT_EQ(3u, drawnCircles(test));
}

// The old held body still in flight at the rebuild.
TEST(LabelLane, ARebuildWithTheOldHeldBodyInFlightDoesNotDrawIt) {
    LaneMap test(rebuildStyle());
    oldObjectState(test, true, false);
    std::size_t before = 0;
    const uint64_t n = rebuildUnderTheSameID(test, 0, kTwo, before);
    ASSERT_TRUE(reportedSince(test, n, before, "reported"));
    EXPECT_EQ(4u, heldFrameCircles(test)) << "1 = the old held body";
    test.labels().setLandingLimit(std::nullopt);
    ASSERT_TRUE(runOneFrame(test, "release"));
    EXPECT_EQ(2u, drawnCircles(test));
}

// A rebuild with no hold on the new object: reported, drawn at once.
TEST(LabelLane, ARebuildWithNoNewHoldDrawsTheNewBody) {
    LaneMap test(rebuildStyle());
    oldObjectState(test, true, true);
    std::size_t before = 0;
    const uint64_t n = rebuildUnderTheSameID(test, std::nullopt, kTwo, before);
    ASSERT_TRUE(reportedSince(test, n, before, "reported"));
    EXPECT_EQ(2u, heldFrameCircles(test));
}

// A rebuild, then a pan in the same turn (some tiles kept, some new).
TEST(LabelLane, ARebuildDuringAPanDoesNotDrawAHeldBody) {
    LaneMap test(rebuildStyle());
    oldObjectState(test, true, true);
    std::size_t before = 0;
    const uint64_t n = rebuildUnderTheSameID(test, 0, kTwo, before);
    test.map->jumpTo(CameraOptions().withCenter(LatLng{2, 136}));
    ASSERT_TRUE(reportedSince(test, n, before, "reported after the pan"));
    const std::size_t held = heldFrameCircles(test);
    EXPECT_NE(1u, held) << "the old held body drew";
    {
        RenderedQueryOptions options;
        options.layerIDs = std::vector<std::string>{"circles"};
        for (const auto& f : test.frontend->getRenderer()->queryRenderedFeatures(ScreenBox{{0, 0}, {256, 256}}, options)) {
            const auto& p = f.geometry.get<mapbox::geometry::point<double>>();
            EXPECT_NE(0.0, std::round(p.y * 4) / 4) << "a lat-0 point (new held body or old held body) drew in the hold";
        }
    }
    test.labels().setLandingLimit(std::nullopt);
    ASSERT_TRUE(runOneFrame(test, "release"));
    EXPECT_EQ(2u, drawnCircles(test));
}

// The old object at no hold, a body in flight, rebuild: no wait for ever.
TEST(LabelLane, ARebuildLandsABodyTheOldObjectReleasedInEveryTile) {
    LaneMap test(rebuildStyle());
    oldObjectState(test, false, false);
    test.labels().setGeoJSON(body(kOne));
    test.map->triggerRepaint();
    ASSERT_TRUE(runOneFrame(test, "frame with the body in flight"));
    std::size_t before = 0;
    const uint64_t n = rebuildUnderTheSameID(test, 0, kTwo, before);
    ASSERT_TRUE(reportedSince(test, n, before, "reported"));
    // The old object released kOne (it had no limit), so every tile lands it,
    // also one whose layout was still on its way at the rebuild. The count
    // alone passes when a tile keeps the 4-point body, so check the point.
    EXPECT_EQ(1u, heldFrameCircles(test)) << "a tile lost the released body";
    RenderedQueryOptions options;
    options.layerIDs = std::vector<std::string>{"circles"};
    const auto drawn = test.frontend->getRenderer()->queryRenderedFeatures(ScreenBox{{0, 0}, {256, 256}}, options);
    for (const auto& feature : drawn) {
        const auto& point = feature.geometry.get<mapbox::geometry::point<double>>();
        EXPECT_NEAR(135.0, point.x, 0.01) << "not the released body";
        EXPECT_NEAR(0.0, point.y, 0.01) << "not the released body";
    }
    test.labels().setLandingLimit(std::nullopt);
    ASSERT_TRUE(runOneFrame(test, "release"));
    EXPECT_EQ(2u, drawnCircles(test));
}

// Placement transitions OFF is what makes it one frame: `PlacementController::
// placementIsRecent` is then false every frame, so the landed symbols are
// placed in the frame that lands them. With them on (MapLibre's default),
// placement runs at most once a period (the probe measured 22 frames), so the
// app's one-frame labels depend on its no-fade setting. A test of the
// transitions-on case cannot pin a frame count: the period is time, not
// frames (audit N1).
TEST(LabelLane, AReleasedBodysSymbolsArePlacedInTheReleaseFrameWithPlacementTransitionsOff) {
    LaneMap test;
    placementTransitionsOff(test);
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", 1); }, "the lane source laid out"));
    ASSERT_TRUE(runUntil(test.loop, [&] { return placedLabels(test) == 4u; }, "the body's four labels placed"));

    test.labels().setLandingLimit(test.labels().getLatestGeneration());
    const uint64_t one = test.labels().setGeoJSON(body(kOnePoint));
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("labels", one); }, "the held body laid out"));
    for (int i = 0; i < 3; ++i) {
        test.map->triggerRepaint(); // a still map draws no frame of its own
        ASSERT_TRUE(runOneFrame(test, "a frame while held"));
        EXPECT_EQ(4u, placedLabels(test)) << "a held body's labels were placed, frame " << i;
    }

    test.labels().setLandingLimit(std::nullopt);
    ASSERT_TRUE(runOneFrame(test, "the frame that takes the release"));
    EXPECT_EQ(1u, placedLabels(test)) << "the released labels were not placed in the frame that took the release";
}
