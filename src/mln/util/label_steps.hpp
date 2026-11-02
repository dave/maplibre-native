#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace mln {
namespace util {

/// One step of a GeoJSON body's work, as the step recorder saw it.
struct LabelStep {
    /// The source the step was for.
    std::string sourceID;
    /// What ran: "convert" (a body's parse in a platform binding),
    /// "parse-url" (a URL body's parse), "index" (the geojson-vt index
    /// build, inside the parse, or on the caller's thread for a body set
    /// with `setGeoJSON`), "slice" (a tile's geojson-vt cut),
    /// "tile-parse" and "tile-layout" (the tile worker). A hop is a tile's
    /// own job on the thread that owns the tile: "hop-data" (the cut tile
    /// sent to the worker), "hop-glyphs", "hop-images", "hop-layout", and
    /// "hop-inline-layout" (a layout run inline, in the call that asks for it).
    std::string name;
    /// The name of the thread the step ran on, for printing. Not every
    /// platform names its threads (Qt answers "unknown"): use `threadID` to
    /// tell threads apart.
    std::string thread;
    /// The thread the step ran on.
    std::thread::id threadID;
    /// Whether the step is a hop (see `name`), not work.
    bool hop = false;
    /// When the step was sent to its scheduler, or zero where the recorder
    /// cannot see it (the tile worker's steps and the hops: there, the queue
    /// time is the gap after the step before it).
    std::chrono::duration<double> queued{0};
    std::chrono::duration<double> start{0};
    std::chrono::duration<double> end{0};
};

/// The step recorder, for debug probes and tests: when on, every GeoJSON
/// source's off-thread steps and tile hops record their thread and their
/// times. It is off by default.
///
/// This is not public API: the header is not installed, and the platform
/// bindings expose it as private API only. While the recorder is off, a step
/// costs one relaxed atomic load and makes no string and no `LabelStep`.
class LabelSteps {
public:
    /// Turns the recorder on or off. Work already running when it changes may
    /// record or not: turn it on before the work to record, and `take()` after
    /// turning it off to clear what is left.
    static void setRecording(bool) noexcept;
    static bool recording() noexcept { return on.load(std::memory_order_relaxed); }

    /// The steps recorded since the last call, in the order they ended.
    static std::vector<LabelStep> take();

    class Scope;

    /// Made where a step is sent to its scheduler, and given to the step's
    /// `Scope` where it runs: the time of the send, and the source if the
    /// task has no other way to know it. Empty when the recorder is off at
    /// the send, and then the step is not recorded. Copying an empty one
    /// copies a null pointer.
    class Queued {
    public:
        /// The time only: the step's `Scope` names the source.
        Queued() {
            if (recording()) [[unlikely]] {
                start({});
            }
        }
        /// The time and the source.
        explicit Queued(const std::string& sourceID) {
            if (recording()) [[unlikely]] {
                start(sourceID);
            }
        }

    private:
        friend class Scope;
        struct Sent;
        void start(const std::string& sourceID);
        std::shared_ptr<const Sent> sent;
    };

    /// Records the step it spans, if the recorder is on when it is made.
    class Scope {
    public:
        Scope(const std::string& sourceID, const char* name, bool hop = false) {
            if (recording()) [[unlikely]] {
                begin(sourceID, name, hop, std::chrono::duration<double>{0});
            }
        }
        /// A step sent with `queued` (made with the source ID): recorded only
        /// if the recorder was on at the send and is on now.
        Scope(const Queued& queued, const char* name) {
            if (queued.sent && recording()) [[unlikely]] {
                begin(queued, nullptr, name);
            }
        }
        /// A step of `sourceID` sent with `queued` (made with or without the
        /// source ID): recorded as above.
        Scope(const std::string& sourceID, const char* name, const Queued& queued) {
            if (queued.sent && recording()) [[unlikely]] {
                begin(queued, &sourceID, name);
            }
        }
        ~Scope() {
            if (step) [[unlikely]] {
                end();
            }
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        void begin(const std::string& sourceID, const char* name, bool hop, std::chrono::duration<double> queued);
        void begin(const Queued&, const std::string* sourceID, const char* name);
        void end() noexcept;
        std::unique_ptr<LabelStep> step;
    };

private:
    static std::atomic<bool> on;
};

} // namespace util
} // namespace mln
