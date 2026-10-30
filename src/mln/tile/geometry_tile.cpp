#include <mln/tile/geometry_tile.hpp>

#include <mln/actor/scheduler.hpp>
#include <mln/geometry/feature_index.hpp>
#include <mln/style/layers/custom_layer.hpp>
#include <mln/renderer/layers/render_custom_layer.hpp>
#include <mln/map/transform_state.hpp>
#include <mln/renderer/buckets/symbol_bucket.hpp>
#include <mln/renderer/layers/render_background_layer.hpp>
#include <mln/renderer/layers/render_symbol_layer.hpp>
#include <mln/renderer/query.hpp>
#include <mln/renderer/render_source.hpp>
#include <mln/renderer/tile_parameters.hpp>
#include <mln/renderer/tile_render_data.hpp>
#include <mln/style/layer_impl.hpp>
#include <mln/style/layers/background_layer.hpp>
#include <mln/tile/geometry_tile_data.hpp>
#include <mln/tile/geometry_tile_worker.hpp>
#include <mln/tile/tile_observer.hpp>
#include <mln/util/error_sink.hpp>
#include <mln/util/instrumentation.hpp>
#include <mln/util/label_steps.hpp>
#include <mln/util/logging.hpp>
#include <mln/util/thread_pool.hpp>
#include <mln/gfx/upload_pass.hpp>

#include <utility>

namespace mln {

LayerRenderData* GeometryTile::LayoutResult::getLayerRenderData(const style::Layer::Impl& layerImpl) {
    MLN_TRACE_FUNC();
    MLN_ZONE_STR(layerImpl.id);

    auto it = layerRenderData.find(layerImpl.id);
    if (it == layerRenderData.end()) {
        return nullptr;
    }
    LayerRenderData& result = it->second;
    if (result.layerProperties->baseImpl->getTypeInfo() != layerImpl.getTypeInfo()) {
        // Layer data might be outdated, see issue #12432.
        return nullptr;
    }
    return &result;
}

class ImagePatch {
public:
    ImagePatch(Immutable<style::Image::Impl> image_, const Rect<uint16_t>& paddedRect_)
        : image(std::move(image_)),
          paddedRect(paddedRect_) {}
    Immutable<style::Image::Impl> image;
    Rect<uint16_t> paddedRect;
};

void populateImagePatches(ImagePositions& imagePositions,
                          const ImageManager& imageManager,
                          std::vector<ImagePatch>& /*out*/ patches) {
    if (imagePositions.empty()) {
        imagePositions.reserve(imageManager.updatedImageVersions.size());
    }
    for (auto& updatedImageVersion : imageManager.updatedImageVersions) {
        const std::string& name = updatedImageVersion.first;
        const uint32_t version = updatedImageVersion.second;
        const auto it = imagePositions.find(updatedImageVersion.first);
        if (it != imagePositions.end()) {
            auto& position = it->second;
            if (position.version == version) continue;

            const auto updatedImage = imageManager.getSharedImage(name);
            if (updatedImage == nullptr) continue;

            patches.emplace_back(*updatedImage, position.paddedRect);
            position.version = version;
        }
    }
}

class GeometryTileRenderData final : public TileRenderData {
public:
    GeometryTileRenderData(std::shared_ptr<GeometryTile::LayoutResult> layoutResult_,
                           std::shared_ptr<TileAtlasTextures> atlasTextures_)
        : TileRenderData(std::move(atlasTextures_)),
          layoutResult(std::move(layoutResult_)) {}

private:
    // TileRenderData overrides.
    std::optional<ImagePosition> getPattern(const std::string&) const override;
    const LayerRenderData* getLayerRenderData(const style::Layer::Impl&) const override;
    Bucket* getBucket(const style::Layer::Impl&) const override;
    void upload(gfx::UploadPass&) override;
    void prepare(const SourcePrepareParameters&) override;

    std::shared_ptr<GeometryTile::LayoutResult> layoutResult;
    std::vector<ImagePatch> imagePatches;
};

using namespace style;

std::optional<ImagePosition> GeometryTileRenderData::getPattern(const std::string& pattern) const {
    MLN_TRACE_FUNC();

    if (layoutResult) {
        const auto& patternPositions = layoutResult->imageAtlas.patternPositions;
        auto it = patternPositions.find(pattern);
        if (it != patternPositions.end()) {
            return it->second;
        }
    }
    return std::nullopt;
}

void GeometryTileRenderData::upload(gfx::UploadPass& uploadPass) {
    MLN_TRACE_FUNC();

    if (!layoutResult) return;

    auto uploadFn = [&](Bucket& bucket) {
        if (bucket.needsUpload()) {
            bucket.upload(uploadPass);
        }
    };

    for (auto& entry : layoutResult->layerRenderData) {
        uploadFn(*entry.second.bucket);
    }

    assert(atlasTextures);

    if (const auto& glyphDynamicTexture = layoutResult->glyphAtlas.dynamicTexture) {
        glyphDynamicTexture->uploadDeferredImages(uploadPass);
        atlasTextures->glyph = glyphDynamicTexture->getTexture();
    }
    if (const auto& imageDynamicTexture = layoutResult->imageAtlas.dynamicTexture) {
        imageDynamicTexture->uploadDeferredImages(uploadPass);
        atlasTextures->icon = imageDynamicTexture->getTexture();
    }

    if (!imagePatches.empty()) {
        for (const auto& imagePatch : imagePatches) { // patch updated images.
            atlasTextures->icon->uploadSubRegion(imagePatch.image->image,
                                                 imagePatch.paddedRect.x + ImagePosition::padding,
                                                 imagePatch.paddedRect.y + ImagePosition::padding);
        }
        imagePatches.clear();
    }
}

void GeometryTileRenderData::prepare(const SourcePrepareParameters& parameters) {
    MLN_TRACE_FUNC();

    if (!layoutResult) return;
    imagePatches.clear();
    populateImagePatches(layoutResult->imageAtlas.iconPositions, parameters.imageManager, imagePatches);
    populateImagePatches(layoutResult->imageAtlas.patternPositions, parameters.imageManager, imagePatches);
}

Bucket* GeometryTileRenderData::getBucket(const Layer::Impl& layer) const {
    const LayerRenderData* data = getLayerRenderData(layer);
    return data ? data->bucket.get() : nullptr;
}

const LayerRenderData* GeometryTileRenderData::getLayerRenderData(const style::Layer::Impl& layerImpl) const {
    return layoutResult ? layoutResult->getLayerRenderData(layerImpl) : nullptr;
}

GeometryTile::LayoutResult::~LayoutResult() {
    if (dynamicTextureAtlas) {
        dynamicTextureAtlas->removeTextures(glyphAtlas.textureHandles, glyphAtlas.dynamicTexture);
        dynamicTextureAtlas->removeTextures(imageAtlas.textureHandles, imageAtlas.dynamicTexture);
    }
}

/*
   Correlation between GeometryTile and GeometryTileWorker is safeguarded by two
   correlation schemes:

   GeometryTile's 'correlationID' is used for ensuring the tile will be flagged
   as non-pending only when the placement coming from the last operation (as in
   'setData', 'setLayers',  'setShowCollisionBoxes') occurs. This is important
   for still mode rendering as we want to render only when all layout and
   placement operations are completed.

   GeometryTileWorker's 'imageCorrelationID' is used for checking whether an
   image request reply coming from `GeometryTile` is valid. Previous image
   request replies are ignored as they result in incomplete placement attempts
   that could flag the tile as non-pending too early.
 */

GeometryTile::GeometryTile(const OverscaledTileID& id_,
                           std::string sourceID_,
                           const TileParameters& parameters,
                           TileObserver* observer_)
    : Tile(Kind::Geometry, id_, std::move(sourceID_), observer_),
      ImageRequestor(parameters.imageManager),
      threadPool(parameters.threadPool),
      mailbox(std::make_shared<Mailbox>(*Scheduler::GetCurrent())),
      worker(parameters.isUpdateSynchronous,
             // Scheduler reference for the Actor retainer: the worker's own
             // runs. A label lane source's worker runs on the lane, with the
             // renderer's tag; its render-thread jobs (the next argument) stay
             // on the renderer's scheduler, the only one the graphics context
             // drains.
             parameters.labelLane ? TaggedScheduler(parameters.labelLane, parameters.threadPool.tag)
                                  : parameters.threadPool,
             OptionalActorRef<GeometryTile>(parameters.isUpdateSynchronous, *this, mailbox),
             parameters.threadPool,
             id_,
             sourceID,
             obsolete,
             parameters.mode,
             parameters.pixelRatio,
             parameters.debugOptions & MapDebugOptions::Collision,
             parameters.dynamicTextureAtlas,
             parameters.glyphManager->getFontFaces(),
             parameters.captureRenderedFeatures,
             observer),
      laysOutInline(parameters.labelLane && !parameters.isUpdateSynchronous),
      pixelRatio(parameters.pixelRatio),
      dynamicTextureAtlas(parameters.dynamicTextureAtlas),
      fontFaces(parameters.glyphManager->getFontFaces()),
      captureRenderedFeatures(parameters.captureRenderedFeatures),
      fileSource(parameters.fileSource),
      glyphManager(parameters.glyphManager),
      imageManager(parameters.imageManager),
      mode(parameters.mode),
      showCollisionBoxes(parameters.debugOptions & MapDebugOptions::Collision) {}

GeometryTile::~GeometryTile() {
    MLN_TRACE_FUNC();

    markObsolete();

    glyphManager->removeRequestor(*this);
    imageManager->removeRequestor(*this);
    inlineWorker.reset();
    if (inlineRequestor) {
        glyphManager->removeRequestor(*inlineRequestor);
        imageManager->removeRequestor(*inlineRequestor);
    }

    if (pending) {
        // This tile never finished loading or was abandoned, emit a cancellation event
        observer->onTileAction(id, sourceID, TileOperation::Cancelled);
    }

    if (layoutResult) {
        threadPool.runOnRenderThread(
            [layoutResult_{std::move(layoutResult)}, atlasTextures_{std::move(atlasTextures)}]() {
                layoutResult_->dynamicTextureAtlas->removeUnusedDynamicTextures();
            });
    }
    // The layouts kept aside above the landing limit hold atlas entries too.
    for (auto& kept : keptResults) {
        if (kept.second) {
            threadPool.runOnRenderThread([result_{std::move(kept.second)}]() {
                result_->dynamicTextureAtlas->removeUnusedDynamicTextures();
            });
        }
    }
}

void GeometryTile::cancel() {
    markObsolete();
}

void GeometryTile::markObsolete() {
    obsolete = true;
    mailbox->abandon();
}

void GeometryTile::setError(std::exception_ptr err) {
    loaded = true;
    observer->onTileError(*this, std::move(err));
}

void GeometryTile::setData(std::unique_ptr<const GeometryTileData> data_) {
    MLN_TRACE_FUNC();
    const util::LabelSteps::Scope step(sourceID, "hop-data", true);

    if (obsolete) {
        return;
    }

    if (!pending) {
        observer->onTileAction(id, sourceID, TileOperation::StartParse);
    }

    // Mark the tile as pending again if it was complete before to prevent
    // signaling a complete state despite pending parse operations.
    pending = true;

    if (laysOutInline) {
        // A GeoJSON tile's data shares its features: the copy is cheap.
        lastData = data_ ? data_->clone() : nullptr;
    }

    nextCorrelationID();
    worker.self().invoke(
        &GeometryTileWorker::setData, std::move(data_), imageManager->getAvailableImages(), correlationID);
}

void GeometryTile::reset() {
    MLN_TRACE_FUNC();

    // If there is pending work, indicate that work has been cancelled.
    // Clear the pending status.
    if (pending) {
        observer->onTileAction(id, sourceID, TileOperation::Cancelled);
        pending = false;
    }

    // Reset the tile to an unloaded state to avoid signaling completion
    // after clearing the tile's pending status.
    loaded = false;

    // Reset the worker to the `NeedsParse` state.
    nextCorrelationID();
    worker.self().invoke(&GeometryTileWorker::reset, correlationID);
}

std::unique_ptr<TileRenderData> GeometryTile::createRenderData() {
    MLN_TRACE_FUNC();

    return std::make_unique<GeometryTileRenderData>(layoutResult, atlasTextures);
}

void GeometryTile::setLayers(const std::vector<Immutable<LayerProperties>>& layers,
                             std::shared_ptr<const GlobalStateMap> globalState) {
    MLN_TRACE_FUNC();

    // Mark the tile as pending again if it was complete before to prevent
    // signaling a complete state despite pending parse operations.
    if (!pending) {
        pending = true;
        observer->onTileAction(id, sourceID, TileOperation::StartParse);
    }

    nextCorrelationID();
    worker.self().invoke(&GeometryTileWorker::setLayers,
                         layersForZoom(layers),
                         std::move(globalState),
                         imageManager->getAvailableImages(),
                         correlationID);
}

std::vector<Immutable<LayerProperties>> GeometryTile::layersForZoom(
    const std::vector<Immutable<LayerProperties>>& layers) const {
    std::vector<Immutable<LayerProperties>> impls;
    impls.reserve(layers.size());

    for (const auto& layer : layers) {
        MLN_TRACE_ZONE(layer);
        MLN_ZONE_STR(layer->baseImpl->id);
        // Skip irrelevant layers.
        const auto& layerImpl = *layer->baseImpl;
        assert(layerImpl.getTypeInfo()->source != LayerTypeInfo::Source::NotRequired);
        assert(layerImpl.source == sourceID);
        assert(layerImpl.visibility != VisibilityType::None);
        if (id.overscaledZ < std::floor(layerImpl.minZoom) || id.overscaledZ >= std::ceil(layerImpl.maxZoom)) {
            continue;
        }

        impls.push_back(layer);
    }
    return impls;
}

void GeometryTile::setLayersInline(const std::vector<Immutable<LayerProperties>>& layers,
                                   std::shared_ptr<const GlobalStateMap> globalState) {
    MLN_TRACE_FUNC();

    if (!laysOutInline || !lastData || obsolete) {
        // Nothing to lay out here yet (no data has come): the worker does it.
        setLayers(layers, globalState);
        return;
    }

    if (!pending) {
        pending = true;
        observer->onTileAction(id, sourceID, TileOperation::StartParse);
    }
    nextCorrelationID();
    const auto images = imageManager->getAvailableImages();
    // The lane's worker takes the layers for the bodies to come, and lays
    // nothing out with them: this tile does, below. Its layouts for the old
    // layers, still on the way, are older than this one and are dropped
    // (`takenCorrelationID`).
    worker.self().invoke(&GeometryTileWorker::setLayersQuietly, layersForZoom(layers), globalState, images);

    if (!inlineRequestor) {
        inlineRequestor = std::make_unique<InlineRequestor>(*this, imageManager);
    }
    // A worker of its own, synchronous: the lane's worker is the Actor's, and
    // may be parsing a body on the lane now.
    inlineWorker = std::make_unique<OptionalActor<GeometryTileWorker>>(
        true,
        threadPool,
        OptionalActorRef<GeometryTile>(true, *this, mailbox),
        threadPool,
        id,
        sourceID,
        obsolete,
        mode,
        pixelRatio,
        showCollisionBoxes,
        dynamicTextureAtlas,
        fontFaces,
        captureRenderedFeatures,
        observer);
    const util::LabelSteps::Scope step(sourceID, "hop-inline-layout", true);
    const bool wasInside = std::exchange(inInlineWorker, true);
    inlineWorker->self().invoke(
        &GeometryTileWorker::setLayers, layersForZoom(layers), std::move(globalState), images, correlationID);
    inlineWorker->self().invoke(&GeometryTileWorker::setData, lastData->clone(), images, correlationID);
    inInlineWorker = wasInside;
}

void GeometryTile::InlineRequestor::onGlyphsAvailable(GlyphMap glyphMap, HBShapeRequests requests) {
    if (!tile.inlineWorker) return;
    HBShapeResults results = tile.shape(glyphMap, requests);
    const bool wasInside = std::exchange(tile.inInlineWorker, true);
    tile.inlineWorker->self().invoke(&GeometryTileWorker::onGlyphsAvailable, std::move(glyphMap), std::move(results));
    tile.inInlineWorker = wasInside;
}

void GeometryTile::InlineRequestor::onImagesAvailable(ImageMap images,
                                                      ImageMap patterns,
                                                      ImageVersionMap versionMap,
                                                      uint64_t imageCorrelationID) {
    if (!tile.inlineWorker) return;
    const bool wasInside = std::exchange(tile.inInlineWorker, true);
    tile.inlineWorker->self().invoke(&GeometryTileWorker::onImagesAvailable,
                                     std::move(images),
                                     std::move(patterns),
                                     std::move(versionMap),
                                     imageCorrelationID);
    tile.inInlineWorker = wasInside;
}

void GeometryTile::setShowCollisionBoxes(const bool showCollisionBoxes_) {
    MLN_TRACE_FUNC();

    if (showCollisionBoxes != showCollisionBoxes_) {
        showCollisionBoxes = showCollisionBoxes_;
        nextCorrelationID();
        worker.self().invoke(&GeometryTileWorker::setShowCollisionBoxes, showCollisionBoxes, correlationID);
    }
}

void GeometryTile::onLayout(std::shared_ptr<LayoutResult>&& result, const uint64_t resultCorrelationID) {
    MLN_TRACE_FUNC();
    const util::LabelSteps::Scope step(sourceID, "hop-layout", true);

    // Results come in order from one worker. With an inline layout there are
    // two: a lane result for older layers can come after the inline one.
    if (resultCorrelationID < takenCorrelationID) {
        return;
    }
    takenCorrelationID = resultCorrelationID;

    loaded = true;
    if (resultCorrelationID == correlationID) {
        pending = false;
        observer->onTileAction(id, sourceID, TileOperation::EndParse);
    }

    const auto found = generationOfCorrelation.find(resultCorrelationID);
    const uint64_t generation = found != generationOfCorrelation.end() ? found->second : bodyGeneration;
    generationOfCorrelation.erase(generationOfCorrelation.begin(), generationOfCorrelation.upper_bound(resultCorrelationID));
    if (landingLimit && generation > *landingLimit) {
        // Laid out, not landed: the tile keeps drawing its landed layout
        // until the limit reaches this body. The change is still reported, so
        // the source learns it is laid out.
        if (auto& slot = keptResults[generation]) {
            freeLayout(std::move(slot));
        }
        keptResults[generation] = std::move(result);
        // Bounded: a stream of bodies during one hold keeps the newest few.
        while (keptResults.size() > maxKeptLayouts) {
            freeLayout(std::move(keptResults.begin()->second));
            keptResults.erase(keptResults.begin());
        }
        observer->onTileChanged(*this);
        // A layout kept aside is still a layout done: a body waiting for it
        // (`GeoJSONTile::updateDataAfterLanding`) goes on now, or a limit
        // below the landed body would keep the newer one from ever laying out.
        onLayoutTaken();
        return;
    }
    landedGeneration = generation;
    renderable = true;

    const ErrorScope errorScope{observer};

    layoutResult = std::move(result);
    if (!atlasTextures) {
        atlasTextures = std::make_shared<TileAtlasTextures>();
    }

    if (layoutResult) {
        for (const auto& data : layoutResult->layerRenderData) {
            if (data.second.bucket) {
                data.second.bucket->check(SYM_GUARD_LOC);
            }
        }
    }

    observer->onTileChanged(*this);

    if (layoutResult) {
        for (const auto& data : layoutResult->layerRenderData) {
            if (data.second.bucket) {
                data.second.bucket->check(SYM_GUARD_LOC);
            }
        }
    }
    onLayoutTaken();
}

uint64_t GeometryTile::nextCorrelationID() {
    ++correlationID;
    generationOfCorrelation[correlationID] = bodyGeneration;
    return correlationID;
}

void GeometryTile::land(std::shared_ptr<LayoutResult>&& result, uint64_t generation) {
    landedGeneration = generation;
    renderable = true;
    layoutResult = std::move(result);
    if (!atlasTextures) {
        atlasTextures = std::make_shared<TileAtlasTextures>();
    }
    observer->onTileChanged(*this);
    onLayoutTaken();
}

void GeometryTile::setLandingLimit(std::optional<uint64_t> limit) {
    if (limit == landingLimit) return;
    landingLimit = limit;
    // The newest kept layout the limit now lets land, if newer than the drawn one.
    auto end = limit ? keptResults.upper_bound(*limit) : keptResults.end();
    if (end == keptResults.begin()) return;
    auto newest = std::prev(end);
    std::shared_ptr<LayoutResult> result = std::move(newest->second);
    const uint64_t generation = newest->first;
    // Every kept layout at or below the limit goes: the newest lands, the
    // others are freed.
    for (auto it = keptResults.begin(); it != newest; ++it) {
        freeLayout(std::move(it->second));
    }
    keptResults.erase(keptResults.begin(), end);
    if (generation >= landedGeneration) {
        land(std::move(result), generation);
    } else {
        freeLayout(std::move(result));
    }
}

void GeometryTile::freeLayout(std::shared_ptr<LayoutResult>&& result) {
    if (!result) return;
    threadPool.runOnRenderThread(
        [result_{std::move(result)}]() { result_->dynamicTextureAtlas->removeUnusedDynamicTextures(); });
}

void GeometryTile::setObserver(TileObserver* observer_) {
    Tile::setObserver(observer_);

    worker.self().invoke(&GeometryTileWorker::setObserver, observer);
}

void GeometryTile::onError(std::exception_ptr err, const uint64_t resultCorrelationID) {
    loaded = true;
    if (resultCorrelationID == correlationID) {
        pending = false;
        observer->onTileAction(id, sourceID, TileOperation::Error);
    }
    observer->onTileError(*this, std::move(err));
}

void GeometryTile::onGlyphsAvailable(GlyphMap glyphMap, HBShapeRequests requests) {
    MLN_TRACE_FUNC();

    HBShapeResults results = shape(glyphMap, requests);
    worker.self().invoke(&GeometryTileWorker::onGlyphsAvailable, std::move(glyphMap), std::move(results));
}

HBShapeResults GeometryTile::shape(GlyphMap& glyphMap, [[maybe_unused]] const HBShapeRequests& requests) {
    HBShapeResults results;
#ifdef MLN_TEXT_SHAPING_HARFBUZZ
    for (auto& fontStackIT : requests) {
        auto fontStack = fontStackIT.first;
        auto& fontTypes = fontStackIT.second;
        for (auto& typesIT : fontTypes) {
            auto type = typesIT.first;
            auto& strs = typesIT.second;

            for (auto& str : strs) {
                std::vector<GlyphID> shapedGlyphIDs;
                std::shared_ptr<std::vector<HBShapeAdjust>> shapedAdjusts =
                    std::make_shared<std::vector<HBShapeAdjust>>();
                glyphManager->hbShaping(str, fontStack, type, shapedGlyphIDs, *shapedAdjusts);
                std::u16string shapedstr;

                shapedstr.reserve(shapedGlyphIDs.size());
                for (auto& glyphID : shapedGlyphIDs) {
                    shapedstr += glyphID.complex.code;

                    auto fontStackHash = FontStackHasher()(fontStack);
                    bool needShape = true;
                    if (glyphMap.contains(fontStackHash)) {
                        auto& glyphs = glyphMap[fontStackHash];
                        if (glyphs.contains(glyphID)) needShape = false;
                    }
                    if (needShape) {
                        auto glyph = glyphManager->getGlyph(fontStack, glyphID);
                        glyphMap[fontStackHash].emplace(glyph->id, glyph);
                    }
                }

                results[fontStack][type][str] = HBShapeResult{shapedstr,
                                                              shapedAdjusts}; //.emplace(str, shapedstr, shapedAdjusts);
            }
        }
    }
#endif // MLN_TEXT_SHAPING_HARFBUZZ
    return results;
}

void GeometryTile::getGlyphs(GlyphDependencies glyphDependencies) {
    MLN_TRACE_FUNC();
    const util::LabelSteps::Scope step(sourceID, "hop-glyphs", true);

    if (fileSource) {
        if (inInlineWorker) {
            glyphManager->getGlyphs(*inlineRequestor, std::move(glyphDependencies), *fileSource);
        } else {
            glyphManager->getGlyphs(*this, std::move(glyphDependencies), *fileSource);
        }
    }
}

void GeometryTile::onImagesAvailable(ImageMap images,
                                     ImageMap patterns,
                                     ImageVersionMap versionMap,
                                     uint64_t imageCorrelationID) {
    MLN_TRACE_FUNC();

    worker.self().invoke(&GeometryTileWorker::onImagesAvailable,
                         std::move(images),
                         std::move(patterns),
                         std::move(versionMap),
                         imageCorrelationID);
}

void GeometryTile::getImages(ImageRequestPair pair) {
    MLN_TRACE_FUNC();
    const util::LabelSteps::Scope step(sourceID, "hop-images", true);

    if (inInlineWorker) {
        imageManager->getImages(*inlineRequestor, std::move(pair));
    } else {
        imageManager->getImages(*this, std::move(pair));
    }
}

std::shared_ptr<FeatureIndex> GeometryTile::getFeatureIndex() const {
    return layoutResult ? layoutResult->featureIndex : nullptr;
}

bool GeometryTile::layerPropertiesUpdated(const Immutable<style::LayerProperties>& layerProperties) {
    MLN_TRACE_FUNC();

    LayerRenderData* renderData = getLayerRenderData(*layerProperties->baseImpl);
    if (!renderData) {
        return false;
    }

    if (renderData->layerProperties != layerProperties &&
        renderData->layerProperties->constantsMask() == layerProperties->constantsMask()) {
        renderData->layerProperties = layerProperties;
    }

    return true;
}

const GeometryTileData* GeometryTile::getData() const {
    if (!layoutResult || !layoutResult->featureIndex) {
        return nullptr;
    }
    return layoutResult->featureIndex->getData();
}

LayerRenderData* GeometryTile::getLayerRenderData(const style::Layer::Impl& layerImpl) {
    MLN_TRACE_FUNC();

    return layoutResult ? layoutResult->getLayerRenderData(layerImpl) : nullptr;
}

float GeometryTile::getQueryPadding(const std::unordered_map<std::string, const RenderLayer*>& layers) {
    MLN_TRACE_FUNC();

    float queryPadding = 0;
    for (const auto& pair : layers) {
        const LayerRenderData* data = getLayerRenderData(*pair.second->baseImpl);
        if (data && data->bucket && data->bucket->hasData()) {
            queryPadding = std::max(queryPadding, data->bucket->getQueryRadius(*pair.second));
        }
    }
    return queryPadding;
}

void GeometryTile::queryRenderedFeatures(std::unordered_map<std::string, std::vector<Feature>>& result,
                                         const GeometryCoordinates& queryGeometry,
                                         const TransformState& transformState,
                                         const std::unordered_map<std::string, const RenderLayer*>& layers,
                                         const RenderedQueryOptions& options,
                                         const GlobalStateMap* globalState,
                                         const mat4& projMatrix,
                                         const SourceFeatureState& featureState) {
    MLN_TRACE_FUNC();

    if (!getData()) return;

    const float queryPadding = getQueryPadding(layers);

    mat4 posMatrix;
    transformState.matrixFor(posMatrix, id.toUnwrapped());
    matrix::multiply(posMatrix, projMatrix, posMatrix);

    layoutResult->featureIndex->query(result,
                                      queryGeometry,
                                      transformState,
                                      posMatrix,
                                      util::tileSize_D * id.overscaleFactor(),
                                      std::pow(2, transformState.getZoom() - id.overscaledZ),
                                      options,
                                      globalState,
                                      id.toUnwrapped(),
                                      layers,
                                      queryPadding * transformState.maxPitchScaleFactor(),
                                      featureState);
}

void GeometryTile::querySourceFeatures(std::vector<Feature>& result,
                                       const SourceQueryOptions& options,
                                       const GlobalStateMap* globalState) {
    MLN_TRACE_FUNC();

    // Data not yet available, or tile is empty
    if (!getData()) {
        return;
    }

    // No source layers, specified, nothing to do
    if (!options.sourceLayers) {
        Log::Warning(Event::General, "At least one sourceLayer required");
        return;
    }

    for (const auto& sourceLayer : *options.sourceLayers) {
        // Go through all sourceLayers, if any
        // to gather all the features
        auto layer = getData()->getLayer(sourceLayer);

        if (layer) {
            auto featureCount = layer->featureCount();
            for (std::size_t i = 0; i < featureCount; i++) {
                auto feature = layer->getFeature(i);

                // Apply filter, if any
                if (options.filter && !(*options.filter)(style::expression::EvaluationContext{
                                          static_cast<float>(this->id.overscaledZ), feature.get()}
                                                             .withGlobalState(globalState))) {
                    continue;
                }

                result.emplace_back(convertFeature(*feature, id.canonical));
            }
        }
    }
}

bool GeometryTile::holdForFade() const {
    return mode == MapMode::Continuous &&
           (fadeState == FadeState::NeedsFirstPlacement || fadeState == FadeState::NeedsSecondPlacement);
}

void GeometryTile::markRenderedIdeal() {
    fadeState = FadeState::Loaded;
}
void GeometryTile::markRenderedPreviously() {
    if (fadeState == FadeState::Loaded) {
        fadeState = FadeState::NeedsFirstPlacement;
    }
}
void GeometryTile::performedFadePlacement() {
    if (fadeState == FadeState::NeedsFirstPlacement) {
        fadeState = FadeState::NeedsSecondPlacement;
    } else if (fadeState == FadeState::NeedsSecondPlacement) {
        fadeState = FadeState::CanRemove;
    }
}

void GeometryTile::setFeatureState(const LayerFeatureStates& states) {
    MLN_TRACE_FUNC();

    const auto layers = getData();
    if ((layers == nullptr) || states.empty() || !layoutResult) {
        return;
    }

    for (auto& layerIdToLayerRenderData = layoutResult->layerRenderData;
         auto& [layerID, renderData] : layerIdToLayerRenderData) {
        std::string sourceLayerId = renderData.layerProperties->baseImpl->sourceLayer;
        if (const auto sourceLayer = layers->getLayer(sourceLayerId)) {
            auto entry = states.find(sourceLayerId);
            if (entry == states.end()) {
                continue;
            }
            const auto& featureStates = entry->second;
            if (featureStates.empty()) {
                continue;
            }
            if (const auto bucket = renderData.bucket; bucket && bucket->hasData()) {
                bucket->update(featureStates, *sourceLayer, layerID, layoutResult->imageAtlas.patternPositions);
            }
        }
    }
}

} // namespace mln
