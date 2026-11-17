// A symbol quad's corner offsets from its anchor are packed as int16 at
// 1/SYMBOL_OFFSET_PACK_FACTOR layout units (#966). At the old factor (32) a
// corner more than 1,024 units from its anchor wrapped by 2048 units: a tall map
// card's last rows drew 1,195 pt away from the card, and a glyph across the
// wrap drew as a long thin spindle. These tests pin the pack, the decode every
// shader uses, and the error past the limit.

#include <mln/test/util.hpp>

#include <mln/geometry/anchor.hpp>
#include <mln/renderer/buckets/symbol_bucket.hpp>
#include <mln/shaders/shader_source.hpp>
#include <mln/text/quads.hpp>

#if MLN_RENDER_BACKEND_OPENGL
#include <mln/shaders/gl/symbol_icon.hpp>
#include <mln/shaders/gl/symbol_sdf.hpp>
#include <mln/shaders/gl/symbol_text_and_icon.hpp>
#elif MLN_RENDER_BACKEND_METAL
#include <mln/shaders/mtl/symbol.hpp>
#elif MLN_RENDER_BACKEND_VULKAN
#include <mln/shaders/vulkan/symbol.hpp>
#elif MLN_RENDER_BACKEND_WEBGPU
#include <mln/shaders/webgpu/symbol.hpp>
#endif

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

using namespace mln;

namespace {

// The value comes from a runtime variable, so the compiler cannot fold the
// pack and the test measures the code that runs.
float runtime(const float value) {
    volatile float v = value;
    return v;
}

// The int16 a value would have had at the old factor, wrapped (two's
// complement), as the old `static_cast<int16_t>(std::round(v * 32))` gave on
// arm64 and x86-64.
int16_t wrappedAt32(const float value) {
    const auto packed = static_cast<int32_t>(std::round(value * 32.0f));
    return static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(packed) & 0xFFFFu));
}

// Packs a quad whose four corners sit `y` units below the anchor and returns
// the four packed y values, through the packer this backend uses.
std::vector<int16_t> packedCornerYs(const float y, bool& overflow) {
    const Point<float> corner{1.0f, y};
    const SymbolQuad quad{corner,
                          corner,
                          corner,
                          corner,
                          Rect<uint16_t>{0, 0, 1, 1},
                          WritingModeType::None,
                          Point<float>{0.0f, 0.0f},
                          false,
                          Point<float>{0.0f, 0.0f},
                          Point<float>{0.0f, 0.0f},
                          Point<float>{1.0f, 1.0f}};
    const Range<float> size{14.0f, 14.0f};
#if MLN_USE_SYMBOL_INSTANCING
    const Anchor anchor{100.0f, 100.0f, 0.0f};
    const auto attributes = SymbolBucket::layoutAttributes(quad, anchor, size, overflow);
    // offset_tltr = tl.x, tl.y, tr.x, tr.y; offset_blbr = bl.x, bl.y, br.x, br.y
    return {attributes.a2[1], attributes.a2[3], attributes.a3[1], attributes.a3[3]};
#else
    std::vector<int16_t> ys;
    for (const auto& c : {quad.tl, quad.tr, quad.bl, quad.br}) {
        const auto attributes = SymbolBucket::layoutAttributes(
            {100.0f, 100.0f}, c, 0.0f, 0, 0, size, false, {0.0f, 0.0f}, quad.minFontScale, overflow);
        ys.push_back(attributes.a1[3]); // pos_offset = anchor.x, anchor.y, offset.x, offset.y
    }
    return ys;
#endif
}

// Decodes a packed offset as every symbol shader does: a_offset / factor.
float decode(const int16_t packed) {
    return static_cast<float>(packed) / SYMBOL_OFFSET_PACK_FACTOR;
}

} // namespace

TEST(SymbolOffsetPack, OffsetPastOldLimitDecodes) {
    // 1,100 units below the anchor: past the old limit (1,024), inside the new
    // one (4,096). At 14 pt text that is 642 pt, where the 22-row card of #966
    // had its last rows.
    const float input = runtime(1100.0f);
    // What the old pack gave: 35,200 wraps to -30,336, which decodes to -948 units.
    ASSERT_EQ(-30336, wrappedAt32(input));

    bool overflow = false;
    const auto ys = packedCornerYs(input, overflow);
    ASSERT_EQ(4u, ys.size());
    EXPECT_FALSE(overflow);
    for (const auto packed : ys) {
        EXPECT_NE(static_cast<float>(wrappedAt32(input)) / 32.0f, decode(packed));
        EXPECT_FLOAT_EQ(input, decode(packed));
    }

    // And the other side of the anchor.
    const float above = runtime(-1100.0f);
    bool overflowAbove = false;
    for (const auto packed : packedCornerYs(above, overflowAbove)) {
        EXPECT_FLOAT_EQ(above, decode(packed));
    }
    EXPECT_FALSE(overflowAbove);
}

TEST(SymbolOffsetPack, OffsetPastLimitReportsError) {
    // 5,000 units: past even the new limit. The packer reports it and does not
    // wrap: the value stays at the limit, on its own side of the anchor.
    const float input = runtime(5000.0f);
    const float limit = std::numeric_limits<int16_t>::max() / SYMBOL_OFFSET_PACK_FACTOR;
    ASSERT_GT(input, limit);

    bool overflow = false;
    const auto ys = packedCornerYs(input, overflow);
    ASSERT_EQ(4u, ys.size());
    EXPECT_TRUE(overflow);
    for (const auto packed : ys) {
        EXPECT_GT(packed, 0) << "a wrap puts the corner on the other side of the anchor";
        EXPECT_FLOAT_EQ(limit, decode(packed));
    }

    const float below = runtime(-5000.0f);
    bool overflowBelow = false;
    for (const auto packed : packedCornerYs(below, overflowBelow)) {
        EXPECT_LT(packed, 0);
    }
    EXPECT_TRUE(overflowBelow);

    // Inside the limit nothing is reported.
    bool overflowInside = false;
    packedCornerYs(runtime(4000.0f), overflowInside);
    EXPECT_FALSE(overflowInside);
}

namespace {

// The lines of a vertex shader that decode `a_offset`: the ones that use it
// in a division (its declarations do not divide).
std::vector<std::string> decodeLines(const std::string& source) {
    std::vector<std::string> lines;
    std::istringstream in(source);
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("a_offset") != std::string::npos && line.find('/') != std::string::npos) {
            lines.push_back(line);
        }
    }
    return lines;
}

void expectDecodesWithThePackFactor(const std::string& name, const std::string& source) {
    char factor[32];
    std::snprintf(factor, sizeof(factor), "/ %.1f", static_cast<double>(SYMBOL_OFFSET_PACK_FACTOR));
    const auto lines = decodeLines(source);
    ASSERT_EQ(1u, lines.size()) << name << ": expected one a_offset decode";
    EXPECT_NE(std::string::npos, lines.front().find(factor))
        << name << " decodes a_offset with another factor than SYMBOL_OFFSET_PACK_FACTOR (" << factor
        << "): " << lines.front();
}

} // namespace

TEST(SymbolOffsetPack, EveryCompiledSymbolShaderDecodesWithThePackFactor) {
    using shaders::BuiltIn;
#if MLN_RENDER_BACKEND_OPENGL
    constexpr auto backend = gfx::Backend::Type::OpenGL;
#define MLN_TEST_SYMBOL_VERTEX(Shader) shaders::ShaderSource<BuiltIn::Shader, backend>::vertex
#elif MLN_RENDER_BACKEND_METAL
    constexpr auto backend = gfx::Backend::Type::Metal;
#define MLN_TEST_SYMBOL_VERTEX(Shader) shaders::ShaderSource<BuiltIn::Shader, backend>::source
#elif MLN_RENDER_BACKEND_VULKAN
    constexpr auto backend = gfx::Backend::Type::Vulkan;
#define MLN_TEST_SYMBOL_VERTEX(Shader) shaders::ShaderSource<BuiltIn::Shader, backend>::vertex
#elif MLN_RENDER_BACKEND_WEBGPU
    constexpr auto backend = gfx::Backend::Type::WebGPU;
#define MLN_TEST_SYMBOL_VERTEX(Shader) shaders::ShaderSource<BuiltIn::Shader, backend>::vertex
#endif
    expectDecodesWithThePackFactor("SymbolIconShader", MLN_TEST_SYMBOL_VERTEX(SymbolIconShader));
    expectDecodesWithThePackFactor("SymbolSDFShader", MLN_TEST_SYMBOL_VERTEX(SymbolSDFShader));
    expectDecodesWithThePackFactor("SymbolTextAndIconShader", MLN_TEST_SYMBOL_VERTEX(SymbolTextAndIconShader));
#undef MLN_TEST_SYMBOL_VERTEX
}
