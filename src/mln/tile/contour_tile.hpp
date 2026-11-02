#pragma once

#include <mln/actor/scheduler.hpp>
#include <mln/algorithm/contour/isolines.hpp>
#include <mln/algorithm/contour/units.hpp>
#include <mln/tile/geometry_tile.hpp>

#include <mapbox/feature.hpp>
#include <mapbox/std/weak.hpp>

#include <mln/util/image.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
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

    // How to build a tile's lines.
    struct LineOptions {
        // The contour spacing in the source's display unit (e.g. 100 feet, 50
        // metres): the source's `intervals` schedule resolved for the tile.
        double intervalDisplayUnits = 0.0;
        // The source's `majorMultiplier` schedule resolved for the tile. Lines
        // at elevations divisible by `intervalDisplayUnits × majorMultiplier`
        // get `major: true`.
        std::int64_t majorMultiplier = 0;
        // Controls the metres↔display conversion.
        algorithm::contour::UnitConfig unit;
        // The tile's size in pixels at its own zoom. It sets the simplify
        // tolerance, which is half of such a pixel: a tile of 8192 units across
        // 512 pixels has 16 units to a pixel. It does not grow with overscaling.
        double tileSize = 512.0;
    };

    // Populate the tile from an upstream DEM tile that just finished parsing.
    //
    // Each call starts a new generation. One build runs on the tile's thread
    // pool at a time. A call made while one runs waits for it, and only the
    // newest such call is built, so neighbours arriving together cost one more
    // build. Only the newest generation's result is applied (see
    // `populatedGeneration`), so a tile draws once for those calls.
    void populateFromDEM(const RasterDEMTile& demTile, const LineOptions&);

    // A copy of a DEM tile's buffered image as it is now, to populate from
    // later: the DEM tile can change (a backfill) or go away meanwhile.
    struct DEMSnapshot {
        std::shared_ptr<const PremultipliedImage> image;
        int dim = 0;
        std::array<float, 4> unpack{};
    };
    // Nothing when the DEM tile has no data.
    static std::optional<DEMSnapshot> snapshot(const RasterDEMTile&);

    // The line features of a snapshot. Runs on any thread.
    static mapbox::feature::feature_collection<std::int16_t> buildFeatures(const DEMSnapshot&, const LineOptions&);

    // As `populateFromDEM`, from a snapshot.
    void populate(const DEMSnapshot&, const LineOptions&);

    // The DEM tile has no data: it had no content or it failed. The tile
    // completes with no lines, and any population still running is dropped.
    void setNoData();

    // The generation of the last result applied to the tile: 0 before any
    // result, otherwise the number of the `populateFromDEM` call it came from.
    std::uint64_t populatedGeneration() const { return appliedGeneration; }

private:
    void startBuild(const DEMSnapshot&, const LineOptions&);

    struct PendingBuild {
        DEMSnapshot snapshot;
        LineOptions options;
    };

    TaggedScheduler threadPool;
    bool buildRunning = false;
    // The newest call made while a build ran.
    std::optional<PendingBuild> nextBuild;
    std::uint64_t requestedGeneration = 0;
    std::uint64_t appliedGeneration = 0;
    // `requestedGeneration` for the jobs, which skip their work once a newer
    // call has been made.
    std::shared_ptr<std::atomic<std::uint64_t>> latestGeneration = std::make_shared<std::atomic<std::uint64_t>>(0);

    mapbox::base::WeakPtrFactory<ContourTile> weakFactory{this};
    // Do not add members here, see `WeakPtrFactory`.
};

} // namespace mln
