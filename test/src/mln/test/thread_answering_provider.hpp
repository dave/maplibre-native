#pragma once

#include <mln/storage/resource_provider.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mln {

/// A `ResourceProvider` that answers each request from another thread, as an app's
/// does. Its bodies are by URL; any other URL is "not found". Its destructor joins
/// its threads, so it must outlive only the answers, not the map.
class ThreadAnsweringProvider final : public ResourceProvider {
public:
    ~ThreadAnsweringProvider() override {
        const std::scoped_lock lock(mutex);
        for (auto& thread : threads) {
            thread.join();
        }
    }

    void setBody(const std::string& url, const std::string& body) {
        const std::scoped_lock lock(mutex);
        bodies[url] = std::make_shared<const std::string>(body);
    }

    void start(const ProviderRequest& request, std::shared_ptr<ProviderResponder> responder) override {
        const std::scoped_lock lock(mutex);
        const auto it = bodies.find(request.url);
        auto body = it == bodies.end() ? nullptr : it->second;
        threads.emplace_back([body = std::move(body), responder = std::move(responder), url = request.url] {
            if (body) {
                responder->data(body);
            } else {
                responder->notFound("No body for " + url);
            }
        });
    }

    void cancel(uint64_t) override {}

private:
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<const std::string>> bodies;
    std::vector<std::thread> threads;
};

} // namespace mln
