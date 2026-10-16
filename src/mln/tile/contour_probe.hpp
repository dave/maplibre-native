#pragma once

// SCRATCH (scratch/contour-profile): timing and counts for the contour
// profile, and switches for the lossless levers. Not for commit.

#include <mapbox/feature.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace mln {
namespace contour_probe {

struct Stats {
    int populates = 0;  // populateFromDEM calls
    int jobsRun = 0;    // background jobs that ran to the end
    int jobsSkipped = 0; // background jobs that saw a newer generation and stopped
    int applied = 0;    // results given to setData
    int dropped = 0;    // results dropped on reply as superseded
    double snapshotMs = 0, decodeMs = 0, marchMs = 0, shiftMs = 0, clipMs = 0, dpMs = 0, ch1Ms = 0, ch2Ms = 0,
           postMs = 0, buildMs = 0;
    std::size_t marchLines = 0, marchPoints = 0, clipPoints = 0, dpPoints = 0, ch1Points = 0, ch2Points = 0,
                postPoints = 0, features = 0;
};

// Lever (a): a Douglas-Peucker pass after the Chaikin passes, in tile units. 0 = off.
inline std::atomic<double> postEpsilon{0.0};
// Lever (d): copy the raw image on the render thread and decode on the worker.
inline std::atomic<bool> rawSnapshot{false};
// Lever (c): a job that sees a newer generation stops before its work.
inline std::atomic<bool> skipStale{false};
// Lever (c): populate each notified tile once, after the render thread's current messages.
inline std::atomic<bool> coalesce{false};
// The faster marching squares (same output).
inline std::atomic<bool> fastMarch{false};

inline std::mutex mutex;
inline Stats stats;
// The features last applied to each tile, by "z/x/y".
inline std::map<std::string, mapbox::feature::feature_collection<std::int16_t>> lastFeatures;

inline Stats take() {
    std::lock_guard<std::mutex> lock(mutex);
    Stats out = stats;
    stats = Stats{};
    return out;
}

} // namespace contour_probe
} // namespace mln
