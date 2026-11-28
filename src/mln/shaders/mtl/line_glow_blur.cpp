#include <mln/shaders/mtl/line_glow_blur.hpp>
#include <mln/shaders/shader_defines.hpp>

namespace mln {
namespace shaders {

using LineGlowBlurShaderSource = ShaderSource<BuiltIn::LineGlowBlurShader, gfx::Backend::Type::Metal>;

const std::array<AttributeInfo, 1> LineGlowBlurShaderSource::attributes = {
    AttributeInfo{0, gfx::AttributeDataType::Short2, lineGlowBlurUBOCount + 0, idLineGlowPosVertexAttribute},
};
const std::array<TextureInfo, 1> LineGlowBlurShaderSource::textures = {
    TextureInfo{0, idLineGlowImageTexture},
};

} // namespace shaders
} // namespace mln
