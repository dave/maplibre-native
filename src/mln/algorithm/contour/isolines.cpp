#include <mln/algorithm/contour/isolines.hpp>

#include <mln/util/containers.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace mln {
namespace algorithm {
namespace contour {

namespace {

// CASES[idx][i] = i-th segment for marching-squares case `idx`.
// Each segment is { startEdge, endEdge } where each edge is one of:
//   {1, 0} top      — between TL and TR
//   {2, 1} right    — between TR and BR
//   {1, 2} bottom   — between BL and BR
//   {0, 1} left     — between TL and BL
//
// Bit layout of `idx`: TL=8 | TR=4 | BR=2 | BL=1, set if the corresponding
// sample is *strictly above* the threshold. Cases 0 and 15 (all below / all
// above) emit no segments. Cases 5 and 10 are saddle configurations and emit
// two segments — saddle disambiguation is applied later.
struct Edge {
    std::int8_t u; // 0=left  1=mid  2=right (along the cell's x axis)
    std::int8_t v; // 0=top   1=mid  2=bottom (along the cell's y axis)
};
struct Segment {
    Edge start;
    Edge end;
};

constexpr std::array<std::array<Segment, 2>, 16> CASES = {{
    /*  0 ----  */ {Segment{{0, 0}, {0, 0}}, Segment{{0, 0}, {0, 0}}},
    /*  1 ---BL */ {Segment{{1, 2}, {0, 1}}, Segment{{0, 0}, {0, 0}}},
    /*  2 --BR- */ {Segment{{2, 1}, {1, 2}}, Segment{{0, 0}, {0, 0}}},
    /*  3 --BR+BL (bottom row) */ {Segment{{2, 1}, {0, 1}}, Segment{{0, 0}, {0, 0}}},
    /*  4 -TR-- */ {Segment{{1, 0}, {2, 1}}, Segment{{0, 0}, {0, 0}}},
    /*  5 -TR+BL (saddle) */ {Segment{{1, 2}, {0, 1}}, Segment{{1, 0}, {2, 1}}},
    /*  6 -TR+BR (right column) */ {Segment{{1, 0}, {1, 2}}, Segment{{0, 0}, {0, 0}}},
    /*  7 -TR+BR+BL (only TL below) */ {Segment{{1, 0}, {0, 1}}, Segment{{0, 0}, {0, 0}}},
    /*  8 TL--- */ {Segment{{0, 1}, {1, 0}}, Segment{{0, 0}, {0, 0}}},
    /*  9 TL+BL (left column) */ {Segment{{1, 2}, {1, 0}}, Segment{{0, 0}, {0, 0}}},
    /* 10 TL+BR (saddle) */ {Segment{{0, 1}, {1, 0}}, Segment{{2, 1}, {1, 2}}},
    /* 11 TL+BR+BL (only TR below) */ {Segment{{2, 1}, {1, 0}}, Segment{{0, 0}, {0, 0}}},
    /* 12 TL+TR (top row) */ {Segment{{0, 1}, {2, 1}}, Segment{{0, 0}, {0, 0}}},
    /* 13 TL+TR+BL (only BR below) */ {Segment{{1, 2}, {2, 1}}, Segment{{0, 0}, {0, 0}}},
    /* 14 TL+TR+BR (only BL below) */ {Segment{{0, 1}, {1, 2}}, Segment{{0, 0}, {0, 0}}},
    /* 15 ---- */ {Segment{{0, 0}, {0, 0}}, Segment{{0, 0}, {0, 0}}},
}};

constexpr std::array<int, 16> SEGMENT_COUNT = {0, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 0};

inline double ratio(double a, double threshold, double b) {
    return (threshold - a) / (b - a);
}

inline void edgePosition(const Edge edge,
                         int c,
                         int r,
                         double tl,
                         double tr,
                         double bl,
                         double br,
                         double threshold,
                         double& outX,
                         double& outY) {
    if (edge.u == 0) {
        outX = static_cast<double>(c - 1);
        outY = static_cast<double>(r) - ratio(bl, threshold, tl);
    } else if (edge.u == 2) {
        outX = static_cast<double>(c);
        outY = static_cast<double>(r) - ratio(br, threshold, tr);
    } else if (edge.v == 0) {
        outX = static_cast<double>(c) - ratio(tr, threshold, tl);
        outY = static_cast<double>(r - 1);
    } else {
        outX = static_cast<double>(c) - ratio(br, threshold, bl);
        outY = static_cast<double>(r);
    }
}

inline std::int32_t scaleCoord(double v, double multiplier) {
    return static_cast<std::int32_t>(std::lround(v * multiplier));
}

// Edge slots are unique across the whole grid. Each cell occupies a 2×2
// block of slots; adjacent cells share their boundary slots. The encoding
// matches `maplibre-contour`'s isolines.ts so the algorithm produces the
// same connectivity. Returns a 64-bit packed slot index — comfortably within
// range for any practical tile size.
inline std::uint64_t edgeIndex(int width, int c, int r, Edge edge) {
    const std::int64_t x = static_cast<std::int64_t>(c) * 2 + edge.u;
    const std::int64_t y = static_cast<std::int64_t>(r) * 2 + edge.v;
    const std::int64_t stride = static_cast<std::int64_t>(width + 1) * 2;
    return static_cast<std::uint64_t>(x + y * stride);
}

// One in-progress polyline at a single threshold. `start` and `end` are edge
// slot indices. The points are interleaved (x, y) tile-local int coords, in
// two vectors because a polyline grows at both ends: `tail` holds the
// appended points in order, `head` the prepended ones as (x, y) pairs in
// reverse, so the line is reversePairs(head) ++ tail. (A deque costs a 4 KB
// block for every new fragment.) `merged` is set when this fragment has been
// folded into another and should be ignored in the final emission pass.
struct Fragment {
    std::uint64_t start;
    std::uint64_t end;
    std::vector<std::int32_t> head;
    std::vector<std::int32_t> tail;
    bool merged = false;

    std::size_t size() const { return head.size() + tail.size(); }
};

struct LevelState {
    std::vector<Fragment> fragments;
    mln::unordered_map<std::uint64_t, std::size_t> byStart;
    mln::unordered_map<std::uint64_t, std::size_t> byEnd;
};

// Appends the (x, y) pairs of `points` to `out`, last pair first.
inline void appendReversedPairs(std::vector<std::int32_t>& out, const std::vector<std::int32_t>& points) {
    for (std::size_t i = points.size(); i >= 2; i -= 2) {
        out.push_back(points[i - 2]);
        out.push_back(points[i - 1]);
    }
}

// f ++ g into f, copying the shorter of the two. g is left empty.
inline void concatenate(Fragment& f, Fragment& g) {
    if (g.size() <= f.size()) {
        appendReversedPairs(f.tail, g.head);
        f.tail.insert(f.tail.end(), g.tail.begin(), g.tail.end());
    } else {
        // Prepend f to g: g's head gains f reversed, that is reversePairs(f.tail) ++ f.head.
        appendReversedPairs(g.head, f.tail);
        g.head.insert(g.head.end(), f.head.begin(), f.head.end());
        std::swap(f.head, g.head);
        std::swap(f.tail, g.tail);
    }
    g.head.clear();
    g.tail.clear();
}

} // namespace

std::vector<ContourLineString> generateContours(std::span<const std::int16_t> heights,
                                                int width,
                                                int height,
                                                const ContourThresholds& thresholds) {
    if (thresholds.interval <= 0.0) {
        return {};
    }
    if (width < 2 || height < 2) {
        return {};
    }
    if (static_cast<std::ptrdiff_t>(heights.size()) < static_cast<std::ptrdiff_t>(width) * height) {
        return {};
    }

    const double interval = thresholds.interval;
    const double multiplier = static_cast<double>(thresholds.extent) / static_cast<double>(width - 1);

    // Per-threshold polyline accumulators. We key by level rounded to the
    // nearest interval-step, expressed as a signed integer multiple, to avoid
    // floating-point map-key headaches. The output is emitted in this map's
    // order, so it stays a std::unordered_map filled in scan order; `byKey`
    // finds a level's state without hashing (the map's elements stay in
    // place, so the pointers stay valid).
    std::unordered_map<long long, LevelState> levels;
    const auto [minHeight, maxHeight] =
        std::minmax_element(heights.begin(), heights.begin() + static_cast<std::ptrdiff_t>(width) * height);
    // A tiny interval over a tall grid would make `byKey` huge; then every
    // lookup hashes instead (the same result, only slower).
    const long long minKey = static_cast<long long>(std::floor(*minHeight / interval)) - 1;
    const long long maxKey = static_cast<long long>(std::ceil(*maxHeight / interval)) + 1;
    constexpr long long maxCachedLevels = 1 << 16;
    std::vector<LevelState*> byKey(
        maxKey - minKey < maxCachedLevels ? static_cast<std::size_t>(maxKey - minKey + 1) : 0, nullptr);

    // Iterate cells in [1, width) × [1, height): each cell at (c, r)
    // samples corners at (c-1, r-1), (c, r-1), (c-1, r), (c, r), so this
    // covers all (width-1) × (height-1) cells exactly once. Skirts beyond
    // the grid edge are the caller's responsibility (e.g. by passing a
    // border-padded height grid).
    for (int r = 1; r < height; r++) {
        const std::int16_t* above = heights.data() + static_cast<std::size_t>(r - 1) * width;
        const std::int16_t* below = heights.data() + static_cast<std::size_t>(r) * width;
        for (int c = 1; c < width; c++) {
            const double tl = static_cast<double>(above[c - 1]);
            const double tr = static_cast<double>(above[c]);
            const double bl = static_cast<double>(below[c - 1]);
            const double br = static_cast<double>(below[c]);

            const double minV = std::min({tl, tr, bl, br});
            const double maxV = std::max({tl, tr, bl, br});
            const double startLevel = std::ceil(minV / interval) * interval;
            const double endLevel = std::floor(maxV / interval) * interval;

            for (double level = startLevel; level <= endLevel + 0.5 * interval; level += interval) {
                const int idx = (tl > level ? 8 : 0) | (tr > level ? 4 : 0) | (br > level ? 2 : 0) |
                                (bl > level ? 1 : 0);
                const int nSegments = SEGMENT_COUNT[idx];
                if (nSegments == 0) continue;

                const long long levelKey = std::llround(level / interval);
                LevelState* found = nullptr;
                if (byKey.empty()) {
                    found = &levels[levelKey];
                } else {
                    LevelState*& slot = byKey[static_cast<std::size_t>(levelKey - minKey)];
                    if (slot == nullptr) slot = &levels[levelKey];
                    found = slot;
                }
                LevelState& state = *found;

                for (int s = 0; s < nSegments; s++) {
                    const Segment seg = CASES[idx][s];
                    double sx, sy, ex, ey;
                    edgePosition(seg.start, c, r, tl, tr, bl, br, level, sx, sy);
                    edgePosition(seg.end, c, r, tl, tr, bl, br, level, ex, ey);
                    const std::int32_t sxi = scaleCoord(sx, multiplier);
                    const std::int32_t syi = scaleCoord(sy, multiplier);
                    const std::int32_t exi = scaleCoord(ex, multiplier);
                    const std::int32_t eyi = scaleCoord(ey, multiplier);

                    const std::uint64_t startIdx = edgeIndex(width, c, r, seg.start);
                    const std::uint64_t endIdx = edgeIndex(width, c, r, seg.end);

                    auto fByEnd = state.byEnd.find(startIdx);
                    auto fByStart = state.byStart.find(endIdx);

                    if (fByEnd != state.byEnd.end()) {
                        // Some fragment ends at our start. Extend its tail.
                        const std::size_t fIdx = fByEnd->second;
                        Fragment& f = state.fragments[fIdx];
                        state.byEnd.erase(fByEnd);

                        if (fByStart != state.byStart.end()) {
                            const std::size_t gIdx = fByStart->second;
                            state.byStart.erase(fByStart);
                            if (fIdx == gIdx) {
                                // Closing a ring: append the closing point and emit f as-is.
                                f.tail.push_back(exi);
                                f.tail.push_back(eyi);
                                // Ring is complete; remove from any maps (already done) and
                                // leave it in `fragments` to be emitted at the end.
                            } else {
                                // Connect two distinct fragments.
                                Fragment& g = state.fragments[gIdx];
                                concatenate(f, g);
                                f.end = g.end;
                                g.merged = true;
                                state.byEnd[f.end] = fIdx;
                            }
                        } else {
                            // Just extend f's tail with the new endpoint.
                            f.tail.push_back(exi);
                            f.tail.push_back(eyi);
                            f.end = endIdx;
                            state.byEnd[endIdx] = fIdx;
                        }
                    } else if (fByStart != state.byStart.end()) {
                        // Some fragment starts at our end. Prepend our start.
                        const std::size_t fIdx = fByStart->second;
                        Fragment& f = state.fragments[fIdx];
                        state.byStart.erase(fByStart);
                        f.head.push_back(sxi);
                        f.head.push_back(syi);
                        f.start = startIdx;
                        state.byStart[startIdx] = fIdx;
                    } else {
                        // New fragment.
                        const std::size_t fIdx = state.fragments.size();
                        state.fragments.push_back(Fragment{startIdx, endIdx, {}, {sxi, syi, exi, eyi}, false});
                        state.byStart[startIdx] = fIdx;
                        state.byEnd[endIdx] = fIdx;
                    }
                }
            }
        }
    }

    // Collect surviving fragments into ContourLineStrings.
    std::vector<ContourLineString> result;
    for (auto& [levelKey, state] : levels) {
        const double level = static_cast<double>(levelKey) * interval;
        for (auto& f : state.fragments) {
            if (f.merged) continue;
            if (f.size() < 4) continue;
            ContourLineString line;
            line.elevation = level;
            line.points.reserve(f.size());
            appendReversedPairs(line.points, f.head);
            line.points.insert(line.points.end(), f.tail.begin(), f.tail.end());
            result.push_back(std::move(line));
        }
    }

    return result;
}

} // namespace contour
} // namespace algorithm
} // namespace mln
