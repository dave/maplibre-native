#pragma once

#include <mln/style/source.hpp>
#include <mln/util/tileset.hpp>
#include <mln/util/variant.hpp>

namespace mln {

class AsyncRequest;

namespace style {

class TileSource : public Source {
public:
    TileSource(std::string id, variant<std::string, Tileset> urlOrTileset, uint16_t tileSize, SourceType sourceType);
    ~TileSource() override;

    const variant<std::string, Tileset>& getURLOrTileset() const;
    std::optional<std::string> getURL() const;

    uint16_t getTileSize() const;

    class Impl;
    const Impl& impl() const;

    void loadDescription(FileSource&) final;

    /// Asks the network again for each tile of this source whose last answer
    /// was empty (no content) or an error, and that the view needs now. The
    /// tile keeps what it draws until the new answer lands. Tiles with data
    /// are not asked again, and none is cleared. A tile that is not needed
    /// now asks again when the view needs it, as before.
    void reloadEmptyTiles();

protected:
    Mutable<Source::Impl> createMutable() const noexcept final;
    virtual void setTilesetOverrides(Tileset& tileset);

private:
    const variant<std::string, Tileset> urlOrTileset;
    std::unique_ptr<AsyncRequest> req;
};

template <>
inline bool Source::is<TileSource>() const {
    return getType() == SourceType::Raster;
}

} // namespace style
} // namespace mln
