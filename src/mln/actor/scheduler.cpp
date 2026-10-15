#include <mln/actor/scheduler.hpp>
#include <mln/util/thread_local.hpp>
#include <mln/util/thread_pool.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/logging.hpp>

#include <cassert>

namespace mln {

std::function<void()> Scheduler::bindOnce(std::function<void()> fn) {
    assert(fn);
    return [scheduler = makeWeakPtr(), scheduled = std::move(fn)]() mutable {
        if (!scheduled) return; // Repeated call.
        if (auto guard = scheduler.lock(); scheduler) {
            scheduler->schedule(std::move(scheduled));
        }
    };
}

namespace {

thread_local Scheduler* localScheduler;
} // namespace

void Scheduler::SetCurrent(Scheduler* scheduler) {
    localScheduler = scheduler;
}

Scheduler* Scheduler::GetCurrent(bool init) {
    if (!localScheduler && init) {
        static thread_local util::RunLoop runLoop;
        SetCurrent(&runLoop);
    }
    return localScheduler;
}

// static
std::shared_ptr<Scheduler> Scheduler::GetBackground() {
    static std::weak_ptr<Scheduler> weak;
    static std::mutex mtx;

    std::scoped_lock lock(mtx);
    std::shared_ptr<Scheduler> scheduler = weak.lock();

    if (!scheduler) {
        weak = scheduler = std::make_shared<ThreadPool>();
    }

    return scheduler;
}

// static
std::shared_ptr<Scheduler> Scheduler::GetSequenced() {
    constexpr std::size_t kSchedulersCount = 10;
    static std::vector<std::weak_ptr<Scheduler>> weaks(kSchedulersCount);
    static std::mutex mtx;
    static std::size_t lastUsedIndex = 0u;

    std::scoped_lock lock(mtx);

    lastUsedIndex = (lastUsedIndex + 1) % kSchedulersCount;

    if (auto scheduler = weaks[lastUsedIndex].lock()) {
        return scheduler;
    } else {
        auto result = std::make_shared<SequencedScheduler>();
        weaks[lastUsedIndex] = result;
        return result;
    }
}

namespace {

// Two threads: in the headless real-data check (LabelLaneRealData) the two
// label sources' work in series filled about 90% of a body's time to its
// layout on one thread, and two cut the median by about 10 ms (lead,
// 2026-10-15). Each mailbox still runs one message at a time.
class LabelLaneScheduler final : public ThreadedScheduler {
public:
    LabelLaneScheduler()
        : ThreadedScheduler(2, "Label lane") {}
    ~LabelLaneScheduler() override { invalidateWeakPtrsEarly(); }
};

} // namespace

// static
std::shared_ptr<Scheduler> Scheduler::GetLabelLane() {
    // Leaked on purpose: the lane lives for the life of the process, so no
    // static destructor joins its threads at exit.
    static const std::shared_ptr<Scheduler>* lane = [] {
        try {
            return new std::shared_ptr<Scheduler>(std::make_shared<LabelLaneScheduler>());
        } catch (const std::exception& error) {
            Log::Error(Event::General, std::string("The label lane cannot be made: ") + error.what());
            assert(false);
            return new std::shared_ptr<Scheduler>();
        }
    }();
    return *lane;
}

} // namespace mln
