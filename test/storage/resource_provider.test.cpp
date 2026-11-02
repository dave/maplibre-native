// A ResourceProvider, through a real MainResourceLoader with a database, as an
// app has it. Each wait ends on an event, and its bound only turns a missing
// event into a failure.

#include <mln/storage/main_resource_loader.hpp>
#include <mln/storage/resource.hpp>
#include <mln/storage/resource_options.hpp>
#include <mln/storage/resource_provider.hpp>
#include <mln/test/util.hpp>
#include <mln/util/async_request.hpp>
#include <mln/util/client_options.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/timer.hpp>

#include <functional>
#include <mutex>
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
