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
    /// hillshade prepare pass renders two "ghost" texels per side beyond a
    /// tile's interior, and its Sobel kernel reads one pixel further out,
    /// so it needs three backfilled pixels per side. Adjacent tiles' ghost
    /// outputs then read identical neighbour data, bilinear sampling at the
    /// boundary stays in shared data, and the visible step (seam) at
    /// overzoomed tile boundaries goes away. The contour algorithm reads
    /// the same border.
    static constexpr int32_t border = 3;

    DEMData(const PremultipliedImage& image, Tileset::RasterEncoding encoding);
    void backfillBorder(const DEMData& borderTileData, int8_t dx, int8_t dy);

    int32_t get(int32_t x, int32_t y) const;
    const std::array<float, 4>& getUnpackVector() const;

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
