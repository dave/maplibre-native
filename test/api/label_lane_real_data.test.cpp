// The label lane with Viatrail's real bodies (label-lane plan, Task 3): a
// headless continuous map with a basemap-like vector source that keeps the
// worker pool busy, and Viatrail's two label sources (the placement slots and
// the names), filled from the GPT sample's bodies (test/fixtures/label_lane).
// Three arms, interleaved: sync (synchronousUpdate), async (plain), lane
// (labelLane). The bodies are set as the Android binding sets them: parsed
// on the source's parse scheduler, or on the calling thread for sync.
//
// It asserts only WHERE each step ran (the step recorder, util::LabelSteps):
// every work step on the render thread for sync, none for async and lane, and
// every one on the lane for lane. It prints, and does not assert, the queue
// and work times of each step and hop, the time from a body's set to its
// laid-out signal, and whether that came before the basemap's work ended:
// times on a laptop say little about a phone, and a timing assertion flakes.

#include <mln/test/map_adapter.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/util.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_observer.hpp>
#include <mln/map/map_options.hpp>
#include <mln/style/image.hpp>
#include <mln/style/sources/geojson_source.hpp>
#include <mln/style/sources/geojson_source_impl.hpp>
#include <mln/style/style.hpp>
#include <mln/util/io.hpp>
#include <mln/util/label_steps.hpp>
#include <mln/util/monotonic_timer.hpp>
#include <mln/util/platform.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/timer.hpp>

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#if defined(__APPLE__)
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void*);
#endif

using namespace mln;
using namespace mln::style;
using Secs = std::chrono::duration<double>;

namespace {

constexpr auto kBound = std::chrono::seconds(60);
constexpr int kRounds = 3;

/**
 * One turn of the loop. On Apple, inside an autorelease pool: the headless
 * Metal backend's command buffers are autoreleased, and with no pool drained
 * between frames they stay alive, until the queue's 64 are used up and the
 * next frame waits for ever in -[MTLCommandQueue commandBuffer].
 */
void turn(util::RunLoop& loop) {
#if defined(__APPLE__)
    void* pool = objc_autoreleasePoolPush();
    loop.runOnce();
    objc_autoreleasePoolPop(pool);
#else
    loop.runOnce();
#endif
}

enum class Arm {
    Sync,
    Async,
    Lane
};

const char* armName(Arm arm) {
    switch (arm) {
        case Arm::Sync:
            return "sync";
        case Arm::Async:
            return "async";
        case Arm::Lane:
            return "lane";
    }
    return "?";
}

std::string optionsJSON(Arm arm) {
    switch (arm) {
        case Arm::Sync:
            return R"(, "synchronousUpdate": true)";
        case Arm::Async:
            return "";
        case Arm::Lane:
            return R"(, "labelLane": true)";
    }
    return "";
}

// A cut-down copy of Viatrail's placement layers (PlacementLayers.kt): the
// small marks, a card's anchor, leader, card, text, name mark and row marks,
// and the line-placed names.
std::string styleJSON(Arm arm) {
    const std::string options = optionsJSON(arm);
    const std::string empty = R"({"type": "FeatureCollection", "features": []})";
    std::string placed;
    const auto iconLayer = [&](const std::string& id, const std::string& filter, const std::string& image) {
        placed +=
            R"(, {"id": ")" + id + R"(", "type": "symbol", "source": "placement", "filter": )" + filter +
            R"(, "layout": {"icon-image": ["get", ")" + image +
            R"("], "icon-allow-overlap": true, "icon-ignore-placement": true, "symbol-sort-key": ["get", "sort"]}})";
    };
    const std::string small = R"(["in", ["get", "form"], ["literal", ["badge", "dot", "ellipsis"]]])";
    const std::string tag = R"(["==", ["get", "form"], "tag"])";
    iconLayer("small", small, "image");
    iconLayer("anchor", tag, "anchorImage");
    iconLayer("leader", tag, "leaderImage");
    iconLayer("card-mark", tag, "nameImage");
    for (int i = 0; i < 4; ++i) {
        iconLayer("card-row" + std::to_string(i), tag, "row" + std::to_string(i) + "Image");
    }
    placed +=
        R"(, {"id": "card-text", "type": "symbol", "source": "placement", "filter": )" + tag +
        R"(, "layout": {"text-field": ["format", ["get", "name"], {}, "\n", {}, ["coalesce", ["get", "row0Text"], ""], {}],
                  "text-font": ["Test Stack"], "text-size": 13, "text-allow-overlap": true,
                  "text-ignore-placement": true, "symbol-sort-key": ["get", "sort"]}})";
    const std::string names = R"(, {"id": "names", "type": "symbol", "source": "names",
        "layout": {"symbol-placement": "line", "text-field": ["get", "name"], "text-font": ["Test Stack"],
                   "text-size": 12, "symbol-sort-key": ["get", "sort"]}})";
    const std::string basemap = R"(
        {"id": "water", "type": "fill", "source": "basemap", "source-layer": "water", "paint": {"fill-color": "#9cf"}},
        {"id": "landuse", "type": "fill", "source": "basemap", "source-layer": "landuse", "paint": {"fill-color": "#cfc"}},
        {"id": "building", "type": "fill", "source": "basemap", "source-layer": "building", "paint": {"fill-color": "#ccc"}},
        {"id": "road", "type": "line", "source": "basemap", "source-layer": "road", "paint": {"line-color": "#fff"}},
        {"id": "road-casing", "type": "line", "source": "basemap", "source-layer": "road",
         "paint": {"line-color": "#999", "line-gap-width": 2}},
        {"id": "waterway", "type": "line", "source": "basemap", "source-layer": "waterway", "paint": {"line-color": "#9cf"}})";
    return R"({"version": 8, "glyphs": "http://glyphs/{fontstack}/{range}.pbf",
        "sources": {
          "basemap": {"type": "vector", "tileSize": 256, "maxzoom": 14, "tiles": ["http://basemap/{z}/{x}/{y}.mvt"]},
          "placement": {"type": "geojson", "data": )" +
           empty + options + R"(},
          "names": {"type": "geojson", "data": )" +
           empty + options + R"(}},
        "layers": [)" +
           basemap + placed + names + "]}";
}

/** Every image key the placement body names, for the images the app registers. */
std::set<std::string> imageKeys(const std::string& body) {
    std::set<std::string> keys;
    for (const std::string property :
         {"image", "anchorImage", "leaderImage", "nameImage", "row0Image", "row1Image", "row2Image", "row3Image"}) {
        const std::string marker = "\"" + property + "\":\"";
        for (std::size_t at = body.find(marker); at != std::string::npos; at = body.find(marker, at + 1)) {
            const std::size_t start = at + marker.size();
            const std::size_t end = body.find('"', start);
            if (end > start) keys.insert(body.substr(start, end - start));
        }
    }
    return keys;
}

class Observer final : public MapObserver {
public:
    void onSourceLaidOut(const std::string& sourceID, uint64_t generation) override {
        laidOut[{sourceID, generation}] = util::MonotonicTimer::now();
    }
    void onDidFinishRenderingFrame(const RenderFrameStatus& status) override {
        lastFrameFull = status.mode == RenderMode::Full;
    }
    std::map<std::pair<std::string, uint64_t>, Secs> laidOut;
    bool lastFrameFull = false;
};

struct BodySet {
    std::string sourceID;
    uint64_t generation = 0;
    Secs at{0};
};

struct ArmResult {
    Arm arm;
    std::vector<util::LabelStep> steps;
    std::vector<BodySet> sets;
    std::map<std::pair<std::string, uint64_t>, Secs> laidOut;
};

bool ours(const util::LabelStep& step) {
    return step.sourceID == "placement" || step.sourceID == "names";
}

/** One arm: a fresh map, the basemap loading, both bodies set as its tiles queue, run to the signal and a full frame.
 */
ArmResult runArm(Arm arm, const std::string& placementBody, const std::string& namesBody) {
    (void)util::LabelSteps::take(); // the arm before's last steps
    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>();
    const auto basemapTile = std::make_shared<std::string>(
        util::read_file("metrics/integration/tiles/14-8803-5375.mvt"));
    fileSource->tileResponse = [basemapTile](const Resource&) {
        Response response;
        response.data = basemapTile;
        return response;
    };
    const auto glyphs = std::make_shared<std::string>(util::read_file("test/fixtures/resources/glyphs.pbf"));
    fileSource->glyphsResponse = [glyphs](const Resource&) {
        Response response;
        response.data = glyphs;
        return response;
    };
    Observer observer;
    // Pixel 7: 1080 x 2400 px at 2.625 px per dp.
    HeadlessFrontend frontend{Size{411, 914}, 2.625f};
    MapAdapter map{frontend,
                   observer,
                   fileSource,
                   MapOptions().withMapMode(MapMode::Continuous).withSize(frontend.getSize()).withPixelRatio(2.625f)};
    map.getStyle().loadJSON(styleJSON(arm));
    for (const auto& key : imageKeys(placementBody)) {
        map.getStyle().addImage(std::make_unique<style::Image>(key, PremultipliedImage({48, 48}), 2.0f));
    }
    map.jumpTo(CameraOptions().withCenter(LatLng{-48.65, -72.85}).withZoom(10.0));

    bool late = false;
    util::Timer watchdog;
    watchdog.start(kBound, Duration::zero(), [&] { late = true; });
    std::vector<util::LabelStep> steps;
    const auto drain = [&] {
        auto taken = util::LabelSteps::take();
        steps.insert(steps.end(), taken.begin(), taken.end());
    };

    // The basemap's tiles reach their workers: the pool is filling.
    while (!late &&
           std::ranges::none_of(steps, [](const auto& s) { return s.sourceID == "basemap" && s.name == "hop-data"; })) {
        turn(loop);
        drain();
    }
    EXPECT_FALSE(late) << armName(arm) << ": the basemap's tiles did not arrive";

    ArmResult result{.arm = arm};
    const auto set = [&](const std::string& sourceID, const std::string& body) {
        auto& source = *map.getStyle().getSource(sourceID)->as<GeoJSONSource>();
        const uint64_t generation = source.nextDataGeneration();
        result.sets.push_back({.sourceID = sourceID, .generation = generation, .at = util::MonotonicTimer::now()});
        const auto queued = util::MonotonicTimer::now();
        const auto convert =
            [slicer = source.getSliceScheduler(), options = source.impl().getOptions(), sourceID, body, queued] {
                const util::LabelSteps::Scope step(sourceID, "convert", false, queued);
                return GeoJSONData::parse(body, slicer, options, sourceID);
            };
        const auto land = [&map, sourceID, generation](GeoJSONData::ParseResult parsed) {
            auto* source = map.getStyle().getSource(sourceID)->as<GeoJSONSource>();
            source->setGeoJSONData(std::get<std::shared_ptr<GeoJSONData>>(std::move(parsed)), generation);
        };
        if (arm == Arm::Sync) {
            land(convert());
        } else {
            source.getParseScheduler()->scheduleAndReplyValue(util::SimpleIdentity::Empty, convert, land);
        }
    };
    set("placement", placementBody);
    set("names", namesBody);

    const auto bothLaidOut = [&] {
        return std::ranges::all_of(
            result.sets, [&](const BodySet& s) { return observer.laidOut.contains({s.sourceID, s.generation}); });
    };
    while (!late && !(bothLaidOut() && observer.lastFrameFull)) {
        turn(loop);
        drain();
    }
    EXPECT_FALSE(late) << armName(arm) << ": the bodies or the basemap did not finish";
    drain();
    result.steps = std::move(steps);
    result.laidOut = observer.laidOut;
    return result;
}

double ms(Secs s) {
    return s.count() * 1000.0;
}

struct Spread {
    std::vector<double> values;
    void add(double v) { values.push_back(v); }
    std::string str() const {
        if (values.empty()) return "-";
        auto sorted = values;
        std::ranges::sort(sorted);
        char out[96];
        std::snprintf(out,
                      sizeof out,
                      "%.1f (%.1f-%.1f, n=%zu)",
                      sorted[sorted.size() / 2],
                      sorted.front(),
                      sorted.back(),
                      sorted.size());
        return out;
    }
};

} // namespace

TEST(LabelLaneRealData, OurStepsRunWhereEachArmPutsThemAndTheirTimesArePrinted) {
    const std::string placementBody = util::read_file("test/fixtures/label_lane/gpt-placement-z10.json");
    const std::string namesBody = util::read_file("test/fixtures/label_lane/gpt-names-z10.json");
    const std::string renderThread = platform::getCurrentThreadName();

    (void)util::LabelSteps::take();
    util::LabelSteps::setRecording(true);
    std::vector<ArmResult> results;
    for (int round = 0; round < kRounds; ++round) {
        for (const Arm arm : {Arm::Sync, Arm::Async, Arm::Lane}) {
            results.push_back(runArm(arm, placementBody, namesBody));
        }
    }
    util::LabelSteps::setRecording(false);
    (void)util::LabelSteps::take();

    // Per arm: work steps and hops of ours on the render thread, and the times.
    struct Totals {
        int work = 0, workOnRender = 0, workOnLane = 0, hops = 0, hopsOnRender = 0;
        std::map<std::string, Spread> queue, work_, hop;
        Spread setToLaidOut, laneSeries, basemapBusy, laidOutBeforeBasemapEnd;
    };
    std::map<Arm, Totals> totals;
    for (const auto& r : results) {
        Totals& t = totals[r.arm];
        Secs basemapStart{1e18}, basemapEnd{0};
        for (const auto& s : r.steps) {
            if (s.sourceID == "basemap" && !s.hop) {
                basemapStart = std::min(basemapStart, s.start);
                basemapEnd = std::max(basemapEnd, s.end);
            }
        }
        t.basemapBusy.add(ms(basemapEnd - basemapStart));
        Secs laneSeries{0};
        // Our bodies' steps only: the style's empty bodies (its inline "data")
        // are set with setGeoJSON while the style loads, so their index
        // builds run on the render thread in every arm.
        const Secs bodiesSet = r.sets.front().at;
        for (const auto& s : r.steps) {
            if (!ours(s) || s.start < bodiesSet) continue;
            const std::string key = s.sourceID + " " + s.name;
            if (s.hop) {
                ++t.hops;
                t.hopsOnRender += s.thread == renderThread;
                t.hop[key].add(ms(s.end - s.start));
                continue;
            }
            ++t.work;
            t.workOnRender += s.thread == renderThread;
            const bool onLane = s.thread.find("Label lane") != std::string::npos;
            t.workOnLane += onLane;
            if (s.thread.find("Label lane 1") != std::string::npos) laneSeries += s.end - s.start;
            if (s.queued.count() > 0) t.queue[key].add(ms(s.start - s.queued));
            t.work_[key].add(ms(s.end - s.start));
        }
        t.laneSeries.add(ms(laneSeries));
        for (const auto& set : r.sets) {
            const auto it = r.laidOut.find({set.sourceID, set.generation});
            if (it == r.laidOut.end()) continue;
            t.setToLaidOut.add(ms(it->second - set.at));
            t.laidOutBeforeBasemapEnd.add(it->second < basemapEnd ? 1 : 0);
        }
    }

    for (const Arm arm : {Arm::Sync, Arm::Async, Arm::Lane}) {
        const Totals& t = totals[arm];
        std::printf(
            "\n== %s: our work steps %d (on the render thread %d, on the lane %d); hops %d (on the render thread %d)\n",
            armName(arm),
            t.work,
            t.workOnRender,
            t.workOnLane,
            t.hops,
            t.hopsOnRender);
        std::printf("   body set -> laid out, ms: %s\n", t.setToLaidOut.str().c_str());
        std::printf("   laid out before the basemap's work ended (1 yes, 0 no): %s\n",
                    t.laidOutBeforeBasemapEnd.str().c_str());
        std::printf("   basemap work window, ms: %s\n", t.basemapBusy.str().c_str());
        std::printf("   both sources' work on \"Label lane 1\" in series, ms: %s\n", t.laneSeries.str().c_str());
        for (const auto& [key, spread] : t.work_) {
            const auto q = t.queue.find(key);
            std::printf("   step %-24s work ms %-28s queue ms %s\n",
                        key.c_str(),
                        spread.str().c_str(),
                        q == t.queue.end() ? "-" : q->second.str().c_str());
        }
        for (const auto& [key, spread] : t.hop) {
            std::printf("   hop  %-24s work ms %s\n", key.c_str(), spread.str().c_str());
        }

        ASSERT_GT(t.work, 0) << armName(arm) << ": no step of ours was recorded";
        switch (arm) {
            case Arm::Sync:
                EXPECT_EQ(t.work, t.workOnRender) << "sync: a step of ours ran off the render thread";
                break;
            case Arm::Async:
                EXPECT_EQ(0, t.workOnRender) << "async: a step of ours ran on the render thread";
                EXPECT_EQ(0, t.workOnLane) << "async: a step of ours ran on the lane";
                break;
            case Arm::Lane:
                EXPECT_EQ(0, t.workOnRender) << "lane: a step of ours ran on the render thread";
                EXPECT_EQ(t.work, t.workOnLane) << "lane: a step of ours ran off the lane";
                break;
        }
    }
}
