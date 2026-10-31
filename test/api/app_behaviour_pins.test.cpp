// Pins: fork behaviours that the Viatrail app relies on (Viatrail #170 F1,
// spec F5). Each test pins what the fork does TODAY, so a fork update that
// changes one of them fails here before it reaches the app. Each wait ends on
// an event (a frame, a tile action, a laid-out report); the 30 s bound only
// turns a missing event into a failure instead of a hang.
//
// Pinned elsewhere: the landing limit and the laid-out report (LabelLane.*),
// and a filter change on a lane source drawn in the next frame
// (LabelLane.AFilterChangeIsDrawnInTheNextFrame).

#include <mln/test/map_adapter.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/util.hpp>

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/map_observer.hpp>
#include <mln/map/map_options.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/style/conversion/filter.hpp>
#include <mln/style/conversion/geojson.hpp>
#include <mln/style/conversion/json.hpp>
#include <mln/style/image.hpp>
#include <mln/style/layer.hpp>
#include <mln/style/rapidjson_conversion.hpp>
#include <mln/style/sources/geojson_source.hpp>
#include <mln/style/style.hpp>
#include <mln/style/transition_options.hpp>
#include <mln/tile/tile_operation.hpp>
#include <mln/util/io.hpp>
#include <mln/util/logging.hpp>
#include <mln/util/rapidjson.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/timer.hpp>

#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>

using namespace mln;
using namespace mln::style;

namespace {

constexpr auto kBound = std::chrono::seconds(30);

#if defined(__APPLE__)
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void*);
#endif

/** Runs the loop until `done()`. False, with a failure, if it has not come within the bound. */
bool runUntil(util::RunLoop& loop, const std::function<bool()>& done, const std::string& what) {
    bool late = false;
    util::Timer watchdog;
    watchdog.start(kBound, Duration::zero(), [&] { late = true; });
    while (!done() && !late) {
        // On Apple, inside an autorelease pool: the headless Metal backend's
        // command buffers are autoreleased (see label_lane.test.cpp).
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

/** Holds every thread of the shared worker pool until released; confirms all four are held. */
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

class PinObserver final : public MapObserver {
public:
    void onSourceLaidOut(const std::string& sourceID, uint64_t generation) override {
        laidOut.emplace_back(sourceID, generation);
    }
    void onDidFinishRenderingFrame(const RenderFrameStatus& status) override {
        ++frames;
        lastFrameFull = status.mode == RenderMode::Full;
        if (status.placementChanged) ++placements;
    }
    void onTileAction(TileOperation op, const OverscaledTileID& id, const std::string& sourceID) override {
        tileActions.push_back({.op = op, .id = id, .sourceID = sourceID});
    }

    bool has(const std::string& sourceID, uint64_t generation) const {
        return std::ranges::find(laidOut, std::make_pair(sourceID, generation)) != laidOut.end();
    }
    bool reported(const std::string& sourceID) const {
        return std::ranges::any_of(laidOut, [&](const auto& entry) { return entry.first == sourceID; });
    }
    std::size_t count(TileOperation op, const std::string& sourceID, std::size_t from = 0) const {
        return static_cast<std::size_t>(std::count_if(tileActions.begin() + static_cast<std::ptrdiff_t>(from),
                                                      tileActions.end(),
                                                      [&](const auto& a) { return a.op == op && a.sourceID == sourceID; }));
    }
    /** From `from` on: every tile of `sourceID` that started a parse has ended it, and one did start. */
    bool everyStartedParseEnded(const std::string& sourceID, std::size_t from) const {
        std::map<OverscaledTileID, int> open;
        bool started = false;
        for (auto it = tileActions.begin() + static_cast<std::ptrdiff_t>(from); it != tileActions.end(); ++it) {
            if (it->sourceID != sourceID) continue;
            if (it->op == TileOperation::StartParse) {
                open[it->id] = 1;
                started = true;
            } else if (it->op == TileOperation::EndParse) {
                open.erase(it->id);
            }
        }
        return started && open.empty();
    }

    struct TileAction {
        TileOperation op;
        OverscaledTileID id;
        std::string sourceID;
    };
    std::vector<std::pair<std::string, uint64_t>> laidOut;
    std::vector<TileAction> tileActions;
    int frames = 0;
    int placements = 0;
    bool lastFrameFull = false;
};

/** A continuous headless map, 256 x 256, with the test glyphs. */
class PinMap {
public:
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>();
    PinObserver observer;
    std::unique_ptr<HeadlessFrontend> frontend = std::make_unique<HeadlessFrontend>(Size{256, 256}, 1.0f);
    std::unique_ptr<MapAdapter> map = std::make_unique<MapAdapter>(
        *frontend,
        observer,
        fileSource,
        MapOptions().withMapMode(MapMode::Continuous).withSize(frontend->getSize()).withPixelRatio(1.0));

    explicit PinMap(const std::string& style, bool placementTransitions = false) {
        fileSource->glyphsResponse = [](const Resource&) {
            Response response;
            response.data = std::make_shared<std::string>(util::read_file("test/fixtures/resources/glyphs.pbf"));
            return response;
        };
        map->getStyle().loadJSON(style);
        if (!placementTransitions) {
            // As the Viatrail app runs (Android MAP_STYLE_TRANSITION): no
            // paint transition and no label fade, so placement runs in every
            // frame. The style's first update went out before this, so the
            // first placement still keeps MapLibre's period: `placed` waits
            // for a placement.
            map->getStyle().setTransitionOptions(TransitionOptions(Duration::zero(), Duration::zero(), false));
        }
    }

    /** Runs the loop until a placement runs in a frame that begins after this call. */
    bool placed(const std::string& what) {
        const int start = observer.placements;
        int seen = observer.frames;
        map->triggerRepaint();
        return runUntil(
            loop,
            [&] {
                if (observer.placements > start) return true;
                if (observer.frames != seen) {
                    // A still map draws no frame of its own.
                    seen = observer.frames;
                    map->triggerRepaint();
                }
                return false;
            },
            what);
    }

    GeoJSONSource& source(const std::string& id) { return *map->getStyle().getSource(id)->as<GeoJSONSource>(); }
    Layer& layer(const std::string& id) { return *map->getStyle().getLayer(id); }

    /** The features of `layerID` the last frame drew (or placed, for a symbol layer) in `box`. */
    std::vector<Feature> query(const std::string& layerID, ScreenBox box = {{0, 0}, {256, 256}}) {
        RenderedQueryOptions options;
        options.layerIDs = std::vector<std::string>{layerID};
        return frontend->getRenderer()->queryRenderedFeatures(box, options);
    }
    std::size_t count(const std::string& layerID, ScreenBox box = {{0, 0}, {256, 256}}) {
        return query(layerID, box).size();
    }

    /** Runs the loop until one more frame is drawn; a still map is asked for one. */
    bool frame(const std::string& what) {
        const int before = observer.frames;
        map->triggerRepaint();
        return runUntil(loop, [&] { return observer.frames > before; }, what);
    }
};

GeoJSON geoJSON(const std::string& text) {
    conversion::Error error;
    auto parsed = conversion::parseGeoJSON(text, error);
    EXPECT_TRUE(parsed) << error.message;
    return *parsed;
}

Filter filter(const std::string& json) {
    conversion::Error error;
    auto parsed = conversion::convertJSON<Filter>(json, error);
    EXPECT_TRUE(parsed) << error.message;
    return *parsed;
}

void setProperty(Layer& layer, const std::string& name, const std::string& json) {
    JSDocument document;
    document.Parse<0>(json.c_str());
    ASSERT_FALSE(document.HasParseError()) << json;
    const JSValue* value = &document;
    const auto error = layer.setProperty(name, conversion::Convertible(value));
    ASSERT_FALSE(error) << name << ": " << error->message;
}

std::string point(double lon, double lat, const std::string& properties = "{}") {
    return R"({"type": "Feature", "properties": )" + properties +
           R"(, "geometry": {"type": "Point", "coordinates": [)" + std::to_string(lon) + ", " +
           std::to_string(lat) + "]}}";
}

std::string collection(const std::vector<std::string>& features) {
    std::string out = R"({"type": "FeatureCollection", "features": [)";
    for (std::size_t i = 0; i < features.size(); ++i) {
        out += (i ? ", " : "") + features[i];
    }
    return out + "]}";
}

/** A screen box of `half` pixels round `coordinate`. */
ScreenBox around(const ScreenCoordinate& coordinate, double half = 4) {
    return {{coordinate.x - half, coordinate.y - half}, {coordinate.x + half, coordinate.y + half}};
}

/** The "id" property of each feature, in the order a query gave them (top first). */
std::vector<std::string> ids(const std::vector<Feature>& features) {
    std::vector<std::string> out;
    for (const auto& f : features) {
        const auto it = f.properties.find("id");
        out.push_back(it != f.properties.end() && it->second.is<std::string>() ? it->second.get<std::string>() : "?");
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Opacity 0 and placement.
//
// A symbol layer "over", above a layer "under", each with one name at the
// same point, neither allowed to overlap. Placement places the top layer
// first, so "under" is culled exactly when "over" is placed. "over"'s
// opacity is 0 in three forms, on two shapes of layer: a text-only layer
// with text-opacity 0, and a card (an icon and a text, as the app's
// placement names) with icon-opacity and text-opacity 0. Placement reads no
// opacity (`RenderLayer::needsPlacement`). A rendered query skips a layer
// with no render pass (`RenderLayer::needsRendering`), and
// `RenderSymbolLayer::evaluate` gives a symbol layer no render pass only when
// BOTH its icon and its text opacity are a constant 0 (a zoom-only value
// evaluates to a constant). A text-only layer keeps the icon's default
// opacity 1, so text-opacity 0 alone keeps the pass. Measured:
//
//   layer      | form          | "over" in a query | "over" placed (culls "under")
//   text only  | constant 0    | yes               | yes
//   text only  | zoom-only 0   | yes               | yes
//   text only  | data-driven 0 | yes               | yes
//   card       | constant 0    | no                | yes
//   card       | zoom-only 0   | no                | yes
//   card       | data-driven 0 | yes               | yes
//   either     | visibility none (control) | no    | no
//   either     | opacity 1 (control)       | yes   | yes
//
// So opacity 0 never frees collision room (use a filter, ignore-placement
// or visibility none), and a query is no measure of placement at opacity 0.

namespace {

struct OpacityResult {
    std::size_t over;
    std::size_t under;
};

enum class Shape { Text, Card };

/** `opacity` is the JSON value of each opacity property of "over": text-opacity, and icon-opacity on a card. */
OpacityResult measureOpacity(Shape shape, const std::string& opacity, const std::string& overVisibility = "visible") {
    const bool card = shape == Shape::Card;
    const std::string layout = std::string(R"("text-field": "abab", "text-font": ["Test Stack"], "text-size": 24)") +
                               (card ? R"(, "icon-image": "card", "icon-text-fit": "both")" : "");
    const std::string paint = std::string(R"({"text-opacity": )") + opacity +
                              (card ? R"(, "icon-opacity": )" + opacity : "") + "}";
    const std::string style = R"({"version": 8, "center": [0, 0], "zoom": 2,
        "glyphs": "http://glyphs/{fontstack}/{range}.pbf",
        "sources": {
          "under": {"type": "geojson", "synchronousUpdate": true, "data": )" +
                              collection({point(0, 0, R"({"op": 0})")}) + R"(},
          "over": {"type": "geojson", "synchronousUpdate": true, "data": )" +
                              collection({point(0, 0, R"({"op": 0})")}) + R"(}},
        "layers": [
          {"id": "under", "type": "symbol", "source": "under", "layout": {)" +
                              layout + R"(}},
          {"id": "over", "type": "symbol", "source": "over", "layout": {)" + layout +
                              R"(, "visibility": ")" + overVisibility + R"("}, "paint": )" + paint + "}]}";
    PinMap test(style);
    if (card) {
        PremultipliedImage image({16, 16});
        std::fill(image.data.get(), image.data.get() + image.bytes(), 255);
        test.map->getStyle().addImage(std::make_unique<style::Image>("card", std::move(image), 1.0f));
    }
    EXPECT_TRUE(runUntil(
        test.loop,
        [&] { return test.observer.reported("under") && test.observer.lastFrameFull; },
        "the names laid out"));
    EXPECT_TRUE(test.placed("a placement with both names"));
    return {.over = test.count("over"), .under = test.count("under")};
}

const std::string kZoomOnly0 = R"(["interpolate", ["linear"], ["zoom"], 1, 1, 1.5, 0])"; // 0 from z1.5; the map is at z2
const std::string kDataDriven0 = R"(["get", "op"])";                                    // the feature's op is 0

} // namespace

TEST(AppBehaviourPins, OpacityControlTheUnderNameIsPlacedWhenTheTopLayerIsHidden) {
    for (const auto shape : {Shape::Text, Shape::Card}) {
        const auto result = measureOpacity(shape, "1", "none");
        EXPECT_EQ(0u, result.over);
        EXPECT_EQ(1u, result.under) << "with the top layer hidden, nothing culls the under name";
    }
}

TEST(AppBehaviourPins, OpacityControlAnOpaqueTopNameCullsTheUnderName) {
    for (const auto shape : {Shape::Text, Shape::Card}) {
        const auto result = measureOpacity(shape, "1");
        EXPECT_EQ(1u, result.over);
        EXPECT_EQ(0u, result.under) << "the top name, placed first, culls the under name";
    }
}

TEST(AppBehaviourPins, ATextOnlyNameAtOpacity0IsPlacedCullsAndIsQueriedInEveryForm) {
    for (const auto& opacity : {std::string("0"), kZoomOnly0, kDataDriven0}) {
        const auto result = measureOpacity(Shape::Text, opacity);
        EXPECT_EQ(1u, result.over) << opacity << ": the default icon opacity keeps the render pass";
        EXPECT_EQ(0u, result.under) << opacity << ": the name is placed and culls the under name";
    }
}

TEST(AppBehaviourPins, ACardAtAConstantOpacity0IsPlacedAndCullsButIsNotQueried) {
    const auto result = measureOpacity(Shape::Card, "0");
    EXPECT_EQ(0u, result.over) << "both opacities a constant 0: no render pass, a query skips the layer";
    EXPECT_EQ(0u, result.under) << "the card is placed and culls the under name";
}

TEST(AppBehaviourPins, ACardAtAZoomOnlyOpacity0IsPlacedAndCullsButIsNotQueried) {
    const auto result = measureOpacity(Shape::Card, kZoomOnly0);
    EXPECT_EQ(0u, result.over) << "a zoom-only 0 evaluates to a constant 0: a query skips the layer";
    EXPECT_EQ(0u, result.under) << "the card is placed and culls the under name";
}

TEST(AppBehaviourPins, ACardAtADataDrivenOpacity0IsPlacedCullsAndIsQueried) {
    const auto result = measureOpacity(Shape::Card, kDataDriven0);
    EXPECT_EQ(1u, result.over) << "a data-driven opacity keeps the render pass: a query sees the card";
    EXPECT_EQ(0u, result.under) << "the card is placed and culls the under name";
}

// ---------------------------------------------------------------------------
// A hidden source or layer and the laid-out report.

namespace {

std::string hiddenStyle() {
    const std::string one = collection({point(0, 0)});
    return R"({"version": 8, "center": [0, 0], "zoom": 1.5,
        "sources": {
          "shown": {"type": "geojson", "synchronousUpdate": true, "data": )" +
           one + R"(},
          "hidden": {"type": "geojson", "synchronousUpdate": true, "data": )" +
           one + R"(},
          "hiddenLane": {"type": "geojson", "labelLane": true, "data": )" +
           one + R"(},
          "outOfZoom": {"type": "geojson", "synchronousUpdate": true, "data": )" +
           one + R"(},
          "noLayer": {"type": "geojson", "synchronousUpdate": true, "data": )" +
           one + R"(}},
        "layers": [
          {"id": "shown", "type": "circle", "source": "shown"},
          {"id": "hidden", "type": "circle", "source": "hidden", "layout": {"visibility": "none"}},
          {"id": "hiddenLane", "type": "circle", "source": "hiddenLane", "layout": {"visibility": "none"}},
          {"id": "outOfZoom", "type": "circle", "source": "outOfZoom", "minzoom": 10}]})";
}

} // namespace

// What the fork does today (the spec's F5 said the opposite): a source with
// no tile to lay out reports its body laid out, as `idealTilesLaidOut` is
// true over no ideal tiles. So a source whose layers are all hidden, all out
// of their zoom range, or which has no layer, reports each body laid out in
// the first frame after its write, as a shown synchronous source does, and
// with no tile action. The app cannot read "laid out" as "drawn".
TEST(AppBehaviourPins, ASourceWithNoTileToLayOutReportsEachBodyLaidOut) {
    PinMap test(hiddenStyle());
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.reported("shown"); }, "the shown source laid out"));
    ASSERT_TRUE(test.frame("one frame more"));
    const std::vector<std::string> unseen{"hidden", "hiddenLane", "outOfZoom", "noLayer"};
    for (const auto& id : unseen) {
        EXPECT_TRUE(test.observer.has(id, 1)) << id << ": the style's body was not reported laid out";
    }

    const std::size_t from = test.observer.tileActions.size();
    const std::string two = collection({point(10, 10)});
    const uint64_t shown = test.source("shown").setGeoJSON(geoJSON(two));
    std::map<std::string, uint64_t> written;
    for (const auto& id : unseen) {
        written[id] = test.source(id).setGeoJSON(geoJSON(two));
    }
    ASSERT_TRUE(test.frame("the frame after the writes"));
    EXPECT_TRUE(test.observer.has("shown", shown)) << "the shown synchronous source's body";
    for (const auto& id : unseen) {
        EXPECT_TRUE(test.observer.has(id, written[id])) << id << ": its body was not reported laid out";
        EXPECT_EQ(0u, test.observer.count(TileOperation::StartParse, id, from)) << id << ": a tile parsed";
    }
}

// ---------------------------------------------------------------------------
// `*-translate` is a paint change: drawn in the next frame, with no layout
// (no tile starts a parse, a lane source's generation does not move).

namespace {

std::string translateStyle() {
    // Away from the z1 tile edges (lon 0, lat 0): a feature on an edge is in
    // the query of each tile it touches.
    const std::string origin = collection({point(60, 40)});
    const std::string line =
        R"({"type": "FeatureCollection", "features": [{"type": "Feature", "properties": {},
            "geometry": {"type": "LineString", "coordinates": [[50, 40], [70, 40]]}}]})";
    const std::string square =
        R"({"type": "FeatureCollection", "features": [{"type": "Feature", "properties": {},
            "geometry": {"type": "Polygon", "coordinates": [[[55, 36], [65, 36], [65, 44], [55, 44], [55, 36]]]}}]})";
    return R"({"version": 8, "center": [60, 40], "zoom": 1.5,
        "sources": {
          "lane": {"type": "geojson", "labelLane": true, "data": )" +
           origin + R"(},
          "line": {"type": "geojson", "synchronousUpdate": true, "data": )" +
           line + R"(},
          "fill": {"type": "geojson", "synchronousUpdate": true, "data": )" +
           square + R"(}},
        "layers": [
          {"id": "fill", "type": "fill", "source": "fill"},
          {"id": "line", "type": "line", "source": "line", "paint": {"line-width": 4}},
          {"id": "circle", "type": "circle", "source": "lane", "paint": {"circle-radius": 6}}]})";
}

/** Sets `property` to [0, 60] on `layer`; the next frame draws it 60 px down, with no layout. */
void expectTranslateInTheNextFrame(const std::string& layer, const std::string& property, const std::string& source) {
    PinMap test(translateStyle());
    ASSERT_TRUE(runUntil(
        test.loop,
        [&] { return test.observer.reported("lane") && test.observer.reported(source) && test.observer.lastFrameFull; },
        "the sources laid out"));
    ASSERT_TRUE(test.frame("a frame with every layer"));
    const ScreenCoordinate centre{128, 128};
    const ScreenCoordinate moved{128, 188};
    ASSERT_EQ(1u, test.count(layer, around(centre))) << layer << " not drawn at the centre";
    ASSERT_EQ(0u, test.count(layer, around(moved))) << layer << " already drawn 60 px down";

    const uint64_t generation = test.source(source).getDataGeneration();
    const std::size_t actions = test.observer.tileActions.size();
    const std::size_t reports = test.observer.laidOut.size();
    setProperty(test.layer(layer), property, "[0, 60]");
    ASSERT_TRUE(test.frame("the frame after the translate"));
    EXPECT_EQ(0u, test.count(layer, around(centre))) << layer << ": the old position still drawn";
    EXPECT_EQ(1u, test.count(layer, around(moved))) << layer << ": the translate not drawn in the next frame";
    EXPECT_EQ(0u, test.observer.count(TileOperation::StartParse, source, actions)) << "a translate started a parse";
    EXPECT_EQ(generation, test.source(source).getDataGeneration()) << "a translate moved the source's generation";
    EXPECT_EQ(reports, test.observer.laidOut.size()) << "a translate was reported laid out";
}

} // namespace

TEST(AppBehaviourPins, ACircleTranslateIsDrawnInTheNextFrameWithNoLayout) {
    expectTranslateInTheNextFrame("circle", "circle-translate", "lane");
}

TEST(AppBehaviourPins, ALineTranslateIsDrawnInTheNextFrameWithNoLayout) {
    expectTranslateInTheNextFrame("line", "line-translate", "line");
}

TEST(AppBehaviourPins, AFillTranslateIsDrawnInTheNextFrameWithNoLayout) {
    expectTranslateInTheNextFrame("fill", "fill-translate", "fill");
}

// ---------------------------------------------------------------------------
// Sort keys: features with an equal key draw in REVERSE feed order
// (`std::lower_bound` + insert in circle_layout.hpp and symbol_layout.cpp).
// With no key they draw in feed order. A non-symbol query lists features top
// first (feature_index.cpp sorts by the order the bucket drew them), so a
// query at the shared point reads the draw order.

namespace {

std::string stackedPoints(const std::vector<int>& keys) {
    std::vector<std::string> features;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        features.push_back(
            point(0, 0, R"({"id": "f)" + std::to_string(i) + R"(", "key": )" + std::to_string(keys[i]) + "}"));
    }
    return collection(features);
}

std::vector<std::string> circlesTopFirst(const std::vector<int>& keys, bool sortKey) {
    const std::string layout = sortKey ? R"(, "layout": {"circle-sort-key": ["get", "key"]})" : "";
    PinMap test(R"({"version": 8, "center": [0, 0], "zoom": 1.5,
        "sources": {"points": {"type": "geojson", "synchronousUpdate": true, "data": )" +
                stackedPoints(keys) + R"(}},
        "layers": [{"id": "points", "type": "circle", "source": "points", "paint": {"circle-radius": 8})" +
                layout + "}]}");
    EXPECT_TRUE(runUntil(
        test.loop,
        [&] { return test.observer.reported("points") && test.observer.lastFrameFull; },
        "the points laid out"));
    EXPECT_TRUE(test.frame("a frame with the points"));
    return ids(test.query("points", around({128, 128})));
}

std::vector<std::string> namesPlaced(const std::vector<int>& keys, bool sortKey) {
    const std::string sort = sortKey ? R"(, "symbol-sort-key": ["get", "key"])" : "";
    PinMap test(R"({"version": 8, "center": [0, 0], "zoom": 1.5,
        "glyphs": "http://glyphs/{fontstack}/{range}.pbf",
        "sources": {"names": {"type": "geojson", "synchronousUpdate": true, "data": )" +
                stackedPoints(keys) + R"(}},
        "layers": [{"id": "names", "type": "symbol", "source": "names",
          "layout": {"text-field": "abab", "text-font": ["Test Stack"], "text-size": 24)" +
                sort + "}}]}");
    EXPECT_TRUE(runUntil(
        test.loop,
        [&] { return test.observer.reported("names") && test.observer.lastFrameFull; },
        "the names laid out"));
    EXPECT_TRUE(test.placed("a placement with the names"));
    return ids(test.query("names"));
}

using Ids = std::vector<std::string>;

} // namespace

TEST(AppBehaviourPins, CirclesWithNoSortKeyDrawInFeedOrder) {
    // The last feature is drawn last, on top.
    EXPECT_EQ((Ids{"f4", "f3", "f2", "f1", "f0"}), circlesTopFirst({0, 0, 0, 5, 0}, false));
}

TEST(AppBehaviourPins, CirclesWithAnEqualSortKeyDrawInReverseFeedOrder) {
    // Drawn f4 f2 f1 f0 f3: the higher key on top, the ties reversed.
    EXPECT_EQ((Ids{"f3", "f0", "f1", "f2", "f4"}), circlesTopFirst({0, 0, 0, 5, 0}, true));
}

TEST(AppBehaviourPins, NamesWithNoSortKeyArePlacedInFeedOrder) {
    // Two names at one point: the first placed wins.
    EXPECT_EQ((Ids{"f0"}), namesPlaced({0, 0}, false));
}

TEST(AppBehaviourPins, NamesWithAnEqualSortKeyArePlacedInReverseFeedOrder) {
    EXPECT_EQ((Ids{"f1"}), namesPlaced({0, 0}, true));
}

TEST(AppBehaviourPins, ANameWithALowerSortKeyIsPlacedFirst) {
    EXPECT_EQ((Ids{"f0"}), namesPlaced({0, 1}, true));
}

// ---------------------------------------------------------------------------
// Writes in one main turn draw together, for SYNCHRONOUS-update sources: a
// body on one and a filter on another, set in one turn, are both drawn in
// the next frame, with the shared worker pool held. A pool source written in
// the same turn is not: its body waits for a worker.

namespace {

std::string threeSourceStyle() {
    const std::string origin = collection({point(0, 0, R"({"kind": "keep"})")});
    return R"({"version": 8, "center": [0, 0], "zoom": 1.5,
        "sources": {
          "a": {"type": "geojson", "synchronousUpdate": true, "data": )" +
           origin + R"(},
          "b": {"type": "geojson", "synchronousUpdate": true, "data": )" +
           origin + R"(},
          "pool": {"type": "geojson", "data": )" +
           origin + R"(}},
        "layers": [
          {"id": "a", "type": "circle", "source": "a"},
          {"id": "b", "type": "circle", "source": "b"},
          {"id": "pool", "type": "circle", "source": "pool"}]})";
}

const std::string kTwoPoints = collection({point(-20, 20, R"({"kind": "keep"})"), point(20, -20, R"({"kind": "drop"})")});

} // namespace

TEST(AppBehaviourPins, WritesInOneTurnToSynchronousSourcesDrawInTheNextFrame) {
    PinMap test(threeSourceStyle());
    ASSERT_TRUE(runUntil(
        test.loop,
        [&] {
            return test.observer.reported("a") && test.observer.reported("b") && test.observer.reported("pool") &&
                   test.observer.lastFrameFull;
        },
        "the sources laid out"));
    ASSERT_TRUE(test.frame("a frame with every source"));
    ASSERT_EQ(1u, test.count("a"));
    ASSERT_EQ(1u, test.count("b"));
    ASSERT_EQ(1u, test.count("pool"));

    PoolBlock block;
    ASSERT_TRUE(block.allHeld) << "the pool's four threads were not all held";
    // One turn: a body on "a", a filter on "b", a body on "pool".
    test.source("a").setGeoJSON(geoJSON(kTwoPoints));
    test.layer("b").setFilter(filter(R"(["==", ["get", "kind"], "none"])"));
    const uint64_t pool = test.source("pool").setGeoJSON(geoJSON(kTwoPoints));
    ASSERT_TRUE(test.frame("the frame after the writes"));
    EXPECT_EQ(2u, test.count("a")) << "the synchronous body was not drawn in the next frame";
    EXPECT_EQ(0u, test.count("b")) << "the synchronous source's filter was not drawn in the next frame";
    EXPECT_EQ(1u, test.count("pool")) << "the pool source's body was drawn with the pool held";

    block.release();
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("pool", pool); }, "the pool body laid out"));
    ASSERT_TRUE(test.frame("a frame with the pool body"));
    EXPECT_EQ(2u, test.count("pool"));
}

// ---------------------------------------------------------------------------
// A filter set on a layer already on the map, with no re-add: on a
// synchronous source it is drawn in the next frame (the lane case is
// LabelLane.AFilterChangeIsDrawnInTheNextFrame). On a pool source it is laid
// out on a worker: each tile of the source reports StartParse, then
// EndParse, and "parse end means complete": the first frame after the last
// EndParse draws the filter.

TEST(AppBehaviourPins, AFilterOnASynchronousSourceIsDrawnInTheNextFrame) {
    PinMap test(threeSourceStyle());
    ASSERT_TRUE(runUntil(
        test.loop, [&] { return test.observer.reported("a") && test.observer.lastFrameFull; }, "the source laid out"));
    test.source("a").setGeoJSON(geoJSON(kTwoPoints));
    ASSERT_TRUE(test.frame("a frame with the two points"));
    ASSERT_EQ(2u, test.count("a"));

    PoolBlock block;
    ASSERT_TRUE(block.allHeld) << "the pool's four threads were not all held";
    test.layer("a").setFilter(filter(R"(["==", ["get", "kind"], "keep"])"));
    ASSERT_TRUE(test.frame("the frame after the filter"));
    EXPECT_EQ(1u, test.count("a")) << "the filter was not drawn in the next frame";
}

TEST(AppBehaviourPins, AFilterOnAPoolSourceIsDrawnInTheFirstFrameAfterTheLastParseEnd) {
    PinMap test(threeSourceStyle());
    ASSERT_TRUE(runUntil(
        test.loop, [&] { return test.observer.reported("pool") && test.observer.lastFrameFull; }, "the source laid out"));
    const uint64_t body = test.source("pool").setGeoJSON(geoJSON(kTwoPoints));
    ASSERT_TRUE(runUntil(test.loop, [&] { return test.observer.has("pool", body); }, "the two points laid out"));
    ASSERT_TRUE(test.frame("a frame with the two points"));
    ASSERT_EQ(2u, test.count("pool"));

    auto block = std::make_unique<PoolBlock>();
    ASSERT_TRUE(block->allHeld) << "the pool's four threads were not all held";
    const std::size_t from = test.observer.tileActions.size();
    test.layer("pool").setFilter(filter(R"(["==", ["get", "kind"], "keep"])"));
    ASSERT_TRUE(test.frame("the frame after the filter, with the pool held"));
    EXPECT_GT(test.observer.count(TileOperation::StartParse, "pool", from), 0u) << "the filter started no parse";
    EXPECT_EQ(0u, test.observer.count(TileOperation::EndParse, "pool", from)) << "a parse ended with the pool held";
    EXPECT_EQ(2u, test.count("pool")) << "the filter was drawn before its parse ended";

    // The frame that draws the filter is the first that begins after the
    // last parse end: the report and the layout come in one call.
    std::size_t endedAtFrame = 0;
    block.reset();
    ASSERT_TRUE(runUntil(
        test.loop,
        [&] {
            if (test.observer.everyStartedParseEnded("pool", from)) {
                endedAtFrame = static_cast<std::size_t>(test.observer.frames);
                return true;
            }
            return false;
        },
        "every started parse ended"));
    ASSERT_TRUE(test.frame("the first frame after the last parse end"));
    EXPECT_EQ(static_cast<std::size_t>(test.observer.frames), endedAtFrame + 1);
    EXPECT_EQ(1u, test.count("pool")) << "the first frame after the last parse end did not draw the filter";
}

// ---------------------------------------------------------------------------
// `Map::setNoFadeInSources`: a new name of a listed source is drawn whole in
// its first placement; a name of another source fades in from 0. The fade is
// set to an hour, so "fades in" is 0 in any frame of this test. Both names
// are placed (the query reads the placement, not the opacity).

namespace {

/** The pixels drawn (alpha above 0) in the left and the right half of the last frame. */
std::pair<std::size_t, std::size_t> drawnPixels(PinMap& test) {
    const gfx::BackendScope scope{*test.frontend->getBackend()};
    const PremultipliedImage image = test.frontend->readStillImage();
    std::size_t left = 0;
    std::size_t right = 0;
    for (uint32_t y = 0; y < image.size.height; ++y) {
        for (uint32_t x = 0; x < image.size.width; ++x) {
            if (image.data[(y * image.size.width + x) * 4 + 3] > 0) {
                (x < image.size.width / 2 ? left : right)++;
            }
        }
    }
    return {left, right};
}

std::pair<std::size_t, std::size_t> firstPlacementPixels(const std::set<std::string>& noFadeIn) {
    PinMap test(
        R"({"version": 8, "center": [0, 0], "zoom": 1.5,
        "glyphs": "http://glyphs/{fontstack}/{range}.pbf",
        "sources": {
          "snap": {"type": "geojson", "synchronousUpdate": true, "data": )" +
            collection({point(-20, 10)}) + R"(},
          "fade": {"type": "geojson", "synchronousUpdate": true, "data": )" +
            collection({point(20, 10)}) + R"(}},
        "layers": [
          {"id": "snap", "type": "symbol", "source": "snap",
           "layout": {"text-field": "abab", "text-font": ["Test Stack"], "text-size": 24}},
          {"id": "fade", "type": "symbol", "source": "fade",
           "layout": {"text-field": "abab", "text-font": ["Test Stack"], "text-size": 24}}]})",
        true);
    TransitionOptions transitions = test.map->getStyle().getTransitionOptions();
    transitions.enablePlacementTransitions = true;
    transitions.duration = std::chrono::hours(1);
    test.map->getStyle().setTransitionOptions(transitions);
    test.map->setNoFadeInSources(noFadeIn);
    EXPECT_TRUE(runUntil(
        test.loop,
        [&] { return test.observer.reported("snap") && test.observer.reported("fade") && test.observer.lastFrameFull; },
        "the names laid out"));
    EXPECT_TRUE(test.placed("a placement with both names"));
    EXPECT_EQ(1u, test.count("snap")) << "the listed source's name is not placed";
    EXPECT_EQ(1u, test.count("fade")) << "the other source's name is not placed";
    return drawnPixels(test);
}

} // namespace

TEST(AppBehaviourPins, ANoFadeInSourcesNewNameIsDrawnWholeInItsFirstPlacement) {
    const auto [snap, fade] = firstPlacementPixels({"snap"});
    EXPECT_GT(snap, 0u) << "the listed source's name was not drawn in its first placement";
    EXPECT_EQ(0u, fade) << "the other source's name did not fade in from 0";
}

TEST(AppBehaviourPins, WithNoNoFadeInSourcesEveryNewNameFadesInFrom0) {
    const auto [snap, fade] = firstPlacementPixels({});
    EXPECT_EQ(0u, snap);
    EXPECT_EQ(0u, fade);
}

TEST(AppBehaviourPins, SetNoFadeInSourcesReplacesTheSet) {
    PinMap test(threeSourceStyle());
    test.map->setNoFadeInSources({"a", "b"});
    test.map->setNoFadeInSources({"pool"});
    EXPECT_EQ((std::set<std::string>{"pool"}), test.map->getNoFadeInSources());
}

// ---------------------------------------------------------------------------
// The source object is kept across writes: a body, a URL and a filter on its
// layer leave the style's source the same object, with no re-add.

TEST(AppBehaviourPins, TheSourceObjectIsKeptAcrossWrites) {
    PinMap test(threeSourceStyle());
    ASSERT_TRUE(runUntil(
        test.loop, [&] { return test.observer.reported("a") && test.observer.lastFrameFull; }, "the source laid out"));
    Source* const before = test.map->getStyle().getSource("a");
    Layer* const layerBefore = test.map->getStyle().getLayer("a");
    const uint64_t body = test.source("a").setGeoJSON(geoJSON(kTwoPoints));
    test.layer("a").setFilter(filter(R"(["==", ["get", "kind"], "keep"])"));
    test.source("a").setGeoJSON(geoJSON(kTwoPoints));
    ASSERT_TRUE(test.frame("a frame after the writes"));
    EXPECT_EQ(before, test.map->getStyle().getSource("a"));
    EXPECT_EQ(layerBefore, test.map->getStyle().getLayer("a"));
    EXPECT_GT(test.source("a").getDataGeneration(), body);
}
