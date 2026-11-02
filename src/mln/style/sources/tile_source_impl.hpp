#pragma once

#include <mln/style/sources/tile_source.hpp>
#include <mln/style/source_impl.hpp>

namespace mln {
namespace style {

class TileSource::Impl : public Source::Impl {
public:
    Impl(SourceType sourceType, std::string id, uint16_t tileSize);
    Impl(const Impl&, Tileset);

    uint16_t getTileSize() const;

    std::optional<std::string> getAttribution() const final;

    const std::optional<Tileset> tileset;

    /// Count the calls to `TileSource::reloadTiles` for each kind of tile. The
    /// render source asks those tiles again when a count changes.
    uint64_t emptyTileReloads = 0;
    uint64_t failedTileReloads = 0;

private:
    uint16_t tileSize;
};

} // namespace style
} // namespace mln
