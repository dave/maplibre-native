#pragma once

#include <mln/layout/pattern_layout.hpp>
#include <mln/renderer/paint_property_binder.hpp>
#include <mln/renderer/render_layer.hpp>
#include <mln/shaders/segment.hpp>
#include <mln/shaders/uniforms.hpp>
#include <mln/style/image_impl.hpp>
#include <mln/style/layers/line_layer_impl.hpp>
#include <mln/style/layers/line_layer_properties.hpp>

#include <array>
#include <memory>

namespace mln {

namespace gfx {
class ShaderGroup;
class UniformBuffer;
using ShaderGroupPtr = std::shared_ptr<ShaderGroup>;
using UniformBufferPtr = std::shared_ptr<UniformBuffer>;
} // namespace gfx

class LayerTweaker;
class RenderTarget;
using RenderTargetPtr = std::shared_ptr<RenderTarget>;
namespace gfx {
class ShaderProgramBase;
using ShaderProgramBasePtr = std::shared_ptr<ShaderProgramBase>;
} // namespace gfx

class LineBucket;
class LineLayerTweaker;
using LineBinders = PaintPropertyBinders<style::LinePaintProperties::DataDrivenProperties>;
using LineLayerTweakerPtr = std::shared_ptr<LineLayerTweaker>;

class RenderLineLayer final : public RenderLayer {
public:
    explicit RenderLineLayer(Immutable<style::LineLayer::Impl>);
    ~RenderLineLayer() override;

    /// Generate any changes needed by the layer
    void update(gfx::ShaderRegistry &,
                gfx::Context &,
                const TransformState &,
                const std::shared_ptr<UpdateParameters> &,
                const PaintParameters &,
                const RenderTree &,
                UniqueChangeRequestVec &) override;

    void markLayerRenderable(bool willRender, UniqueChangeRequestVec &) override;
    void layerRemoved(UniqueChangeRequestVec &) override;
    std::size_t removeAllDrawables() override;

private:
    std::size_t removeTile(RenderPass, const OverscaledTileID &) override;

    /// Glow mode (line-glow-radius > 0): the lines draw into glowTargets[0] instead of the
    /// layer group; [1] blurs it horizontally, [2] vertically, and the layer group draws [2]
    /// onto the map. All three are a quarter of the viewport.
    bool updateGlow(gfx::ShaderRegistry &, gfx::Context &, const TransformState &, UniqueChangeRequestVec &);
    void teardownGlow(UniqueChangeRequestVec &);
    void activateGlowTargets(bool activate, UniqueChangeRequestVec &);

    void transition(const TransitionParameters &) override;
    void evaluate(const PropertyEvaluationParameters &) override;
    bool hasTransition() const override;
    bool hasCrossfade() const override;
    void prepare(const LayerPrepareParameters &) override;

    bool queryIntersectsFeature(const GeometryCoordinates &,
                                const GeometryTileFeature &,
                                float,
                                const TransformState &,
                                float,
                                const mat4 &,
                                const FeatureState &) const override;

    void captureRenderedFeatures(const LineBucket &,
                                 const RenderTile &,
                                 const LineBinders &,
                                 const style::LinePaintProperties::PossiblyEvaluated &,
                                 const TransformState &,
                                 const TransformParameters &);

    // Paint properties
    style::LinePaintProperties::Unevaluated unevaluated;

    float getLineWidth(const GeometryTileFeature &, float, const FeatureState &) const;
    void updateColorRamp();

    std::shared_ptr<PremultipliedImage> colorRamp;
    // The global-state snapshot used to generate the cached color ramp and detect when it needs to be rebuilt.
    std::shared_ptr<const GlobalStateMap> colorRampGlobalState;
    gfx::Texture2DPtr colorRampTexture2D;

    gfx::ShaderGroupPtr lineShaderGroup;
    gfx::ShaderGroupPtr lineGradientShaderGroup;
    gfx::ShaderGroupPtr lineSDFShaderGroup;
    gfx::ShaderGroupPtr lineGradientSDFShaderGroup;
    gfx::ShaderGroupPtr linePatternShaderGroup;

    bool glowMode = false;
    std::array<RenderTargetPtr, 3> glowTargets;
    gfx::ShaderProgramBasePtr glowShader;
    std::array<std::shared_ptr<LayerTweaker>, 3> glowTweakers; // [0] composite, [1] horizontal, [2] vertical
    SegmentVector glowSegments;
};

} // namespace mln
