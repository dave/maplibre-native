#include <mln/storage/resource_provider.hpp>
#include <mln/storage/resource_provider_file_source.hpp>

#include <mln/util/async_request.hpp>
#include <mln/util/client_options.hpp>

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

class Responder final : public ProviderResponder {
public:
    explicit Responder(FileSource::Callback callback_)
        : callback(std::move(callback_)) {}

    void data(std::shared_ptr<const std::string> bytes) override {
        Response response;
        response.data = std::move(bytes);
        callback(response);
    }
    void empty() override {
        Response response;
        response.noContent = true;
        callback(response);
    }
    void unavailable(std::string why) override { answerError(Response::Error::Reason::Connection, why); }
    void notFound(std::string why) override { answerError(Response::Error::Reason::NotFound, why); }
    void error(std::string why) override { answerError(Response::Error::Reason::Other, why); }

private:
    void answerError(Response::Error::Reason reason, const std::string& why) {
        Response response;
        response.error = std::make_unique<Response::Error>(reason, why);
        callback(response);
    }

    const FileSource::Callback callback;
};

class ProviderAsyncRequest final : public AsyncRequest {
public:
    ProviderAsyncRequest(std::shared_ptr<ResourceProvider> provider_, uint64_t id_)
        : provider(std::move(provider_)),
          id(id_) {}
    ~ProviderAsyncRequest() override { provider->cancel(id); }

private:
    const std::shared_ptr<ResourceProvider> provider;
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
    auto responder = std::make_shared<Responder>(std::move(callback));
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
    return std::make_unique<ProviderAsyncRequest>(provider, request.id);
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
