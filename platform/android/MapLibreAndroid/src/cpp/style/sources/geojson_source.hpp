#pragma once

#include "source.hpp"
#include <mln/style/sources/geojson_source.hpp>
#include <mln/actor/optional_actor.hpp>

#include <chrono>
#include "../../geojson/geometry.hpp"
#include "../../geojson/feature.hpp"
#include "../../geojson/feature_collection.hpp"
#include "../../android_renderer_frontend.hpp"
#include <jni/jni.hpp>

namespace mln {
namespace android {

/// A converted body, or the error that stopped it: every conversion answers.
using GeoJSONDataCallback = std::function<void(style::GeoJSONData::ParseResult)>;

/// Converts a source's bodies on its parse scheduler (the label lane for a
/// `labelLane` source), making data that cuts on the source's slice scheduler.
class FeatureConverter {
public:
    FeatureConverter(Immutable<style::GeoJSONOptions> options_,
                     std::shared_ptr<Scheduler> sequencedScheduler_,
                     std::string sourceID_)
        : options(std::move(options_)),
          sequencedScheduler(std::move(sequencedScheduler_)),
          sourceID(std::move(sourceID_)) {}
    void convertJson(std::shared_ptr<std::string>, std::chrono::duration<double> queued, ActorRef<GeoJSONDataCallback>);

    template <class JNIType>
    void convertObject(std::shared_ptr<jni::Global<jni::Object<JNIType>, jni::EnvAttachingDeleter>>,
                       std::chrono::duration<double> queued,
                       ActorRef<GeoJSONDataCallback>);

private:
    Immutable<style::GeoJSONOptions> options;
    std::shared_ptr<Scheduler> sequencedScheduler;
    const std::string sourceID;
};

struct Update {
    using Converter = std::function<void(ActorRef<GeoJSONDataCallback>)>;
    Converter converterFn;

    std::unique_ptr<Actor<GeoJSONDataCallback>> callback;

    Update(Converter, std::unique_ptr<Actor<GeoJSONDataCallback>>);
};

class GeoJSONSource : public Source {
public:
    using SuperTag = Source;
    static constexpr auto Name() { return "org/maplibre/android/style/sources/GeoJsonSource"; };

    static void registerNative(jni::JNIEnv&);

    GeoJSONSource(jni::JNIEnv&, const jni::String&, const jni::Object<>&);
    GeoJSONSource(jni::JNIEnv&, mln::style::Source&, AndroidRendererFrontend*);
    ~GeoJSONSource();

private:
    // Each setter answers the data generation of the body it sets
    // (`style::GeoJSONSource`); the map reports it laid out
    // (`MapView.OnSourceLaidOutListener`).
    jni::jlong setGeoJSONString(jni::JNIEnv&, const jni::String&);
    jni::jlong setFeatureCollection(jni::JNIEnv&, const jni::Object<geojson::FeatureCollection>&);
    jni::jlong setFeature(jni::JNIEnv&, const jni::Object<geojson::Feature>&);
    jni::jlong setGeometry(jni::JNIEnv&, const jni::Object<geojson::Geometry>&);

    jni::jlong setURL(jni::JNIEnv&, const jni::String&);

    jni::Local<jni::Array<jni::Object<geojson::Feature>>> querySourceFeatures(jni::JNIEnv&,
                                                                              const jni::Array<jni::Object<>>&);

    jni::Local<jni::Array<jni::Object<geojson::Feature>>> getClusterChildren(jni::JNIEnv&,
                                                                             const jni::Object<geojson::Feature>&);
    jni::Local<jni::Array<jni::Object<geojson::Feature>>> getClusterLeaves(jni::JNIEnv&,
                                                                           const jni::Object<geojson::Feature>&,
                                                                           jni::jlong,
                                                                           jni::jlong);
    jint getClusterExpansionZoom(jni::JNIEnv&, const jni::Object<geojson::Feature>&);

    jni::Local<jni::String> getURL(jni::JNIEnv&);

    jni::Local<jni::Object<Source>> createJavaPeer(jni::JNIEnv&);
    std::unique_ptr<Update> awaitingUpdate;
    std::unique_ptr<Update> update;
    std::shared_ptr<ThreadPool> threadPool;
    std::unique_ptr<OptionalActor<FeatureConverter>> converter;

    template <class JNIType>
    jni::jlong setCollectionAsync(jni::JNIEnv&, const jni::Object<JNIType>&);

    jni::jlong setAsync(Update::Converter);

    template <class JNIType>
    void setCollectionSync(jni::JNIEnv&, const jni::Object<JNIType>&);

    jboolean isUpdateSynchronous(jni::JNIEnv&);
    void setOverrideSynchronousUpdate(jni::JNIEnv&, jni::jboolean);

}; // class GeoJSONSource

} // namespace android
} // namespace mln
