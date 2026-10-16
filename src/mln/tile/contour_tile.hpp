#pragma once

#include <mln/actor/scheduler.hpp>
#include <mln/algorithm/contour/isolines.hpp>
#include <mln/algorithm/contour/units.hpp>
#include <mln/tile/geometry_tile.hpp>

#include <mapbox/std/weak.hpp>

#include <mln/util/image.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace mln {

class RasterDEMTile;
class TileParameters;

// A vector-tile that renders contour-line features for a single (z, x, y).
//
// `populateFromDEM` copies the buffered (dim + 2·border) DEM image on the
// render thread; the background scheduler decodes the heights, runs the
// marching squares and the smoothing and builds the features; on reply
// (render thread) they go to the standard `GeometryTile::setData` path. A
// job that a newer call has superseded skips its work. A `WeakPtrFactory` guards
// against the tile being destroyed (panned away) before the background
// work completes.
//
// Tile-edge continuity is preserved by sampling DEMData's neighbour-
// border (populated by `RenderRasterDEMSource` via `backfillBorder` as
// adjacent DEM tiles arrive). Lines crossing the tile edge are clipped
// against the tile bbox in `toFeatures` so adjacent tiles' smoothed
// contours meet exactly at the seam.
class ContourTile final : public GeometryTile {
public:
    ContourTile(const OverscaledTileID&, std::string sourceID, const TileParameters&, TileObserver* = nullptr);

    // Populate the tile from an upstream DEM tile that just finished parsing.
    // `intervalDisplayUnits` is the contour spacing in the source's display
    // unit (e.g. 100 feet, 50 metres) — the source's `intervals` schedule is
    // resolved to a single value per the tile's zoom by the caller.
    // `majorMultiplier` is the source's `majorMultiplier` schedule resolved
    // to a single positive integer per the tile's zoom by the caller; lines
    // at elevations divisible by `intervalDisplayUnits × majorMultiplier`
    // get `major: true`. `unit` controls the metres↔display conversion.
    //
    // Each call starts a new generation. The work runs on the tile's thread
    // pool, so calls can finish in any order; only the newest generation's
    // result is applied (see `populatedGeneration`).
    void populateFromDEM(const RasterDEMTile& demTile,
                         double intervalDisplayUnits,
                         std::int64_t majorMultiplier,
                         const algorithm::contour::UnitConfig& unit);

    // A copy of a DEM tile's buffered image as it is now, to populate from
    // later: the DEM tile can change (a backfill) or go away meanwhile.
    struct DEMSnapshot {
        std::shared_ptr<const PremultipliedImage> image;
        int dim = 0;
        std::array<float, 4> unpack{};
    };
    // Nothing when the DEM tile has no data.
    static std::optional<DEMSnapshot> snapshot(const RasterDEMTile&);

    // The marching squares every contour tile runs:
    // algorithm::contour::generateContours. Tests swap in the version from
    // before it was made faster, to draw both on the same renderer.
    using MarchingSquares = std::vector<algorithm::contour::ContourLineString> (*)(
        std::span<const std::int16_t>, int, int, const algorithm::contour::ContourThresholds&);
    static std::atomic<MarchingSquares> marchingSquares;

    // As `populateFromDEM`, from a snapshot.
    void populate(const DEMSnapshot&,
                  double intervalDisplayUnits,
                  std::int64_t majorMultiplier,
                  const algorithm::contour::UnitConfig& unit);

    // The generation of the last result applied to the tile: 0 before any
    // result, otherwise the number of the `populateFromDEM` call it came from.
    std::uint64_t populatedGeneration() const { return appliedGeneration; }

private:
    TaggedScheduler threadPool;
    std::uint64_t requestedGeneration = 0;
    std::uint64_t appliedGeneration = 0;
    // `requestedGeneration` for the jobs, which skip their work once a newer
    // call has been made.
    std::shared_ptr<std::atomic<std::uint64_t>> latestGeneration = std::make_shared<std::atomic<std::uint64_t>>(0);

    mapbox::base::WeakPtrFactory<ContourTile> weakFactory{this};
    // Do not add members here, see `WeakPtrFactory`.
};

} // namespace mln
