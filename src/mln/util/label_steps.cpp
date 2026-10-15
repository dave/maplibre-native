#include <mln/util/label_steps.hpp>

#include <mln/util/platform.hpp>
#include <mln/util/monotonic_timer.hpp>

#include <atomic>
#include <mutex>

namespace mln {
namespace util {

namespace {

std::atomic<bool> on{false};
std::mutex mutex;

std::vector<LabelStep>& steps() {
    static std::vector<LabelStep> recorded;
    return recorded;
}

} // namespace

void LabelSteps::setRecording(bool value) {
    on = value;
}

bool LabelSteps::recording() noexcept {
    return on;
}

std::vector<LabelStep> LabelSteps::take() {
    std::scoped_lock lock(mutex);
    std::vector<LabelStep> result;
    result.swap(steps());
    return result;
}

LabelSteps::Scope::Scope(const std::string& sourceID, const char* name, bool hop, std::chrono::duration<double> queued)
    : on(LabelSteps::recording()) {
    if (!on) return;
    step.sourceID = sourceID;
    step.name = name;
    step.thread = platform::getCurrentThreadName();
    step.hop = hop;
    step.queued = queued;
    step.start = MonotonicTimer::now();
}

LabelSteps::Scope::~Scope() {
    if (!on) return;
    step.end = MonotonicTimer::now();
    std::scoped_lock lock(mutex);
    steps().push_back(std::move(step));
}

} // namespace util
} // namespace mln
