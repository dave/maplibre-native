// A ResourceProvider, through a real MainResourceLoader with a database, as an
// app has it. Each wait ends on an event, and its bound only turns a missing
// event into a failure.

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/map_observer.hpp>
#include <mln/map/map_options.hpp>
#if defined(__APPLE__)
#include <mln/map/map_snapshotter.hpp>
#endif
#include <mln/storage/database_file_source.hpp>
#include <mln/storage/main_resource_loader.hpp>
#include <mln/storage/online_file_source.hpp>
#include <mln/storage/resource.hpp>
#include <mln/storage/resource_options.hpp>
#include <mln/storage/resource_provider.hpp>
#include <mln/storage/resource_provider_file_source.hpp>
#include <mln/storage/resource_transform.hpp>
#include <mln/renderer/query.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/style/sources/tile_source.hpp>
#include <mln/style/sources/vector_source.hpp>
#include <mln/style/style.hpp>
#include <mln/test/fixture_log_observer.hpp>
#include <mln/test/map_adapter.hpp>
#include <mln/test/util.hpp>
#include <mln/util/async_request.hpp>
#include <mln/util/client_options.hpp>
#include <mln/util/io.hpp>
#include <mln/util/label_steps.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/tile_server_options.hpp>
#include <mln/util/timer.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <latch>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

using namespace mln;

namespace {

constexpr auto kBound = std::chrono::seconds(30);

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

/** Keeps each request and responder. `onStart`, if set, answers at once. */
class TestProvider final : public ResourceProvider {
public:
    void start(const ProviderRequest& request, std::shared_ptr<ProviderResponder> responder) override {
        const std::scoped_lock lock(mutex);
        requests.push_back(request);
        responders.push_back(responder);
        startThreads.push_back(std::this_thread::get_id());
        if (onStart) onStart(request, *responder);
    }
    void cancel(uint64_t id) override {
        const std::scoped_lock lock(mutex);
        cancelled.push_back(id);
        cancelThreads.push_back(std::this_thread::get_id());
    }
    std::size_t started() {
        const std::scoped_lock lock(mutex);
        return requests.size();
    }
    std::size_t cancels() {
        const std::scoped_lock lock(mutex);
        return cancelled.size();
    }
    std::shared_ptr<ProviderResponder> responder(std::size_t i) {
        const std::scoped_lock lock(mutex);
        return responders.at(i);
    }

    std::mutex mutex;
    std::function<void(const ProviderRequest&, ProviderResponder&)> onStart;
    std::vector<ProviderRequest> requests;
    std::vector<std::shared_ptr<ProviderResponder>> responders;
    std::vector<uint64_t> cancelled;
    std::vector<std::thread::id> startThreads;
    std::vector<std::thread::id> cancelThreads;
};

/** A provider for a scheme, for one test. */
class Registered {
public:
    explicit Registered(std::string scheme_)
        : scheme(std::move(scheme_)) {
        registerResourceProvider(scheme, provider);
    }
    ~Registered() { unregisterResourceProvider(scheme); }
    const std::string scheme;
    const std::shared_ptr<TestProvider> provider = std::make_shared<TestProvider>();
};

std::shared_ptr<const std::string> bytes(const std::string& text) {
    return std::make_shared<const std::string>(text);
}

} // namespace

// The scheme is http, so a request that went on to the online file source would
// get the test server's answer.
TEST(ResourceProvider, TEST_REQUIRES_SERVER(ARegisteredSchemeNeverReachesTheOnlineFileSource)) {
    util::RunLoop loop;
    Registered registered("http");
    registered.provider->onStart = [](const ProviderRequest&, ProviderResponder& responder) {
        responder.data(bytes("from the provider"));
    };
    MainResourceLoader fs(ResourceOptions{}, ClientOptions{});
    std::optional<Response> response;
    auto request = fs.request({Resource::Unknown, "http://127.0.0.1:3000/test"}, [&](Response res) { response = res; });
    ASSERT_TRUE(runUntil(loop, [&] { return bool(response); }, "the answer"));
    ASSERT_TRUE(response->data);
    EXPECT_EQ("from the provider", *response->data);
    EXPECT_EQ(1u, registered.provider->started());
}

TEST(ResourceProvider, TEST_REQUIRES_SERVER(AnUnregisteredSchemeReachesTheOnlineFileSource)) {
    util::RunLoop loop;
    {
        Registered registered("http");
    }
    MainResourceLoader fs(ResourceOptions{}, ClientOptions{});
    std::shared_ptr<const std::string> data;
    // The first answer can be a connection error, while the server starts: the
    // online file source asks again.
    auto request = fs.request({Resource::Unknown, "http://127.0.0.1:3000/test"},
                              [&](Response res) { data = res.data; });
    ASSERT_TRUE(runUntil(loop, [&] { return bool(data); }, "the server's answer"));
    EXPECT_EQ("Hello World!", *data);
}

TEST(ResourceProvider, ARequestCarriesWhatTheProviderNeeds) {
    util::RunLoop loop;
    Registered registered("provider-test");
    MainResourceLoader fs(ResourceOptions{}, ClientOptions{});
    Resource tile = Resource::tile("provider-test://tiles/{z}/{x}/{y}?v=7",
                                   1.0f,
                                   1,
                                   2,
                                   3,
                                   Tileset::Scheme::XYZ,
                                   Resource::LoadingMethod::NetworkOnly);
    tile.sourceID = "the source";
    tile.setPriority(Resource::Priority::Low);
    auto first = fs.request(tile, [](Response) {});
    Resource cacheOnly = tile;
    cacheOnly.loadingMethod = Resource::LoadingMethod::CacheOnly;
    auto second = fs.request(cacheOnly, [](Response) {});
    ASSERT_TRUE(runUntil(loop, [&] { return registered.provider->started() == 2; }, "both requests"));

    const auto& request = registered.provider->requests[0];
    EXPECT_NE(0u, request.id);
    EXPECT_EQ(Resource::Tile, request.kind);
    EXPECT_EQ("provider-test://tiles/3/1/2?v=7", request.url);
    EXPECT_EQ("the source", request.sourceID);
    ASSERT_TRUE(request.tile);
    EXPECT_EQ(3, request.tile->z);
    EXPECT_FALSE(request.localOnly);
    EXPECT_EQ(Resource::Priority::Low, request.priority);
    EXPECT_TRUE(registered.provider->requests[1].localOnly);
    EXPECT_GT(registered.provider->requests[1].id, request.id);
}

TEST(ResourceProvider, ABadRegistrationThrows) {
    EXPECT_THROW(registerResourceProvider("", std::make_shared<TestProvider>()), std::invalid_argument);
    EXPECT_THROW(registerResourceProvider("scheme", nullptr), std::invalid_argument);
}

// Each request ends once: an answer, or a cancel. An answer and a drop race on two
// threads, for many rounds. The file source is used alone so that the answers
// can be counted: a loader drops the answer to a request it has dropped.
TEST(ResourceProvider, AnAnswerRacingACancelEndsTheRequestOnce) {
    Registered registered("provider-test");
    ResourceProviderFileSource fileSource(ResourceOptions{}, ClientOptions{});
    constexpr std::size_t kRounds = 2000;
    std::atomic<std::size_t> answered{0};
    for (std::size_t round = 0; round < kRounds; ++round) {
        auto request = fileSource.request({Resource::Unknown, "provider-test://race"}, [&](Response) { ++answered; });
        ASSERT_EQ(round + 1, registered.provider->started());
        std::latch go(2);
        std::thread answerer([&, responder = registered.provider->responder(round)] {
            go.arrive_and_wait();
            responder->data(bytes("x"));
        });
        go.arrive_and_wait();
        request.reset();
        answerer.join();
    }
    const std::scoped_lock lock(registered.provider->mutex);
    const std::set<uint64_t> cancelled(registered.provider->cancelled.begin(), registered.provider->cancelled.end());
    EXPECT_EQ(cancelled.size(), registered.provider->cancelled.size()) << "a request was cancelled twice";
    EXPECT_EQ(kRounds, answered.load() + cancelled.size()) << "a request was both answered and cancelled, or neither";
}

TEST(ResourceProvider, AnAnswerAfterACancelIsDropped) {
    util::RunLoop loop;
    Registered registered("provider-test");
    MainResourceLoader fs(ResourceOptions{}, ClientOptions{});
    int answers = 0;
    auto request = fs.request({Resource::Unknown, "provider-test://dropped"}, [&](Response) { ++answers; });
    ASSERT_TRUE(runUntil(loop, [&] { return registered.provider->started() == 1; }, "the start"));
    request.reset();
    ASSERT_TRUE(runUntil(loop, [&] { return registered.provider->cancels() == 1; }, "the cancel"));
    FixtureLog log;
    registered.provider->responder(0)->data(bytes("late"));
    EXPECT_EQ(0u, log.uncheckedCount()) << "a late answer is expected, and is not logged";

    // Everything before it on the loader thread and the mailbox has run by then.
    bool last = false;
    registered.provider->onStart = [](const ProviderRequest&, ProviderResponder& responder) {
        responder.empty();
    };
    auto other = fs.request({Resource::Unknown, "provider-test://last"}, [&](Response) { last = true; });
    ASSERT_TRUE(runUntil(loop, [&] { return last; }, "the next request"));
    EXPECT_EQ(0, answers);
}

TEST(ResourceProvider, ACancelComesOnceOnTheLoaderThread) {
    util::RunLoop loop;
    Registered registered("provider-test");
    MainResourceLoader fs(ResourceOptions{}, ClientOptions{});
    auto request = fs.request({Resource::Unknown, "provider-test://dropped"}, [](Response) {});
    ASSERT_TRUE(runUntil(loop, [&] { return registered.provider->started() == 1; }, "the start"));
    request.reset();
    ASSERT_TRUE(runUntil(loop, [&] { return registered.provider->cancels() == 1; }, "the cancel"));

    const std::scoped_lock lock(registered.provider->mutex);
    EXPECT_EQ(registered.provider->requests[0].id, registered.provider->cancelled[0]);
    EXPECT_EQ(registered.provider->startThreads[0], registered.provider->cancelThreads[0]);
    EXPECT_NE(std::this_thread::get_id(), registered.provider->cancelThreads[0]);
}

TEST(ResourceProvider, AnAnsweredRequestGetsNoCancel) {
    util::RunLoop loop;
    Registered registered("provider-test");
    registered.provider->onStart = [](const ProviderRequest&, ProviderResponder& responder) {
        responder.data(bytes("x"));
    };
    MainResourceLoader fs(ResourceOptions{}, ClientOptions{});
    bool answered = false;
    auto request = fs.request({Resource::Unknown, "provider-test://answered"}, [&](Response) { answered = true; });
    ASSERT_TRUE(runUntil(loop, [&] { return answered; }, "the answer"));
    request.reset();
    // A later request is behind the drop on the loader thread.
    bool last = false;
    auto other = fs.request({Resource::Unknown, "provider-test://last"}, [&](Response) { last = true; });
    ASSERT_TRUE(runUntil(loop, [&] { return last; }, "the next request"));
    EXPECT_EQ(0u, registered.provider->cancels());
}

// The first answer wins, and a second one is a defect in the provider: it is logged.
TEST(ResourceProvider, ASecondAnswerIsIgnoredAndLogged) {
    util::RunLoop loop;
    Registered registered("provider-test");
    MainResourceLoader fs(ResourceOptions{}, ClientOptions{});
    int answers = 0;
    std::optional<Response> response;
    auto request = fs.request({Resource::Unknown, "provider-test://twice"}, [&](Response res) {
        ++answers;
        response = res;
    });
    ASSERT_TRUE(runUntil(loop, [&] { return registered.provider->started() == 1; }, "the start"));
    FixtureLog log;
    registered.provider->responder(0)->data(bytes("first"));
    registered.provider->responder(0)->data(bytes("second"));
    EXPECT_EQ(1u,
              log.count({EventSeverity::Warning,
                         Event::General,
                         -1,
                         "Resource provider request provider-test://twice was answered twice"}));
    ASSERT_TRUE(runUntil(loop, [&] { return answers == 1; }, "the first answer"));
    EXPECT_EQ("first", *response->data);

    bool last = false;
    registered.provider->onStart = [](const ProviderRequest&, ProviderResponder& responder) {
        responder.empty();
    };
    auto other = fs.request({Resource::Unknown, "provider-test://last"}, [&](Response) { last = true; });
    ASSERT_TRUE(runUntil(loop, [&] { return last; }, "the next request"));
    EXPECT_EQ(1, answers);
}

// How each answer reaches MapLibre, in each step. A tile asks twice: first with
// `localOnly`, then a fetch. Other resources ask once.

namespace {

enum class Outcome {
    Data,
    Empty,
    Unavailable,
    NotFound,
    Error
};

void give(ProviderResponder& responder, Outcome outcome) {
    switch (outcome) {
        case Outcome::Data:
            return responder.data(bytes("the bytes"));
        case Outcome::Empty:
            return responder.empty();
        case Outcome::Unavailable:
            return responder.unavailable("the reason");
        case Outcome::NotFound:
            return responder.notFound("the reason");
        case Outcome::Error:
            return responder.error("the reason");
    }
}

Response answerTo(Resource resource, Outcome outcome) {
    util::RunLoop loop;
    Registered registered("provider-test");
    registered.provider->onStart = [&](const ProviderRequest&, ProviderResponder& responder) {
        give(responder, outcome);
    };
    MainResourceLoader fs(ResourceOptions{}, ClientOptions{});
    std::optional<Response> response;
    auto request = fs.request(resource, [&](Response res) { response = res; });
    EXPECT_TRUE(runUntil(loop, [&] { return bool(response); }, "the answer"));
    EXPECT_EQ(1u, registered.provider->started());
    return response ? *response : Response{};
}

Resource tileResource(Resource::LoadingMethod method) {
    return Resource::tile("provider-test://tiles/{z}/{x}/{y}", 1.0f, 0, 0, 0, Tileset::Scheme::XYZ, method);
}

void expectError(const Response& response, Response::Error::Reason reason, const std::string& message) {
    ASSERT_TRUE(response.error);
    EXPECT_EQ(reason, response.error->reason);
    EXPECT_EQ(message, response.error->message);
    EXPECT_FALSE(response.data);
    EXPECT_FALSE(response.notModified);
}

void expectStoredForTheFork(const Response& response) {
    EXPECT_FALSE(response.error);
    EXPECT_TRUE(response.etag) << "the fetch step would call the provider again";
}

using Method = Resource::LoadingMethod;
using Reason = Response::Error::Reason;

} // namespace

TEST(ResourceProvider, ALocalOnlyStepAnswers) {
    const auto local = tileResource(Method::CacheOnly);

    const Response data = answerTo(local, Outcome::Data);
    expectStoredForTheFork(data);
    ASSERT_TRUE(data.data);
    EXPECT_EQ("the bytes", *data.data);

    const Response empty = answerTo(local, Outcome::Empty);
    expectStoredForTheFork(empty);
    EXPECT_TRUE(empty.noContent);

    // "Not stored", for whichever reason: the fetch step reports any error, once.
    for (const auto outcome : {Outcome::Unavailable, Outcome::NotFound, Outcome::Error}) {
        const Response response = answerTo(local, outcome);
        ASSERT_TRUE(response.error);
        EXPECT_EQ(Reason::NotFound, response.error->reason);
        EXPECT_FALSE(response.etag);
    }
}

TEST(ResourceProvider, ATileFetchStepAnswers) {
    const auto fetch = tileResource(Method::NetworkOnly);

    const Response data = answerTo(fetch, Outcome::Data);
    expectStoredForTheFork(data);
    ASSERT_TRUE(data.data);

    const Response empty = answerTo(fetch, Outcome::Empty);
    expectStoredForTheFork(empty);
    EXPECT_TRUE(empty.noContent);

    // A tile that is not there is an empty tile, not a failure.
    const Response notFound = answerTo(fetch, Outcome::NotFound);
    expectStoredForTheFork(notFound);
    EXPECT_TRUE(notFound.noContent);

    expectError(answerTo(fetch, Outcome::Unavailable), Reason::Connection, "the reason");
    expectError(answerTo(fetch, Outcome::Error), Reason::Other, "the reason");
}

TEST(ResourceProvider, ANonTileRequestAnswers) {
    for (const auto& resource :
         {Resource::source("provider-test://body"),
          Resource::glyphs("provider-test://glyphs/{fontstack}/{range}.pbf", {"Font"}, {0, 255})}) {
        const Response data = answerTo(resource, Outcome::Data);
        ASSERT_TRUE(data.data);
        EXPECT_EQ("the bytes", *data.data);
        EXPECT_FALSE(data.error);

        EXPECT_TRUE(answerTo(resource, Outcome::Empty).noContent);
        expectError(answerTo(resource, Outcome::Unavailable), Reason::Connection, "the reason");
        // Only a tile treats "not there" as empty.
        expectError(answerTo(resource, Outcome::NotFound), Reason::NotFound, "the reason");
        expectError(answerTo(resource, Outcome::Error), Reason::Other, "the reason");
    }
}

TEST(ResourceProvider, NoBytesIsAnError) {
    util::RunLoop loop;
    Registered registered("provider-test");
    registered.provider->onStart = [](const ProviderRequest&, ProviderResponder& responder) {
        responder.data(nullptr);
    };
    MainResourceLoader fs(ResourceOptions{}, ClientOptions{});
    std::optional<Response> response;
    auto request = fs.request(tileResource(Method::NetworkOnly), [&](Response res) { response = res; });
    ASSERT_TRUE(runUntil(loop, [&] { return bool(response); }, "the answer"));
    ASSERT_TRUE(response->error);
    EXPECT_EQ(Reason::Other, response->error->reason);
}

// A fetch step with the etag of the fork's own answer: the data has not changed
// (the URL names the data), so the provider is not called again, and the tile is
// not parsed again.
TEST(ResourceProvider, AFetchWithTheForksEtagIsNotModifiedAndCallsNoProvider) {
    const Response first = answerTo(tileResource(Method::CacheOnly), Outcome::Data);
    ASSERT_TRUE(first.etag);

    util::RunLoop loop;
    Registered registered("provider-test");
    MainResourceLoader fs(ResourceOptions{}, ClientOptions{});
    Resource fetch = tileResource(Method::NetworkOnly);
    fetch.priorEtag = first.etag;
    std::optional<Response> response;
    auto request = fs.request(fetch, [&](Response res) { response = res; });
    ASSERT_TRUE(runUntil(loop, [&] { return bool(response); }, "the answer"));
    EXPECT_TRUE(response->notModified);
    EXPECT_EQ(0u, registered.provider->started());

    // Another etag is not ours.
    Resource other = tileResource(Method::NetworkOnly);
    other.priorEtag = "another";
    response.reset();
    auto otherRequest = fs.request(other, [&](Response res) { response = res; });
    ASSERT_TRUE(runUntil(loop, [&] { return registered.provider->started() == 1; }, "the provider's call"));
}

// On a real map: each tile lands once, on first load and on a pan-back.

namespace {

class FrameCount final : public MapObserver {
public:
    void onDidFinishRenderingFrame(const RenderFrameStatus& status) override {
        ++frames;
        lastFrameFull = status.mode == RenderMode::Full;
    }
    int frames = 0;
    bool lastFrameFull = false;
};

class ProviderMap {
public:
    util::RunLoop loop;
    FrameCount observer;
    HeadlessFrontend frontend{Size{256, 256}, 1.0f};
    std::shared_ptr<FileSource> fileSource = std::make_shared<MainResourceLoader>(ResourceOptions{}, ClientOptions{});
    MapAdapter map{frontend, observer, fileSource, MapOptions().withMapMode(MapMode::Continuous).withSize({256, 256})};

    ProviderMap() {
        map.setPrefetchZoomDelta(0);
        map.jumpTo(CameraOptions().withCenter(LatLng{0.5, 0.5}).withZoom(3));
        map.getStyle().loadJSON(R"({"version": 8,
            "sources": {"tiles": {"type": "vector", "tiles": ["provider-test://tiles/{z}/{x}/{y}"]}},
            "layers": [{"id": "water", "type": "fill", "source": "tiles", "source-layer": "water"}]})");
    }

    bool settled(const std::string& what) {
        return runUntil(loop, [&] { return observer.lastFrameFull && observer.frames > 0; }, what);
    }
    /** One more frame after the map has settled, for anything still on its way to land. */
    bool oneMoreFrame() {
        const int before = observer.frames;
        map.triggerRepaint();
        return runUntil(loop, [&] { return observer.frames > before; }, "one more frame");
    }
};

const std::shared_ptr<const std::string>& mvt() {
    static const auto tile = std::make_shared<const std::string>(
        util::read_file("metrics/integration/tiles/14-8803-5375.mvt"));
    return tile;
}

/** The bytes that reached a tile, each one a landing; and the provider's calls by URL. */
class Landings {
public:
    Landings() {
        (void)util::LabelSteps::take();
        util::LabelSteps::setRecording(true);
    }
    ~Landings() {
        util::LabelSteps::setRecording(false);
        (void)util::LabelSteps::take();
    }
    std::size_t count() {
        for (const auto& step : util::LabelSteps::take()) {
            landings += step.sourceID == "tiles" && step.name == "hop-data";
        }
        return landings;
    }

private:
    std::size_t landings = 0;
};

std::map<std::string, std::size_t> callsByURL(TestProvider& provider, bool localOnly) {
    const std::scoped_lock lock(provider.mutex);
    std::map<std::string, std::size_t> calls;
    for (const auto& request : provider.requests) {
        if (request.localOnly == localOnly) ++calls[request.url];
    }
    return calls;
}

void expectEachCalledOnce(const std::map<std::string, std::size_t>& calls, const std::string& step) {
    for (const auto& [url, count] : calls) {
        EXPECT_EQ(1u, count) << url << " in the " << step << " step";
    }
}

} // namespace

// A stored tile: answered in its `localOnly` step, and not asked for again.
TEST(ResourceProvider, AStoredTileLandsOnceAndIsAskedForOnce) {
    Registered registered("provider-test");
    registered.provider->onStart = [](const ProviderRequest&, ProviderResponder& responder) {
        responder.data(mvt());
    };
    Landings landings;
    ProviderMap test;
    ASSERT_TRUE(test.settled("the tiles"));
    ASSERT_TRUE(test.oneMoreFrame());
    const auto local = callsByURL(*registered.provider, true);
    EXPECT_FALSE(local.empty());
    expectEachCalledOnce(local, "local");
    EXPECT_TRUE(callsByURL(*registered.provider, false).empty()) << "a stored tile was fetched";
    EXPECT_EQ(local.size(), landings.count()) << "a tile did not land exactly once";
}

// An item tile: nothing stored, so it comes in the fetch step.
TEST(ResourceProvider, AFetchedTileLandsOnce) {
    Registered registered("provider-test");
    registered.provider->onStart = [](const ProviderRequest& request, ProviderResponder& responder) {
        if (request.localOnly) {
            responder.notFound("not stored");
        } else {
            responder.data(mvt());
        }
    };
    Landings landings;
    ProviderMap test;
    ASSERT_TRUE(test.settled("the tiles"));
    ASSERT_TRUE(test.oneMoreFrame());
    const auto fetched = callsByURL(*registered.provider, false);
    EXPECT_FALSE(fetched.empty());
    expectEachCalledOnce(fetched, "fetch");
    EXPECT_EQ(fetched.size(), landings.count()) << "a tile did not land exactly once";
}

TEST(ResourceProvider, AnEmptyTileLandsOnce) {
    Registered registered("provider-test");
    registered.provider->onStart = [](const ProviderRequest&, ProviderResponder& responder) {
        responder.empty();
    };
    Landings landings;
    ProviderMap test;
    ASSERT_TRUE(test.settled("the tiles"));
    ASSERT_TRUE(test.oneMoreFrame());
    const auto local = callsByURL(*registered.provider, true);
    EXPECT_FALSE(local.empty());
    expectEachCalledOnce(local, "local");
    EXPECT_TRUE(callsByURL(*registered.provider, false).empty()) << "an empty tile was fetched";
    EXPECT_EQ(local.size(), landings.count()) << "an empty tile did not land exactly once";
}

// A tile taken back from the pyramid's cache asks for no bytes and parses nothing.
TEST(ResourceProvider, APanBackCallsNoProviderAndParsesNothing) {
    Registered registered("provider-test");
    registered.provider->onStart = [](const ProviderRequest& request, ProviderResponder& responder) {
        if (request.localOnly) {
            responder.notFound("not stored");
        } else {
            responder.data(mvt());
        }
    };
    Landings landings;
    ProviderMap test;
    ASSERT_TRUE(test.settled("the tiles"));
    ASSERT_TRUE(test.oneMoreFrame());

    test.observer.lastFrameFull = false;
    test.map.jumpTo(CameraOptions().withCenter(LatLng{40, 100}));
    ASSERT_TRUE(test.settled("the tiles away"));
    ASSERT_TRUE(test.oneMoreFrame());
    const std::size_t startedAway = registered.provider->started();
    const std::size_t landingsAway = landings.count();

    test.observer.lastFrameFull = false;
    test.map.jumpTo(CameraOptions().withCenter(LatLng{0.5, 0.5}));
    ASSERT_TRUE(test.settled("the tiles back"));
    ASSERT_TRUE(test.oneMoreFrame());

    EXPECT_EQ(startedAway, registered.provider->started()) << "a tile taken from the cache called the provider";
    EXPECT_EQ(landingsAway, landings.count()) << "a tile taken from the cache landed again";
}

// A URL of a scheme that nothing serves fails at once and loudly. It never goes to
// the disk cache or the network.
TEST(ResourceProvider, AnUnregisteredSchemeIsAnError) {
    util::RunLoop loop;
    MainResourceLoader fs(ResourceOptions{}, ClientOptions{});
    EXPECT_FALSE(fs.canRequest({Resource::Unknown, "provider-test://nobody"}));
    for (const auto method : {Method::All, Method::CacheOnly, Method::NetworkOnly}) {
        std::optional<Response> response;
        auto request = fs.request({Resource::Unknown, "provider-test://nobody", std::nullopt, method},
                                  [&](Response res) { response = res; });
        ASSERT_TRUE(runUntil(loop, [&] { return bool(response); }, "the answer"));
        expectError(*response, Reason::Other, "Unsupported resource request.");
    }
}

TEST(ResourceProvider, TheNetworkAndTheDiskCacheTakeOnlyTheirSchemes) {
    OnlineFileSource onlineSource(ResourceOptions{}, ClientOptions{});
    DatabaseFileSource databaseSource(ResourceOptions{}, ClientOptions{});
    const FileSource& online = onlineSource;
    const FileSource& database = databaseSource;
    for (const std::string url : {"http://a/b", "https://a/b", "HTTP://a/b"}) {
        EXPECT_TRUE(online.canRequest({Resource::Unknown, url})) << url;
        EXPECT_TRUE(database.canRequest({Resource::Unknown, url})) << url;
    }
    for (const std::string url : {"provider-test://a/b", "httpx://a/b", "file://a/b", "asset://a/b", "http"}) {
        EXPECT_FALSE(online.canRequest({Resource::Unknown, url})) << url;
        EXPECT_FALSE(database.canRequest({Resource::Unknown, url})) << url;
    }
}

TEST(ResourceProvider, TheTileServersSchemeStaysTheNetworksScheme) {
    const ResourceOptions options =
        ResourceOptions{}.withTileServerOptions(TileServerOptions::MapTilerConfiguration()).clone();
    OnlineFileSource onlineSource(options, ClientOptions{});
    DatabaseFileSource databaseSource(options, ClientOptions{});
    const FileSource& online = onlineSource;
    const FileSource& database = databaseSource;
    EXPECT_TRUE(online.canRequest({Resource::Unknown, "maptiler://maps/streets"}));
    EXPECT_TRUE(database.canRequest({Resource::Unknown, "maptiler://maps/streets"}));
    EXPECT_FALSE(online.canRequest({Resource::Unknown, "mapbox://maps/streets"}));
}

// A resource transform can turn any URL into one the network can read.
TEST(ResourceProvider, AResourceTransformLetsTheNetworkTakeAnyScheme) {
    util::RunLoop loop;
    OnlineFileSource onlineSource(ResourceOptions{}, ClientOptions{});
    FileSource& online = onlineSource;
    EXPECT_FALSE(online.canRequest({Resource::Unknown, "custom://a/b"}));
    online.setResourceTransform(
        {[](Resource::Kind, const std::string& url, ResourceTransform::FinishedCallback finish) {
            finish(url);
        }});
    EXPECT_TRUE(online.canRequest({Resource::Unknown, "custom://a/b"}));
    online.setResourceTransform({});
    EXPECT_FALSE(online.canRequest({Resource::Unknown, "custom://a/b"}));
}

// The requests in flight, and their peak, per provider. A request is in flight
// from its start until its answer or its cancel. There is no limit.
TEST(ResourceProvider, TheCountsFollowTheRequestsInFlight) {
    util::RunLoop loop;
    EXPECT_FALSE(getResourceProviderCounts("provider-test"));
    Registered registered("provider-test");
    const auto counts = [] {
        return *getResourceProviderCounts("provider-test");
    };
    EXPECT_EQ(0u, counts().inFlight);
    EXPECT_EQ(0u, counts().peakInFlight);

    MainResourceLoader fs(ResourceOptions{}, ClientOptions{});
    auto first = fs.request({Resource::Unknown, "provider-test://1"}, [](Response) {});
    auto second = fs.request({Resource::Unknown, "provider-test://2"}, [](Response) {});
    auto third = fs.request({Resource::Unknown, "provider-test://3"}, [](Response) {});
    ASSERT_TRUE(runUntil(loop, [&] { return registered.provider->started() == 3; }, "the starts"));
    EXPECT_EQ(3u, counts().inFlight);
    EXPECT_EQ(3u, counts().peakInFlight);

    // A second answer does not count again.
    registered.provider->responder(0)->empty();
    {
        FixtureLog log;
        registered.provider->responder(0)->empty();
        EXPECT_EQ(1u,
                  log.count({EventSeverity::Warning,
                             Event::General,
                             -1,
                             "Resource provider request provider-test://1 was answered twice"}));
    }
    EXPECT_EQ(2u, counts().inFlight);

    second.reset();
    ASSERT_TRUE(runUntil(loop, [&] { return registered.provider->cancels() == 1; }, "the cancel"));
    EXPECT_EQ(1u, counts().inFlight);

    registered.provider->responder(2)->data(bytes("x"));
    EXPECT_EQ(0u, counts().inFlight);
    EXPECT_EQ(3u, counts().peakInFlight);

    // An answer to a dropped request has already ended it.
    registered.provider->responder(1)->empty();
    EXPECT_EQ(0u, counts().inFlight);
}

TEST(ResourceProvider, AnotherProviderCountsFromZero) {
    util::RunLoop loop;
    Registered registered("provider-test");
    MainResourceLoader fs(ResourceOptions{}, ClientOptions{});
    auto first = fs.request({Resource::Unknown, "provider-test://1"}, [](Response) {});
    ASSERT_TRUE(runUntil(loop, [&] { return registered.provider->started() == 1; }, "the start"));
    EXPECT_EQ(1u, getResourceProviderCounts("provider-test")->peakInFlight);
    registerResourceProvider("provider-test", std::make_shared<TestProvider>());
    EXPECT_EQ(0u, getResourceProviderCounts("provider-test")->inFlight);
    EXPECT_EQ(0u, getResourceProviderCounts("provider-test")->peakInFlight);
    registerResourceProvider("provider-test", registered.provider);
}

// Asking again: a tile whose answer was empty or failed goes back to the provider,
// although its empty answer carries the fork's etag.

namespace {

/** The water tile's bytes once `ready` is set, and before that `early`. */
void answerWhen(TestProvider& provider, const bool& ready, Outcome early) {
    provider.onStart = [&ready, early](const ProviderRequest& request, ProviderResponder& responder) {
        if (request.localOnly) {
            responder.notFound("not stored");
        } else if (ready) {
            responder.data(mvt());
        } else {
            give(responder, early);
        }
    };
}

std::size_t drawnFills(ProviderMap& test) {
    RenderedQueryOptions options;
    options.layerIDs = std::vector<std::string>{"water"};
    return test.frontend.getRenderer()->queryRenderedFeatures(ScreenBox{{0, 0}, {256, 256}}, options).size();
}

void expectReloadFillsTheTiles(Outcome early, style::ReloadFilter filter) {
    Registered registered("provider-test");
    bool ready = false;
    answerWhen(*registered.provider, ready, early);
    ProviderMap test;
    // A failed tile's parents stay incomplete, so this map never renders "full". The
    // view's four tiles have been answered once they have been fetched.
    ASSERT_TRUE(
        runUntil(test.loop, [&] { return callsByURL(*registered.provider, false).size() >= 4; }, "the first answers"));
    ASSERT_TRUE(test.oneMoreFrame());
    ASSERT_EQ(0u, drawnFills(test));

    ready = true;
    test.map.getStyle().getSource("tiles")->as<style::VectorSource>()->reloadTiles(filter);
    // The view's own tiles, by name: a parent tile that loads later would draw too.
    EXPECT_TRUE(runUntil(
        test.loop,
        [&] {
            const auto fetched = callsByURL(*registered.provider, false);
            return std::ranges::all_of(std::array{"3/3/3", "3/3/4", "3/4/3", "3/4/4"}, [&](const std::string& tile) {
                const auto it = fetched.find("provider-test://tiles/" + tile);
                return it != fetched.end() && it->second == 2;
            });
        },
        "the view's tiles asked for again"));
    EXPECT_TRUE(runUntil(test.loop, [&] { return drawnFills(test) > 0; }, "the tiles after the reload"));
}

} // namespace

TEST(ResourceProvider, ReloadTilesAsksAgainForAnEmptyTile) {
    expectReloadFillsTheTiles(Outcome::Empty, style::ReloadFilter::Empty);
}

TEST(ResourceProvider, ReloadTilesAsksAgainForAFailedTile) {
    expectReloadFillsTheTiles(Outcome::Unavailable, style::ReloadFilter::Failed);
}

// A reload does not touch a tile that has data: no call, no second landing.
TEST(ResourceProvider, ReloadTilesLeavesATileWithData) {
    Registered registered("provider-test");
    registered.provider->onStart = [](const ProviderRequest&, ProviderResponder& responder) {
        responder.data(mvt());
    };
    Landings landings;
    ProviderMap test;
    ASSERT_TRUE(test.settled("the tiles"));
    ASSERT_TRUE(test.oneMoreFrame());
    const std::size_t started = registered.provider->started();
    const std::size_t landed = landings.count();
    test.map.getStyle().getSource("tiles")->as<style::VectorSource>()->reloadTiles(style::ReloadFilter::EmptyAndFailed);
    ASSERT_TRUE(test.oneMoreFrame());
    ASSERT_TRUE(test.oneMoreFrame());
    EXPECT_EQ(started, registered.provider->started());
    EXPECT_EQ(landed, landings.count());
}

// A reload reaches an empty tile in the pyramid's cache too: when it is shown again
// it asks, and does not take its old etag's "not modified".
TEST(ResourceProvider, AnEmptyTileInTheCacheAsksAgainWhenShownAfterAReload) {
    Registered registered("provider-test");
    bool ready = false;
    answerWhen(*registered.provider, ready, Outcome::Empty);
    ProviderMap test;
    ASSERT_TRUE(test.settled("the first answers"));
    ASSERT_TRUE(test.oneMoreFrame());

    test.observer.lastFrameFull = false;
    test.map.jumpTo(CameraOptions().withCenter(LatLng{40, 100}));
    ASSERT_TRUE(test.settled("the tiles away"));
    ASSERT_TRUE(test.oneMoreFrame());

    ready = true;
    test.map.getStyle().getSource("tiles")->as<style::VectorSource>()->reloadTiles(style::ReloadFilter::Empty);
    ASSERT_TRUE(test.oneMoreFrame());
    test.map.jumpTo(CameraOptions().withCenter(LatLng{0.5, 0.5}));
    EXPECT_TRUE(runUntil(test.loop, [&] { return drawnFills(test) > 0; }, "the tiles after the pan back"));
}

// A snapshotter takes its file source from the same process-wide manager as a map, so
// it reaches a provider. Only some platforms build the snapshotter into the core.
#if defined(__APPLE__)
TEST(ResourceProvider, ASnapshotterReachesTheProvider) {
    util::RunLoop loop;
    Registered registered("provider-test");
    registered.provider->onStart = [](const ProviderRequest& request, ProviderResponder& responder) {
        if (request.localOnly) {
            responder.notFound("not stored");
        } else {
            responder.data(mvt());
        }
    };
    MapSnapshotter snapshotter(Size{256, 256}, 1.0f, ResourceOptions{}, ClientOptions{});
    snapshotter.setStyleJSON(R"({"version": 8,
        "sources": {"tiles": {"type": "vector", "tiles": ["provider-test://tiles/{z}/{x}/{y}"]}},
        "layers": [{"id": "water", "type": "fill", "source": "tiles", "source-layer": "water",
                    "paint": {"fill-color": "#ff0000"}}]})");
    snapshotter.setCameraOptions(CameraOptions().withCenter(LatLng{0.5, 0.5}).withZoom(3));
    bool done = false;
    std::exception_ptr error;
    PremultipliedImage image;
    snapshotter.snapshot([&](std::exception_ptr failure, PremultipliedImage result, auto&&...) {
        error = failure;
        image = std::move(result);
        done = true;
    });
    ASSERT_TRUE(runUntil(loop, [&] { return done; }, "the snapshot"));
    EXPECT_EQ(nullptr, error) << util::toString(error);
    EXPECT_GT(registered.provider->started(), 0u);
    bool drewRed = false;
    for (std::size_t i = 0; i + 3 < image.bytes(); i += 4) {
        drewRed = drewRed || (image.data[i] > 200 && image.data[i + 1] < 50 && image.data[i + 3] > 200);
    }
    EXPECT_TRUE(drewRed) << "the snapshot did not draw the provider's tile";
}
#endif

// The schemes with a file source of their own still reach it, through the loader.
TEST(ResourceProvider, TheLocalSchemesStillReachTheirFileSources) {
    util::RunLoop loop;
    MainResourceLoader fs(ResourceOptions{}.withAssetPath("test/fixtures/storage/assets").clone(), ClientOptions{});
    const std::string fixtures = std::filesystem::current_path().string() + "/test/fixtures/storage/";
    for (const std::string& url : std::array<std::string, 4>{
             std::string("asset://nonempty"),
             "file://" + fixtures + "assets/nonempty",
             "mbtiles://" + fixtures + "mbtiles/geography-class-png.mbtiles",
             "pmtiles://file://" + fixtures + "pmtiles/geography-class-png.pmtiles",
         }) {
        std::optional<Response> response;
        auto request = fs.request({Resource::Source, url}, [&](Response res) { response = res; });
        ASSERT_TRUE(runUntil(loop, [&] { return bool(response); }, url));
        EXPECT_FALSE(response->error) << url << ": " << response->error->message;
        EXPECT_TRUE(response->data) << url;
    }
    // A PMTiles archive on the network: its own file source takes the URL.
    EXPECT_TRUE(fs.canRequest({Resource::Source, "pmtiles://https://example.com/a.pmtiles"}));
}
