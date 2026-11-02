#include <mln/util/label_steps.hpp>

#include <mln/util/platform.hpp>
#include <mln/util/monotonic_timer.hpp>

#include <cassert>
#include <mutex>

namespace mln {
namespace util {

namespace {

std::mutex mutex;

std::vector<LabelStep>& steps() {
    static std::vector<LabelStep> recorded;
    return recorded;
}

} // namespace

std::atomic<bool> LabelSteps::on{false};

struct LabelSteps::Queued::Sent {
    std::string sourceID;
    std::chrono::duration<double> at;
};

void LabelSteps::setRecording(bool value) noexcept {
    on.store(value, std::memory_order_relaxed);
}

std::vector<LabelStep> LabelSteps::take() {
    std::scoped_lock lock(mutex);
    std::vector<LabelStep> result;
    result.swap(steps());
    return result;
}

void LabelSteps::Queued::start(const std::string& sourceID) {
    sent = std::make_shared<const Sent>(Sent{.sourceID = sourceID, .at = MonotonicTimer::now()});
}

void LabelSteps::Scope::begin(const std::string& sourceID,
                              const char* name,
                              bool hop,
                              std::chrono::duration<double> queued) {
    step = std::make_unique<LabelStep>();
    step->sourceID = sourceID;
    step->name = name;
    step->thread = platform::getCurrentThreadName();
    step->threadID = std::this_thread::get_id();
    step->hop = hop;
    step->queued = queued;
    step->start = MonotonicTimer::now();
}

void LabelSteps::Scope::begin(const Queued& queued, const std::string* sourceID, const char* name) {
    // A time-only Queued needs the Scope that names the source.
    assert(sourceID || !queued.sent->sourceID.empty());
    begin(sourceID ? *sourceID : queued.sent->sourceID, name, false, queued.sent->at);
}

void LabelSteps::Scope::end() noexcept {
    step->end = MonotonicTimer::now();
    std::scoped_lock lock(mutex);
    steps().push_back(std::move(*step));
    step.reset();
}

} // namespace util
} // namespace mln
