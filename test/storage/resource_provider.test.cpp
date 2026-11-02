// A ResourceProvider, through a real MainResourceLoader with a database, as an
// app has it. Each wait ends on an event, and its bound only turns a missing
// event into a failure.

#include <mln/storage/main_resource_loader.hpp>
#include <mln/storage/resource.hpp>
#include <mln/storage/resource_options.hpp>
#include <mln/storage/resource_provider.hpp>
#include <mln/storage/resource_provider_file_source.hpp>
#include <mln/test/fixture_log_observer.hpp>
#include <mln/test/util.hpp>
#include <mln/util/async_request.hpp>
#include <mln/util/client_options.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/timer.hpp>

#include <atomic>
#include <functional>
#include <latch>
#include <mutex>
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
