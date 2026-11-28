#pragma once

#include <mln/shaders/line_glow_ubo.hpp>
#include <mln/shaders/shader_source.hpp>
#include <mln/shaders/mtl/shader_program.hpp>

namespace mln {
namespace shaders {

constexpr auto lineGlowBlurShaderPrelude = R"(

enum {
    idLineGlowBlurUBO = drawableReservedUBOCount,
    lineGlowBlurUBOCount
};

struct alignas(16) LineGlowBlurUBO {
    /*  0 */ float step_x;
    /*  4 */ float step_y;
    /*  8 */ float sigma;
    /* 12 */ float opacity;
    /* 16 */
};
static_assert(sizeof(LineGlowBlurUBO) == 16, "wrong size");

)";

/// A full-target quad that samples one image with a 1D Gaussian (or a plain copy when sigma < 0.5).
template <>
struct ShaderSource<BuiltIn::LineGlowBlurShader, gfx::Backend::Type::Metal> {
    static constexpr auto name = "LineGlowBlurShader";
    static constexpr auto vertexMainFunction = "vertexMain";
    static constexpr auto fragmentMainFunction = "fragmentMain";

    static const std::array<AttributeInfo, 1> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 1> textures;

    static constexpr auto prelude = lineGlowBlurShaderPrelude;
    static constexpr auto source = R"(

struct VertexStage {
    short2 pos [[attribute(0)]];
};

struct FragmentStage {
    float4 position [[position, invariant]];
    float2 uv;
};

FragmentStage vertex vertexMain(thread const VertexStage vertx [[stage_in]]) {
    const float2 pos = float2(vertx.pos);
    return {
        .position = float4(pos.x * 2.0 - 1.0, 1.0 - pos.y * 2.0, 0.0, 1.0),
        .uv       = pos,
    };
}

half4 fragment fragmentMain(FragmentStage in [[stage_in]],
                            device const LineGlowBlurUBO& props [[buffer(idLineGlowBlurUBO)]],
                            texture2d<float, access::sample> image [[texture(0)]],
                            sampler image_sampler [[sampler(0)]]) {
#if defined(OVERDRAW_INSPECTOR)
    return half4(1.0);
#endif
    const float sigma = props.sigma;
    if (sigma < 0.5) {
        return half4(image.sample(image_sampler, in.uv) * props.opacity);
    }
    const int n = min(int(ceil(sigma * 2.5)), 64);
    const float2 step = float2(props.step_x, props.step_y);
    const float k = -0.5 / (sigma * sigma);
    float4 sum = float4(0.0);
    float wsum = 0.0;
    for (int i = -n; i <= n; ++i) {
        const float w = exp(k * float(i * i));
        sum += w * image.sample(image_sampler, in.uv + step * float(i));
        wsum += w;
    }
    return half4(sum / wsum * props.opacity);
}
)";
};

} // namespace shaders
} // namespace mln
