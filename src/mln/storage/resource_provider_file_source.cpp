#include <mln/storage/resource_provider.hpp>
#include <mln/storage/resource_provider_file_source.hpp>

#include <mln/util/async_request.hpp>
#include <mln/util/client_options.hpp>
#include <mln/util/logging.hpp>

#include <atomic>
#include <map>
#include <shared_mutex>
#include <stdexcept>

namespace mln {

namespace {

std::shared_mutex providersMutex;
std::map<std::string, std::shared_ptr<ResourceProvider>, std::less<>>& providers() {
    // Leaked on purpose: a map's loader thread can outlive static destruction.
    static auto* table = new std::map<std::string, std::shared_ptr<ResourceProvider>, std::less<>>();
    return *table;
}

std::shared_ptr<ResourceProvider> providerOf(const std::string& url) {
    const auto end = url.find("://");
    if (end == std::string::npos) {
        return nullptr;
    }
    const std::shared_lock lock(providersMutex);
    const auto it = providers().find(std::string_view(url).substr(0, end));
    return it == providers().end() ? nullptr : it->second;
}

uint64_t nextRequestID() {
    static std::atomic<uint64_t> last{0};
    return ++last;
}

// One request. One atomic state decides how it ends: an answer or a cancel, never both.
class Responder final : public ProviderResponder {
public:
    Responder(FileSource::Callback callback_, std::string url_)
        : callback(std::move(callback_)),
          url(std::move(url_)) {}

    void data(std::shared_ptr<const std::string> bytes) override {
        Response response;
        response.data = std::move(bytes);
        answer(response);
    }
    void empty() override {
        Response response;
        response.noContent = true;
        answer(response);
    }
    void unavailable(std::string why) override { answerError(Response::Error::Reason::Connection, why); }
    void notFound(std::string why) override { answerError(Response::Error::Reason::NotFound, why); }
    void error(std::string why) override { answerError(Response::Error::Reason::Other, why); }

    /// True if the request had no answer yet: it is cancelled now.
    bool cancel() {
        State expected = State::Waiting;
        return state.compare_exchange_strong(expected, State::Cancelled);
    }

private:
    enum class State : uint8_t {
        Waiting,
        Answered,
        Cancelled
    };

    void answerError(Response::Error::Reason reason, const std::string& why) {
        Response response;
        response.error = std::make_unique<Response::Error>(reason, why);
        answer(response);
    }

    void answer(const Response& response) {
        State expected = State::Waiting;
        if (state.compare_exchange_strong(expected, State::Answered)) {
            callback(response);
        } else if (expected == State::Answered) {
            // An answer that races a cancel is expected. A second answer is not.
            Log::Warning(Event::General, "Resource provider request " + url + " was answered twice");
        }
    }

    const FileSource::Callback callback;
    const std::string url;
    std::atomic<State> state{State::Waiting};
};

class ProviderAsyncRequest final : public AsyncRequest {
public:
    ProviderAsyncRequest(std::shared_ptr<ResourceProvider> provider_,
                         std::shared_ptr<Responder> responder_,
                         uint64_t id_)
        : provider(std::move(provider_)),
          responder(std::move(responder_)),
          id(id_) {}
    ~ProviderAsyncRequest() override {
        if (responder->cancel()) {
            provider->cancel(id);
        }
    }

private:
    const std::shared_ptr<ResourceProvider> provider;
    const std::shared_ptr<Responder> responder;
    const uint64_t id;
};

} // namespace

void registerResourceProvider(const std::string& scheme, std::shared_ptr<ResourceProvider> provider) {
    if (scheme.empty()) {
        throw std::invalid_argument("registerResourceProvider: empty scheme");
    }
    if (!provider) {
        throw std::invalid_argument("registerResourceProvider: null provider for " + scheme);
    }
    const std::unique_lock lock(providersMutex);
    providers()[scheme] = std::move(provider);
}

void unregisterResourceProvider(const std::string& scheme) {
    const std::unique_lock lock(providersMutex);
    providers().erase(scheme);
}

ResourceProviderFileSource::ResourceProviderFileSource(const ResourceOptions& resourceOptions_,
                                                       const ClientOptions& clientOptions_)
    : resourceOptions(resourceOptions_.clone()),
      clientOptions(clientOptions_.clone()) {}

std::unique_ptr<AsyncRequest> ResourceProviderFileSource::request(const Resource& resource, Callback callback) {
    auto provider = providerOf(resource.url);
    auto responder = std::make_shared<Responder>(std::move(callback), resource.url);
    if (!provider) {
        // Removed since `canRequest` said yes.
        responder->error("No resource provider for " + resource.url);
        return std::make_unique<AsyncRequest>();
    }
    const ProviderRequest request{.id = nextRequestID(),
                                  .kind = resource.kind,
                                  .url = resource.url,
                                  .sourceID = resource.sourceID,
                                  .tile = resource.tileData,
                                  .localOnly = !resource.hasLoadingMethod(Resource::LoadingMethod::Network),
                                  .priority = resource.priority};
    provider->start(request, responder);
    return std::make_unique<ProviderAsyncRequest>(provider, responder, request.id);
}

bool ResourceProviderFileSource::canRequest(const Resource& resource) const {
    return providerOf(resource.url) != nullptr;
}

void ResourceProviderFileSource::setResourceOptions(ResourceOptions options) {
    const std::scoped_lock lock(optionsMutex);
    resourceOptions = options;
}

ResourceOptions ResourceProviderFileSource::getResourceOptions() {
    const std::scoped_lock lock(optionsMutex);
    return resourceOptions.clone();
}

void ResourceProviderFileSource::setClientOptions(ClientOptions options) {
    const std::scoped_lock lock(optionsMutex);
    clientOptions = options;
}

ClientOptions ResourceProviderFileSource::getClientOptions() {
    const std::scoped_lock lock(optionsMutex);
    return clientOptions.clone();
}

} // namespace mln
