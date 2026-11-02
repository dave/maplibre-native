// The step recorder (util::LabelSteps) is off in every normal run. These tests
// check that, while it is off, it adds no heap allocation to the work it
// watches. A replaced global operator new counts the allocations made on the
// test thread while a CountAllocations is alive.

#include <mln/actor/scheduler.hpp>
#include <mln/style/conversion/geojson.hpp>
#include <mln/style/sources/geojson_source.hpp>
#include <mln/util/label_steps.hpp>

#include <mapbox/std/weak.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <functional>
#include <new>
#include <string>
#include <vector>

namespace {

thread_local std::size_t* allocationCount = nullptr;

} // namespace

// The replaced allocation functions. The array, nothrow and delete forms of
// the standard library call these.
void* operator new(std::size_t size) {
    if (allocationCount) {
        ++*allocationCount;
    }
    if (void* memory = std::malloc(size ? size : 1)) {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

using namespace mln;
using namespace mln::style;

namespace {

/// Counts the allocations on this thread while it is alive.
class CountAllocations {
public:
    CountAllocations() { allocationCount = &count; }
    ~CountAllocations() { allocationCount = nullptr; }
    CountAllocations(const CountAllocations&) = delete;
    CountAllocations& operator=(const CountAllocations&) = delete;

    std::size_t count = 0;
};

/// A scheduler that holds its tasks until the test runs them, on the test
/// thread: every allocation of a send and of its task is on this thread.
class HeldTasks final : public Scheduler {
public:
    HeldTasks() { tasks.reserve(16); }

    void schedule(std::function<void()>&& task) override { tasks.push_back(std::move(task)); }
    void schedule(const util::SimpleIdentity, std::function<void()>&& task) override { schedule(std::move(task)); }
    mapbox::base::WeakPtr<Scheduler> makeWeakPtr() override { return weakFactory.makeWeakPtr(); }
    void waitForEmpty(const util::SimpleIdentity) override {}

    void runAll() {
        while (!tasks.empty()) {
            auto task = std::move(tasks.front());
            tasks.erase(tasks.begin());
            task();
        }
    }

private:
    std::vector<std::function<void()>> tasks;
    mapbox::base::WeakPtrFactory<Scheduler> weakFactory{this};
};

/// The allocations made to send one tile's cut to the source's sequenced
/// scheduler, run it, and answer it, with the recorder off.
std::size_t allocationsOfACut(const std::string& sourceID) {
    auto held = std::make_shared<HeldTasks>();
    Scheduler* const current = Scheduler::GetCurrent(false);
    Scheduler::SetCurrent(held.get());

    conversion::Error error;
    const auto geoJSON = conversion::parseGeoJSON(R"({"type": "Point", "coordinates": [0, 0]})", error);
    EXPECT_TRUE(geoJSON) << error.message;
    auto data = GeoJSONData::create(*geoJSON, held, GeoJSONOptions::defaultOptions(), sourceID);

    std::size_t answers = 0;
    const std::function<void(GeoJSONData::TileFeatures)> answer = [&answers](const GeoJSONData::TileFeatures&) {
        ++answers;
    };
    const CanonicalTileID tile{0, 0, 0};

    // A first cut, not counted, so that anything made once is made.
    data->getTile(tile, answer, false);
    held->runAll();

    std::size_t count = 0;
    {
        const CountAllocations counting;
        data->getTile(tile, answer, false);
        held->runAll();
        count = counting.count;
    }
    EXPECT_EQ(answers, 2u);

    Scheduler::SetCurrent(current);
    return count;
}

} // namespace

// A tile's cut is sent to the source's sequenced scheduler. With the recorder
// off, the source ID must not be copied into the task: a long ID (longer than
// any small-string buffer) costs the same as an empty one.
TEST(LabelSteps, AnOffRecorderAddsNoAllocationToATilesCut) {
    util::LabelSteps::setRecording(false);
    const std::string longID(64, 's');

    const std::size_t withoutID = allocationsOfACut({});
    const std::size_t withLongID = allocationsOfACut(longID);

    EXPECT_GT(withoutID, 0u) << "the counter counts nothing";
    EXPECT_EQ(withLongID, withoutID);
}

// Every form of a step, made and ended with the recorder off, allocates
// nothing and records nothing. On, the same steps allocate and record, so the
// count is not zero for want of counting.
TEST(LabelSteps, AnOffStepAllocatesNothing) {
    const std::string longID(64, 's');
    const auto steps = [&longID] {
        const util::LabelSteps::Queued withID(longID);
        const util::LabelSteps::Queued timeOnly;
        const util::LabelSteps::Queued copied = withID;
        {
            const util::LabelSteps::Scope step(longID, "hop-data", true);
        }
        {
            const util::LabelSteps::Scope step(copied, "slice");
        }
        {
            const util::LabelSteps::Scope step(longID, "convert", timeOnly);
        }
    };

    util::LabelSteps::setRecording(false);
    (void)util::LabelSteps::take();
    std::size_t off = 0;
    {
        const CountAllocations counting;
        steps();
        off = counting.count;
    }
    EXPECT_EQ(off, 0u);
    EXPECT_TRUE(util::LabelSteps::take().empty());

    util::LabelSteps::setRecording(true);
    std::size_t on = 0;
    {
        const CountAllocations counting;
        steps();
        on = counting.count;
    }
    util::LabelSteps::setRecording(false);
    EXPECT_GT(on, 0u);

    const auto recorded = util::LabelSteps::take();
    ASSERT_EQ(recorded.size(), 3u);
    EXPECT_EQ(recorded[0].name, "hop-data");
    EXPECT_TRUE(recorded[0].hop);
    EXPECT_EQ(recorded[1].name, "slice");
    EXPECT_EQ(recorded[2].name, "convert");
    for (const auto& step : recorded) {
        EXPECT_EQ(step.sourceID, longID);
    }
    EXPECT_GT(recorded[1].queued.count(), 0.0);
    EXPECT_GT(recorded[2].queued.count(), 0.0);
}

// A step sent while the recorder is off is not recorded, even if the
// recorder is on when the step runs: the send kept no source ID.
TEST(LabelSteps, AStepSentWhileOffIsNotRecorded) {
    util::LabelSteps::setRecording(false);
    (void)util::LabelSteps::take();
    const util::LabelSteps::Queued sentWhileOff(std::string(64, 's'));
    util::LabelSteps::setRecording(true);
    {
        const util::LabelSteps::Scope step(sentWhileOff, "slice");
    }
    util::LabelSteps::setRecording(false);
    EXPECT_TRUE(util::LabelSteps::take().empty());
}
