in vec2 v_pos;
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
