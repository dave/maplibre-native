#include <mln/renderer/layers/render_line_layer.hpp>

#include <mln/geometry/feature_index.hpp>
#include <mln/geometry/line_atlas.hpp>
#include <mln/gfx/cull_face_mode.hpp>
#include <mln/gfx/drawable_atlases_tweaker.hpp>
#include <mln/gfx/drawable_builder.hpp>
#include <mln/gfx/line_drawable_data.hpp>
#include <mln/gfx/shader_registry.hpp>
#include <mln/renderer/buckets/line_bucket.hpp>
#include <mln/renderer/image_manager.hpp>
#include <mln/renderer/layer_group.hpp>
#include <mln/renderer/layers/line_layer_tweaker.hpp>
#include <mln/renderer/paint_parameters.hpp>
#include <mln/renderer/render_source.hpp>
#include <mln/renderer/render_static_data.hpp>
#include <mln/renderer/render_target.hpp>
#include <mln/renderer/change_request.hpp>
#include <mln/renderer/buckets/heatmap_bucket.hpp>
#include <mln/shaders/line_glow_ubo.hpp>
#include <mln/gfx/context.hpp>
#include <mln/renderer/render_tile.hpp>
#include <mln/renderer/render_tree.hpp>
#include <mln/renderer/tile_render_data.hpp>
#include <mln/renderer/update_parameters.hpp>
#include <mln/renderer/upload_parameters.hpp>
#include <mln/shaders/line_layer_ubo.hpp>
#include <mln/shaders/shader_program_base.hpp>
#include <mln/style/expression/image.hpp>
#include <mln/style/layers/line_layer_impl.hpp>
#include <mln/tile/geometry_tile.hpp>
#include <mln/tile/tile.hpp>
#include <mln/util/convert.hpp>
#include <mln/util/intersection_tests.hpp>
#include <mln/util/logging.hpp>
#include <mln/util/math.hpp>

namespace mln {

using namespace style;
using namespace shaders;

namespace {

inline const LineLayer::Impl& impl_cast(const Immutable<style::Layer::Impl>& impl) {
    assert(impl->getTypeInfo() == LineLayer::Impl::staticTypeInfo());
    return static_cast<const LineLayer::Impl&>(*impl);
}

const auto posNormalAttribName = "a_pos_normal";

// Viatrail line glow (line-glow-radius). The offscreen images are this fraction of the viewport.
constexpr float glowScale = 0.25f;

/// Sets one glow pass's UBO each frame: a 1D Gaussian over `source` along `direction`,
/// or a plain copy (sigma 0) for the final draw onto the map.
class LineGlowBlurTweaker final : public LayerTweaker {
public:
    LineGlowBlurTweaker(std::string id_,
                        Immutable<style::LayerProperties> properties,
                        gfx::Texture2DPtr source_,
                        std::array<float, 2> direction_)
        : LayerTweaker(std::move(id_), std::move(properties)),
          source(std::move(source_)),
          direction(direction_) {}

    void execute(LayerGroupBase& layerGroup, const PaintParameters& parameters) override {
        if (layerGroup.empty()) {
            return;
        }
        const auto& evaluated = static_cast<const LineLayerProperties&>(*evaluatedProperties).evaluated;
        const auto size = source->getSize();
        const bool blur = direction[0] != 0.f || direction[1] != 0.f;
        // line-glow-radius is in pixels (points); sigma = radius / 2, converted to source texels.
        const float sigma = blur ? evaluated.get<LineGlowRadius>() * 0.5f * parameters.pixelRatio * glowScale : 0.f;
        const LineGlowBlurUBO ubo = {
            .step_x = size.width ? direction[0] / static_cast<float>(size.width) : 0.f,
            .step_y = size.height ? direction[1] / static_cast<float>(size.height) : 0.f,
            .sigma = sigma,
            .opacity = 1.f,
        };
        layerGroup.mutableUniformBuffers().createOrUpdate(idLineGlowBlurUBO, &ubo, parameters.context);
    }

private:
    gfx::Texture2DPtr source;
    std::array<float, 2> direction;
};

} // namespace

RenderLineLayer::RenderLineLayer(Immutable<style::LineLayer::Impl> _impl)
    : RenderLayer(makeMutable<LineLayerProperties>(std::move(_impl))),
      unevaluated(impl_cast(baseImpl).paint.untransitioned()),
      colorRamp(std::make_shared<PremultipliedImage>(Size(256, 1))) {
    styleDependencies = unevaluated.getDependencies();
}

RenderLineLayer::~RenderLineLayer() = default;

void RenderLineLayer::transition(const TransitionParameters& parameters) {
    unevaluated = impl_cast(baseImpl).paint.transitioned(parameters, std::move(unevaluated));
    styleDependencies = unevaluated.getDependencies();
    updateColorRamp();

#if MLN_RENDER_BACKEND_METAL
    if (auto* tweaker = static_cast<LineLayerTweaker*>(layerTweaker.get())) {
        tweaker->updateGPUExpressions(unevaluated, parameters.now);
    }
#endif // MLN_RENDER_BACKEND_METAL
}

void RenderLineLayer::evaluate(const PropertyEvaluationParameters& parameters) {
    if (colorRampGlobalState != parameters.globalState) {
        const auto& rampValue = unevaluated.get<LineGradient>().getValue();
        const bool rampAffected = (rampValue.getDependencies() & style::expression::Dependency::GlobalState) &&
                                  style::expression::globalStateRefsIntersect(rampValue.getGlobalStateRefs(),
                                                                              parameters.changedGlobalStateKeys.get());
        colorRampGlobalState = parameters.globalState;
        if (rampAffected) {
            updateColorRamp();
        }
    }

    const auto previousProperties = staticImmutableCast<LineLayerProperties>(evaluatedProperties);
    auto properties = makeMutable<LineLayerProperties>(staticImmutableCast<LineLayer::Impl>(baseImpl),
                                                       parameters.getCrossfadeParameters(),
                                                       unevaluated.evaluate(parameters, previousProperties->evaluated));
    auto& evaluated = properties->evaluated;

    passes = (evaluated.get<style::LineOpacity>().constantOr(1.0) > 0 &&
              evaluated.get<style::LineColor>().constantOr(Color::black()).a > 0 &&
              evaluated.get<style::LineWidth>().constantOr(1.0) > 0)
                 ? RenderPass::Translucent
                 : RenderPass::None;
    properties->renderPasses = mln::underlying_type(passes);
    evaluatedProperties = std::move(properties);

    for (const auto& glowTweaker : glowTweakers) {
        if (glowTweaker) {
            glowTweaker->updateProperties(evaluatedProperties);
        }
    }
    if (auto* tweaker = static_cast<LineLayerTweaker*>(layerTweaker.get())) {
        tweaker->updateProperties(evaluatedProperties);
#if MLN_RENDER_BACKEND_METAL
        tweaker->updateGPUExpressions(unevaluated, parameters.now);
#endif // MLN_RENDER_BACKEND_METAL
    }
}

bool RenderLineLayer::hasTransition() const {
    return unevaluated.hasTransition();
}

bool RenderLineLayer::hasCrossfade() const {
    return getCrossfade<LineLayerProperties>(evaluatedProperties).t != 1;
}

void RenderLineLayer::prepare(const LayerPrepareParameters& params) {
    RenderLayer::prepare(params);
    for (const RenderTile& tile : *renderTiles) {
        const LayerRenderData* renderData = tile.getLayerRenderData(*baseImpl);
        if (!renderData) continue;

        const auto& evaluated = getEvaluated<LineLayerProperties>(renderData->layerProperties);
        if (evaluated.get<LineDasharray>().from.empty()) continue;

        const auto& bucket = static_cast<const LineBucket&>(*renderData->bucket);
        const LinePatternCap cap = bucket.layout.get<LineCap>() == LineCapType::Round ? LinePatternCap::Round
                                                                                      : LinePatternCap::Square;
        // Ensures that the dash data gets added to the atlas.
        params.lineAtlas.getDashPatternTexture(
            evaluated.get<LineDasharray>().from, evaluated.get<LineDasharray>().to, cap);
    }
}

namespace {

GeometryCollection offsetLine(const GeometryCollection& rings, double offset) {
    assert(offset != 0.0f);
    assert(!rings.empty());

    GeometryCollection newRings;
    newRings.reserve(rings.size());

    const Point<double> zero(0, 0);
    for (const auto& ring : rings) {
        newRings.emplace_back();
        auto& newRing = newRings.back();
        newRing.reserve(ring.size());

        for (auto i = ring.begin(); i != ring.end(); ++i) {
            auto& p = *i;

            Point<double> aToB = i == ring.begin() ? zero : util::perp(util::unit(convertPoint<double>(p - *(i - 1))));
            // At the last vertex, use the incoming normal so the endpoint receives the full offset.
            Point<double> bToC = i + 1 == ring.end() ? aToB
                                                     : util::perp(util::unit(convertPoint<double>(*(i + 1) - p)));
            Point<double> extrude = util::unit(aToB + bToC);

            const double cosHalfAngle = extrude.x * bToC.x + extrude.y * bToC.y;
            extrude *= (cosHalfAngle != 0) ? (1.0 / cosHalfAngle) : 0;

            newRing.emplace_back(convertPoint<int16_t>(extrude * offset) + p);
        }
    }

    return newRings;
}

} // namespace

void RenderLineLayer::captureRenderedFeatures(const LineBucket& bucket,
                                              const RenderTile& tile,
                                              const LineBinders& binders,
                                              const style::LinePaintProperties::PossiblyEvaluated& evaluated,
                                              const TransformState& state,
                                              const TransformParameters& transformParams) {
    const auto& tileID = tile.getOverscaledTileID();

    const bool alphaIsConstant = evaluated.template get<LineOpacity>().isConstant();
    const auto& alphaBinder = binders.template get<LineOpacity>();
    if (alphaIsConstant && std::get<0>(alphaBinder->uniformValue(evaluated.get<LineOpacity>())) == 0) {
        return;
    }

    const bool colorIsConstant = evaluated.template get<LineColor>().isConstant();
    const auto& colorBinder = binders.template get<LineColor>();
    if (colorIsConstant && std::get<0>(colorBinder->uniformValue(evaluated.get<LineColor>())).a == 0) {
        return;
    }

    constexpr bool inViewportPixelUnits = false; // from RenderTile::translatedMatrix
    constexpr bool nearClipped = false;
    constexpr bool aligned = false;
    const auto translation = evaluated.get<LineTranslate>();
    const auto translationAnchor = evaluated.get<LineTranslateAnchor>();
    const std::optional<mln::Point<double>> origin = std::nullopt;
    const auto zoomFraction = state.getZoomFraction();
    std::optional<mat4> tileMatrix;

    const auto& features = bucket.getRetainedFeatures();
    stats.renderedFeatures.reserve(features.size());

    for (std::size_t i = 0; i < features.size(); ++i) {
        const auto& featureEntry = features[i];
        const auto& featureID = featureEntry.featureId;
        assert(!featureID.empty());

        // Consider only the value for the first vertex of this feature, for now.
        const auto vertexOffset = featureEntry.vertexOffset;

        if (!alphaIsConstant) {
            const auto& [vertex] = alphaBinder->getVertexValue(vertexOffset);
            if (mln::util::interpolate(vertex.a1[0], vertex.a1[1], zoomFraction) == 0) {
                continue;
            }
        }
        if (!colorIsConstant) {
            const auto& [vertex] = colorBinder->getVertexValue(vertexOffset);
            if (unpack_mix_alpha(vertex.a1, zoomFraction) == 0) {
                continue;
            }
        }

        // Compute the tile matrix once
        if (!tileMatrix.has_value()) {
            tileMatrix = LayerTweaker::getTileMatrix(tileID.toUnwrapped(),
                                                     state,
                                                     transformParams,
                                                     translation,
                                                     translationAnchor,
                                                     origin,
                                                     nearClipped,
                                                     inViewportPixelUnits,
                                                     aligned);
        }

        const auto getVertex = [&bucket, vertexOffset](std::size_t vi) {
            // Not considering line width yet
            const auto& vertex = bucket.vertices.at(vertexOffset + vi).a1;
            return vec3{vertex[0] / 2.0, vertex[1] / 2.0, 0};
        };

        if (const auto bound = computeFeatureNDCBound(featureEntry.vertexCount, *tileMatrix, getVertex)) {
            stats.addRenderedFeature(featureID, *bound, {tileID});
        }
    }
}

bool RenderLineLayer::queryIntersectsFeature(const GeometryCoordinates& queryGeometry,
                                             const GeometryTileFeature& feature,
                                             const float zoom,
                                             const TransformState& transformState,
                                             const float pixelsToTileUnits,
                                             const mat4&,
                                             const FeatureState& featureState) const {
    const auto& evaluated = static_cast<const LineLayerProperties&>(*evaluatedProperties).evaluated;
    // Translate query geometry
    auto translatedQueryGeometry = FeatureIndex::translateQueryGeometry(queryGeometry,
                                                                        evaluated.get<style::LineTranslate>(),
                                                                        evaluated.get<style::LineTranslateAnchor>(),
                                                                        static_cast<float>(transformState.getBearing()),
                                                                        pixelsToTileUnits);

    // Evaluate function
    auto offset = evaluated.get<style::LineOffset>().evaluate(
                      feature, zoom, featureState, style::LineOffset::defaultValue()) *
                  pixelsToTileUnits;
    // Test intersection
    const auto halfWidth = static_cast<float>(getLineWidth(feature, zoom, featureState) / 2.0 * pixelsToTileUnits);

    // Apply offset to geometry
    if (offset != 0.0f && !feature.getGeometries().empty()) {
        return util::polygonIntersectsBufferedMultiLine(
            translatedQueryGeometry.value_or(queryGeometry), offsetLine(feature.getGeometries(), offset), halfWidth);
    }

    return util::polygonIntersectsBufferedMultiLine(
        translatedQueryGeometry.value_or(queryGeometry), feature.getGeometries(), halfWidth);
}

void RenderLineLayer::updateColorRamp() {
    const style::ColorRampPropertyValue colorValue = unevaluated.get<LineGradient>().getValue();
    if (!colorRamp || !applyColorRamp(colorValue, *colorRamp, colorRampGlobalState.get())) {
        return;
    }

    if (colorRampTexture2D) {
        colorRampTexture2D.reset();

        // delete all gradient drawables
        if (glowMode && glowTargets[0]) {
            if (const auto& group = glowTargets[0]->getLayerGroup(0)) {
                stats.drawablesRemoved += group->getDrawableCount();
                group->clearDrawables();
            }
        } else if (layerGroup) {
            stats.drawablesRemoved += layerGroup->getDrawableCount();
            layerGroup->clearDrawables();
        }
    }
}

float RenderLineLayer::getLineWidth(const GeometryTileFeature& feature,
                                    const float zoom,
                                    const FeatureState& featureState) const {
    const auto& evaluated = static_cast<const LineLayerProperties&>(*evaluatedProperties).evaluated;
    float lineWidth = evaluated.get<style::LineWidth>().evaluate(
        feature, zoom, featureState, style::LineWidth::defaultValue());
    float gapWidth = evaluated.get<style::LineGapWidth>().evaluate(
        feature, zoom, featureState, style::LineGapWidth::defaultValue());
    if (gapWidth) {
        return gapWidth + 2 * lineWidth;
    } else {
        return lineWidth;
    }
}

namespace {

inline void setSegments(std::unique_ptr<gfx::DrawableBuilder>& builder, const LineBucket& bucket) {
    builder->setSegments(gfx::Triangles(), bucket.sharedTriangles, bucket.segments.data(), bucket.segments.size());
}

} // namespace

void RenderLineLayer::activateGlowTargets(bool activate, UniqueChangeRequestVec& changes) {
    // In order: the lines, then the horizontal blur that reads them, then the vertical blur.
    for (const auto& target : glowTargets) {
        if (!target) {
            continue;
        }
        if (activate) {
            changes.emplace_back(std::make_unique<AddRenderTargetRequest>(target));
        } else {
            changes.emplace_back(std::make_unique<RemoveRenderTargetRequest>(target));
        }
    }
}

void RenderLineLayer::markLayerRenderable(bool willRender, UniqueChangeRequestVec& changes) {
    RenderLayer::markLayerRenderable(willRender, changes);
    activateGlowTargets(willRender, changes);
}

void RenderLineLayer::layerRemoved(UniqueChangeRequestVec& changes) {
    RenderLayer::layerRemoved(changes);
    activateGlowTargets(false, changes);
}

std::size_t RenderLineLayer::removeTile(RenderPass renderPass, const OverscaledTileID& tileID) {
    if (glowMode) {
        if (glowTargets[0]) {
            auto* group = static_cast<TileLayerGroup*>(glowTargets[0]->getLayerGroup(0).get());
            const auto n = group->removeDrawables(renderPass, tileID).size();
            stats.drawablesRemoved += n;
            return n;
        }
        return 0;
    }
    return RenderLayer::removeTile(renderPass, tileID);
}

std::size_t RenderLineLayer::removeAllDrawables() {
    if (!glowMode) {
        return RenderLayer::removeAllDrawables();
    }
    // Glow mode: remove the line drawables only. The blur and composite quads are built once, with the
    // render targets, and must survive a frame with no tiles, or the glow never draws again.
    if (glowTargets[0]) {
        if (const auto& group = glowTargets[0]->getLayerGroup(0)) {
            const auto n = group->getDrawableCount();
            stats.drawablesRemoved += n;
            group->clearDrawables();
            return n;
        }
    }
    return 0;
}

void RenderLineLayer::teardownGlow(UniqueChangeRequestVec& changes) {
    removeAllDrawables();
    RenderLayer::removeAllDrawables();
    activateGlowTargets(false, changes);
    for (auto& target : glowTargets) {
        target.reset();
    }
    for (auto& tweaker : glowTweakers) {
        tweaker.reset();
    }
    setLayerGroup(nullptr, changes);
    layerTweaker.reset();
}

namespace {
gfx::UniqueDrawable makeGlowQuad(gfx::Context& context,
                                 const gfx::ShaderProgramBasePtr& shader,
                                 const gfx::Texture2DPtr& source,
                                 const gfx::ColorMode& colorMode,
                                 const SegmentVector& segments) {
    const auto vertices = std::make_shared<gfx::VertexVector<HeatmapTextureLayoutVertex>>(
        RenderStaticData::heatmapTextureVertices());

    auto attrs = context.createVertexAttributeArray();
    if (const auto& attr = attrs->set(idLineGlowPosVertexAttribute)) {
        attr->setSharedRawData(vertices,
                               offsetof(HeatmapTextureLayoutVertex, a1),
                               /*vertexOffset=*/0,
                               sizeof(HeatmapTextureLayoutVertex),
                               gfx::AttributeDataType::Short2);
    }
    auto builder = context.createDrawableBuilder("lineGlow");
    builder->setShader(shader);
    builder->setEnableDepth(false);
    builder->setColorMode(colorMode);
    builder->setCullFaceMode(gfx::CullFaceMode::disabled());
    builder->setRenderPass(RenderPass::Translucent);
    builder->setVertexAttributes(std::move(attrs));
    builder->setRawVertices({}, vertices->elements(), gfx::AttributeDataType::Short2);
    builder->setSegments(
        gfx::Triangles(), RenderStaticData::quadTriangleIndices().vector(), segments.data(), segments.size());
    builder->setTexture(source, idLineGlowImageTexture);
    builder->flush(context);
    auto drawables = builder->clearDrawables();
    assert(drawables.size() == 1);
    return std::move(drawables.front());
}
} // namespace

bool RenderLineLayer::updateGlow(gfx::ShaderRegistry& shaders,
                                 gfx::Context& context,
                                 const TransformState& state,
                                 UniqueChangeRequestVec& changes) {
    const auto& viewport = state.getSize();
    const Size size{std::max(1u, static_cast<uint32_t>(viewport.width * glowScale)),
                    std::max(1u, static_cast<uint32_t>(viewport.height * glowScale))};

    if (!glowShader) {
        glowShader = context.getGenericShader(shaders, "LineGlowBlurShader");
        if (!glowShader) {
            Log::Error(Event::Render, "line-glow-radius: no LineGlowBlurShader on this backend");
            return false;
        }
    }

    if (!glowTargets[0]) {
        if (glowSegments.empty()) {
            glowSegments = RenderStaticData::heatmapTextureSegments();
        }
        for (auto& target : glowTargets) {
            target = context.createRenderTarget(size, gfx::TextureChannelDataType::UnsignedByte);
            if (!target) {
                Log::Error(Event::Render, "line-glow-radius: no render target");
                teardownGlow(changes);
                return false;
            }
            target->setClearColor(Color{0.f, 0.f, 0.f, 0.f});
            target->getTexture()->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                                           .wrapU = gfx::TextureWrapType::Clamp,
                                                           .wrapV = gfx::TextureWrapType::Clamp});
        }

        // [0] the lines, drawn by the usual line drawables and tweaker
        auto tileGroup = context.createTileLayerGroup(0, /*initialCapacity=*/64, getID());
        glowTargets[0]->addLayerGroup(tileGroup, /*replace=*/true);

        // [1] horizontal blur of [0], [2] vertical blur of [1]
        for (int i = 1; i < 3; ++i) {
            auto group = context.createLayerGroup(0, /*initialCapacity=*/1, getID());
            glowTweakers[i] = std::make_shared<LineGlowBlurTweaker>(
                getID(),
                evaluatedProperties,
                glowTargets[i - 1]->getTexture(),
                i == 1 ? std::array<float, 2>{1.f, 0.f} : std::array<float, 2>{0.f, 1.f});
            group->addLayerTweaker(glowTweakers[i]);
            group->addDrawable(makeGlowQuad(
                context, glowShader, glowTargets[i - 1]->getTexture(), gfx::ColorMode::unblended(), glowSegments));
            glowTargets[i]->addLayerGroup(std::move(group), /*replace=*/true);
        }
        activateGlowTargets(isRenderable, changes);

        // The layer's own group draws [2] onto the map.
        auto composite = context.createLayerGroup(layerIndex, /*initialCapacity=*/1, getID());
        glowTweakers[0] = std::make_shared<LineGlowBlurTweaker>(
            getID(), evaluatedProperties, glowTargets[2]->getTexture(), std::array<float, 2>{0.f, 0.f});
        composite->addLayerTweaker(glowTweakers[0]);
        composite->addDrawable(makeGlowQuad(
            context, glowShader, glowTargets[2]->getTexture(), gfx::ColorMode::alphaBlended(), glowSegments));
        setLayerGroup(std::move(composite), changes);
    }

    for (const auto& target : glowTargets) {
        if (target->getTexture()->getSize() != size) {
            target->getTexture()->setSize(size);
        }
    }
    return true;
}

void RenderLineLayer::update(gfx::ShaderRegistry& shaders,
                             gfx::Context& context,
                             const TransformState& state,
                             const std::shared_ptr<UpdateParameters>& parameters,
                             [[maybe_unused]] const PaintParameters& paintParameters,
                             const RenderTree& renderTree,
                             UniqueChangeRequestVec& changes) {
    stats.renderedFeatures.clear();

    if (!renderTiles || renderTiles->empty()) {
        removeAllDrawables();
        return;
    }

    const bool wantGlow =
        static_cast<const LineLayerProperties&>(*evaluatedProperties).evaluated.get<LineGlowRadius>() > 0.f;
    if (wantGlow != glowMode) {
        teardownGlow(changes);
        glowMode = wantGlow;
    }

    TileLayerGroup* tileLayerGroup = nullptr;
    if (glowMode) {
        if (!updateGlow(shaders, context, state, changes)) {
            glowMode = false;
            return;
        }
        tileLayerGroup = static_cast<TileLayerGroup*>(glowTargets[0]->getLayerGroup(0).get());
    } else {
        // Set up a layer group
        if (!layerGroup) {
            if (auto layerGroup_ = context.createTileLayerGroup(layerIndex, /*initialCapacity=*/64, getID())) {
                setLayerGroup(std::move(layerGroup_), changes);
            } else {
                return;
            }
        }
        tileLayerGroup = static_cast<TileLayerGroup*>(layerGroup.get());
    }

    if (!layerTweaker) {
        auto tweaker = std::make_shared<LineLayerTweaker>(getID(), evaluatedProperties);
#if MLN_RENDER_BACKEND_METAL
        tweaker->updateGPUExpressions(unevaluated, parameters->timePoint);
#endif // MLN_RENDER_BACKEND_METAL

        layerTweaker = std::move(tweaker);
        tileLayerGroup->addLayerTweaker(layerTweaker);
    }

    const RenderPass renderPass = static_cast<RenderPass>(evaluatedProperties->renderPasses &
                                                          ~mln::underlying_type(RenderPass::Opaque));

    stats.drawablesRemoved += tileLayerGroup->removeDrawablesIf([&](gfx::Drawable& drawable) {
        // If the render pass has changed or the tile has  dropped out of the cover set, remove it.
        const auto& tileID = drawable.getTileID();
        return drawable.getRenderPass() != passes || (tileID && !hasRenderTile(*tileID));
    });

    auto createLineBuilder = [&](const std::string& name,
                                 gfx::ShaderPtr shader) -> std::unique_ptr<gfx::DrawableBuilder> {
        std::unique_ptr<gfx::DrawableBuilder> builder = context.createDrawableBuilder(name);
        builder->setShader(std::static_pointer_cast<gfx::ShaderProgramBase>(shader));
        builder->setRenderPass(renderPass);
        builder->setSubLayerIndex(0);
        builder->setDepthType(gfx::DepthMaskType::ReadOnly);
        builder->setColorMode(renderPass == RenderPass::Translucent ? gfx::ColorMode::alphaBlended()
                                                                    : gfx::ColorMode::unblended());
        builder->setCullFaceMode(gfx::CullFaceMode::disabled());
        builder->setEnableStencil(!glowMode);
        if (glowMode) {
            // The glow image has no depth or stencil buffer.
            builder->setEnableDepth(false);
        }

        return builder;
    };

    auto addAttributes =
        [&](gfx::DrawableBuilder& builder, const LineBucket& bucket, gfx::VertexAttributeArrayPtr&& vertexAttrs) {
            const auto vertexCount = bucket.vertices.elements();
            builder.setRawVertices({}, vertexCount, gfx::AttributeDataType::Short4);

            if (const auto& attr = vertexAttrs->set(idLinePosNormalVertexAttribute)) {
                attr->setSharedRawData(bucket.sharedVertices,
                                       offsetof(LineLayoutVertex, a1),
                                       /*vertexOffset=*/0,
                                       sizeof(LineLayoutVertex),
                                       gfx::AttributeDataType::Short2);
            }

            if (const auto& attr = vertexAttrs->set(idLineDataVertexAttribute)) {
                attr->setSharedRawData(bucket.sharedVertices,
                                       offsetof(LineLayoutVertex, a2),
                                       /*vertexOffset=*/0,
                                       sizeof(LineLayoutVertex),
                                       gfx::AttributeDataType::UByte4);
            }

            builder.setVertexAttributes(std::move(vertexAttrs));
        };

    // The glow image has no stencil buffer, and a clip mask drawn there would be counted as drawn for the map.
    tileLayerGroup->setStencilTiles(glowMode ? RenderTiles{} : renderTiles);

    // The ramp texture is shared by every tile of the layer.
    const auto ensureColorRampTexture = [&] {
        if (!colorRampTexture2D && colorRamp->valid()) {
            colorRampTexture2D = context.createTexture2D();
            colorRampTexture2D->setImage(colorRamp);
            colorRampTexture2D->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                                         .wrapU = gfx::TextureWrapType::Clamp,
                                                         .wrapV = gfx::TextureWrapType::Clamp});
        }
    };

    StringIDSetsPair propertiesAsUniforms;
    for (const RenderTile& tile : *renderTiles) {
        const auto& tileID = tile.getOverscaledTileID();

        const LayerRenderData* renderData = getRenderDataForPass(tile, renderPass);
        if (!renderData) {
            removeTile(renderPass, tileID);
            continue;
        }

        auto& bucket = static_cast<LineBucket&>(*renderData->bucket);
        if (!bucket.sharedTriangles->elements()) {
            removeTile(renderPass, tileID);
            continue;
        }

        auto& paintPropertyBinders = bucket.paintPropertyBinders.at(getID());
        const auto& evaluated = getEvaluated<LineLayerProperties>(renderData->layerProperties);

        if (parameters->captureRenderedFeatures) {
            const auto& params = renderTree.getParameters().transformParams;
            captureRenderedFeatures(bucket, tile, paintPropertyBinders, evaluated, state, params);
        }

        const auto prevBucketID = getRenderTileBucketID(tileID);
        if (prevBucketID != util::SimpleIdentity::Empty && prevBucketID != bucket.getID()) {
            // This tile was previously set up from a different bucket, drop and re-create any drawables for it.
            removeTile(renderPass, tileID);
        }
        setRenderTileBucketID(tileID, bucket.getID());

        auto updateExisting = [&](gfx::Drawable& drawable) {
            // A drawable produced on a previous style/bucket should not be updated.
            return drawable.getLayerTweaker() == layerTweaker;
        };
        if (updateTile(tileLayerGroup, renderPass, tileID, std::move(updateExisting))) {
            continue;
        }

        const auto addDrawable = [&](std::unique_ptr<gfx::Drawable>&& drawable, LineLayerTweaker::LineType type) {
            drawable->setTileID(tileID);
            drawable->setType(mln::underlying_type(type));
            drawable->setLayerTweaker(layerTweaker);
            drawable->setRenderTile(renderTilesOwner, &tile);
            drawable->setBinders(renderData->bucket, &paintPropertyBinders);

            const bool roundCap = bucket.layout.get<LineCap>() == LineCapType::Round;
            const auto capType = roundCap ? LinePatternCap::Round : LinePatternCap::Square;
            drawable->setData(std::make_unique<gfx::LineDrawableData>(capType));

            tileLayerGroup->addDrawable(renderPass, tileID, std::move(drawable));
            ++stats.drawablesAdded;
        };

        propertiesAsUniforms.first.clear();
        propertiesAsUniforms.second.clear();

        auto vertexAttrs = context.createVertexAttributeArray();
        vertexAttrs->readDataDrivenPaintProperties<LineColor,
                                                   LineBlur,
                                                   LineOpacity,
                                                   LineGapWidth,
                                                   LineOffset,
                                                   LineWidth,
                                                   LineFloorWidth,
                                                   LinePattern>(
            paintPropertyBinders, evaluated, propertiesAsUniforms, idLineColorVertexAttribute);

        if (!evaluated.get<LineDasharray>().from.empty() && !unevaluated.get<LineGradient>().getValue().isUndefined()) {
            // dash array over a gradient
            if (!lineGradientSDFShaderGroup) {
                lineGradientSDFShaderGroup = shaders.getShaderGroup("LineGradientSDFShader");
                if (!lineGradientSDFShaderGroup) {
                    continue;
                }
            }

            auto shader = lineGradientSDFShaderGroup->getOrCreateShader(
                context, propertiesAsUniforms, posNormalAttribName);
            if (!shader) {
                continue;
            }

            auto builder = createLineBuilder("lineGradientSDF", std::move(shader));

            // vertices and attributes
            addAttributes(*builder, bucket, std::move(vertexAttrs));

            ensureColorRampTexture();
            if (colorRampTexture2D) {
                builder->setTexture(colorRampTexture2D, idLineImageTexture);

                setSegments(builder, bucket);

                builder->flush(context);
                for (auto& drawable : builder->clearDrawables()) {
                    addDrawable(std::move(drawable), LineLayerTweaker::LineType::GradientSDF);
                }
            }
        } else if (!evaluated.get<LineDasharray>().from.empty()) {
            // dash array line (SDF)
            if (!lineSDFShaderGroup) {
                lineSDFShaderGroup = shaders.getShaderGroup("LineSDFShader");
                if (!lineSDFShaderGroup) {
                    continue;
                }
            }

            auto shader = lineSDFShaderGroup->getOrCreateShader(context, propertiesAsUniforms, posNormalAttribName);
            if (!shader) {
                continue;
            }

            auto builder = createLineBuilder("lineSDF", std::move(shader));

            // vertices, attributes and segments
            addAttributes(*builder, bucket, std::move(vertexAttrs));
            setSegments(builder, bucket);

            builder->flush(context);
            for (auto& drawable : builder->clearDrawables()) {
                addDrawable(std::move(drawable), LineLayerTweaker::LineType::SDF);
            }
        } else if (!unevaluated.get<LinePattern>().isUndefined()) {
            // pattern line
            if (!linePatternShaderGroup) {
                linePatternShaderGroup = shaders.getShaderGroup("LinePatternShader");
                if (!linePatternShaderGroup) {
                    continue;
                }
            }

            auto shader = linePatternShaderGroup->getOrCreateShader(context, propertiesAsUniforms, posNormalAttribName);
            if (!shader) {
                continue;
            }

            auto builder = createLineBuilder("linePattern", std::move(shader));

            // vertices and attributes
            addAttributes(*builder, bucket, std::move(vertexAttrs));

            // texture
            if (const auto& atlases = tile.getAtlasTextures(); atlases && atlases->icon) {
                auto iconTweaker = std::make_shared<gfx::DrawableAtlasesTweaker>(
                    atlases,
                    std::nullopt,
                    idLineImageTexture,
                    /*isText*/ false,
                    /*sdfIcons*/ true, // to force linear filter
                    /*rotationAlignment_*/ AlignmentType::Auto,
                    /*iconScaled*/ false,
                    /*textSizeIsZoomConstant_*/ false);

                builder->addTweaker(std::move(iconTweaker));

                setSegments(builder, bucket);

                builder->flush(context);
                for (auto& drawable : builder->clearDrawables()) {
                    addDrawable(std::move(drawable), LineLayerTweaker::LineType::Pattern);
                }
            }
        } else if (!unevaluated.get<LineGradient>().getValue().isUndefined()) {
            // gradient line
            if (!lineGradientShaderGroup) {
                lineGradientShaderGroup = shaders.getShaderGroup("LineGradientShader");
                if (!lineGradientShaderGroup) {
                    continue;
                }
            }

            auto shader = lineGradientShaderGroup->getOrCreateShader(
                context, propertiesAsUniforms, posNormalAttribName);
            if (!shader) {
                continue;
            }

            auto builder = createLineBuilder("lineGradient", std::move(shader));

            // vertices and attributes
            addAttributes(*builder, bucket, std::move(vertexAttrs));

            ensureColorRampTexture();
            if (colorRampTexture2D) {
                builder->setTexture(colorRampTexture2D, idLineImageTexture);

                setSegments(builder, bucket);

                builder->flush(context);
                for (auto& drawable : builder->clearDrawables()) {
                    addDrawable(std::move(drawable), LineLayerTweaker::LineType::Gradient);
                }
            }
        } else {
            // simple line
            if (!lineShaderGroup) {
                lineShaderGroup = shaders.getShaderGroup("LineShader");
                if (!lineShaderGroup) {
                    continue;
                }
            }

            auto shader = lineShaderGroup->getOrCreateShader(context, propertiesAsUniforms, posNormalAttribName);
            if (!shader) {
                continue;
            }

            auto builder = createLineBuilder("line", std::move(shader));

            // vertices, attributes and segments
            addAttributes(*builder, bucket, std::move(vertexAttrs));
            setSegments(builder, bucket);

            builder->flush(context);
            for (auto& drawable : builder->clearDrawables()) {
                addDrawable(std::move(drawable), LineLayerTweaker::LineType::Simple);
            }
        }
    }
}

} // namespace mln
