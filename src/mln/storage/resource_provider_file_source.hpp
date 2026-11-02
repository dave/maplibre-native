#pragma once

#include <mln/storage/file_source.hpp>

#include <mutex>

namespace mln {

/// Gives a resource whose URL scheme has a registered `ResourceProvider` to that
/// provider. Its callback runs on the thread the provider answers on, so the
/// caller's callback must be safe on any thread, as `MainResourceLoader`'s is.
class ResourceProviderFileSource final : public FileSource {
public:
    ResourceProviderFileSource(const ResourceOptions&, const ClientOptions&);

    std::unique_ptr<AsyncRequest> request(const Resource&, Callback) override;
    bool canRequest(const Resource&) const override;

    void setResourceOptions(ResourceOptions) override;
    ResourceOptions getResourceOptions() override;
    void setClientOptions(ClientOptions) override;
    ClientOptions getClientOptions() override;

private:
    std::mutex optionsMutex;
    ResourceOptions resourceOptions;
    ClientOptions clientOptions;
};

} // namespace mln
