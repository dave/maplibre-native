// Generated code, do not modify this file!
#pragma once
#include <mln/shaders/shader_source.hpp>

namespace mln {
namespace shaders {

template <>
struct ShaderSource<BuiltIn::LineGlowBlurShader, gfx::Backend::Type::OpenGL> {
    static constexpr const char* name = "LineGlowBlurShader";
    static constexpr const char* vertex = R"(layout (location = 0) in vec2 a_pos;
out vec2 v_pos;

void main() {
    gl_Position = vec4(a_pos.x * 2.0 - 1.0, 1.0 - a_pos.y * 2.0, 0.0, 1.0);

    v_pos.x = a_pos.x;
    v_pos.y = 1.0 - a_pos.y;
}
)";
    static constexpr const char* fragment = R"(in vec2 v_pos;
uniform sampler2D u_image;

layout (std140) uniform LineGlowBlurUBO {
    highp float u_step_x;
    highp float u_step_y;
    highp float u_sigma;
    highp float u_opacity;
};

void main() {
    if (u_sigma < 0.5) {
        fragColor = texture(u_image, v_pos) * u_opacity;
    } else {
        int n = min(int(ceil(u_sigma * 2.5)), 64);
        vec2 step = vec2(u_step_x, u_step_y);
        float k = -0.5 / (u_sigma * u_sigma);
        vec4 sum = vec4(0.0);
        float wsum = 0.0;
        for (int i = -64; i <= 64; ++i) {
            if (i < -n || i > n) {
                continue;
            }
            float w = exp(k * float(i * i));
            sum += w * texture(u_image, v_pos + step * float(i));
            wsum += w;
        }
        fragColor = sum / wsum * u_opacity;
    }

#ifdef OVERDRAW_INSPECTOR
    fragColor = vec4(0.0);
#endif
}
)";
};

} // namespace shaders
} // namespace mln
