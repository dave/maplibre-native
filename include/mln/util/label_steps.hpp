#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace mln {
namespace util {

/// One step of a GeoJSON body's work, as the step recorder saw it.
struct LabelStep {
    /// The source the step was for.
    std::string sourceID;
    /// What ran: "convert" (a body's parse in a platform binding),
    /// "parse-url" (a URL body's parse), "slice" (a tile's geojson-vt cut),
    /// "tile-parse" and "tile-layout" (the tile worker). A hop is a tile's
    /// own job on the thread that owns the tile: "hop-data" (the cut tile
    /// sent to the worker), "hop-glyphs", "hop-images", "hop-layout".
    std::string name;
    /// The name of the thread the step ran on.
    std::string thread;
    /// Whether the step is a hop (see `name`), not work.
    bool hop = false;
    /// When the step was sent to its scheduler, or zero where the recorder
    /// cannot see it (the tile worker's steps and the hops: there, the queue
    /// time is the gap after the step before it).
    std::chrono::duration<double> queued{0};
    std::chrono::duration<double> start{0};
    std::chrono::duration<double> end{0};
};

/// The step recorder, for debug builds and tests: when on, every GeoJSON
/// source's off-thread steps and tile hops record their thread and their
/// times. It is off by default, and costs one atomic read per step when off.
class LabelSteps {
public:
    static void setRecording(bool);
    static bool recording() noexcept;

    /// The steps recorded since the last call, in the order they ended.
    static std::vector<LabelStep> take();

    /// Records the step it spans, if the recorder is on when it is made.
    class Scope {
    public:
        Scope(const std::string& sourceID,
              const char* name,
              bool hop = false,
              std::chrono::duration<double> queued = std::chrono::duration<double>{0});
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        const bool on;
        LabelStep step;
    };
};

} // namespace util
} // namespace mln
