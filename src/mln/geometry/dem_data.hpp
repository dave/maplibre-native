#pragma once

#include <mln/math/clamp.hpp>
#include <mln/util/image.hpp>
#include <mln/util/tileset.hpp>

#include <memory>
#include <array>
#include <cassert>
#include <vector>

namespace mln {

class DEMData {
public:
    /// Width of the per-side neighbour border on the buffered image. The
    /// hillshade prepare pass renders the slopes of a tile's interior and of
    /// one texel beyond it on each side, and its Sobel kernel reads one pixel
    /// further out, so it needs two backfilled pixels. The outer slopes then
    /// equal the neighbour's own edge slopes, and linear filtering at an
    /// overzoomed tile boundary shows no seam (as in MapLibre GL JS). The
    /// contour algorithm reads the same border.
    static constexpr int32_t border = 2;

    DEMData(const PremultipliedImage& image, Tileset::RasterEncoding encoding);
    void backfillBorder(const DEMData& borderTileData, int8_t dx, int8_t dy);

    int32_t get(int32_t x, int32_t y) const;
    const std::array<float, 4>& getUnpackVector() const;
    static const std::array<float, 4>& unpackVector(Tileset::RasterEncoding);

    /// The height of one RGBA pixel of the buffered image, as `get` reads it.
    static int32_t decode(const uint8_t* rgba, const std::array<float, 4>& unpack) {
        return static_cast<int32_t>(rgba[0] * unpack[0] + rgba[1] * unpack[1] + rgba[2] * unpack[2] - unpack[3]);
    }

    /// The height of one RGBA pixel with its fraction, in the encoding's full
    /// precision (0.1 m for Mapbox, 1/256 m for Terrarium).
    static float decodeHeight(const uint8_t* rgba, const std::array<float, 4>& unpack) {
        return static_cast<float>(static_cast<double>(rgba[0]) * unpack[0] + static_cast<double>(rgba[1]) * unpack[1] +
                                  static_cast<double>(rgba[2]) * unpack[2] - unpack[3]);
    }

    const PremultipliedImage* getImage() const { return &*image; }
    const std::shared_ptr<PremultipliedImage>& getImagePtr() const { return image; }

    const int32_t dim;
    const int32_t stride;
    const Tileset::RasterEncoding encoding;

private:
    std::shared_ptr<PremultipliedImage> image;

    size_t idx(const int32_t x, const int32_t y) const {
        assert(x >= -border);
        assert(x < dim + border);
        assert(y >= -border);
        assert(y < dim + border);
        return (y + border) * stride + (x + border);
    }
};

} // namespace mln
