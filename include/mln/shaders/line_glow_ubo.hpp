#pragma once

#include <mln/shaders/layer_ubo.hpp>

namespace mln {
namespace shaders {

/// One pass of the line glow: a 1D Gaussian over the source image along `step`.
/// Viatrail: see RenderLineLayer, "line-glow-radius".
struct alignas(16) LineGlowBlurUBO {
    /*  0 */ float step_x; // one source texel along the blur direction, in texture coordinates
    /*  4 */ float step_y;
    /*  8 */ float sigma; // in source texels; below 0.5 the pass copies the image
    /* 12 */ float opacity;
    /* 16 */
};
static_assert(sizeof(LineGlowBlurUBO) == 16);

} // namespace shaders
} // namespace mln
