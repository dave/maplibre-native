#include "geojson_source.hpp"
#include <mln/style/sources/geojson_source_impl.hpp>
#include "attach_env.hpp"

#include <mln/renderer/query.hpp>

// Java -> C++ conversion
#include "../android_conversion.hpp"
#include "../conversion/filter.hpp"
#include <mln/style/conversion/geojson.hpp>
#include <mln/style/conversion/geojson_options.hpp>
#include <mln/style/conversion_impl.hpp>

// C++ -> Java conversion
#include "../../conversion/conversion.hpp"
#include "../../conversion/collection.hpp"
#include "../../geojson/feature.hpp"
#include "../conversion/url_or_tileset.hpp"

#include <mln/util/label_steps.hpp>
#include <mln/util/monotonic_timer.hpp>

#include <string>

// GeoJSONSource uses a "coalescing" model for high frequency asynchronous data
// update calls, which in practice means, that any update that started
// processing is going to finish and the last scheduled update is going to
// finish as well. Any updates scheduled during processing can be canceled. Conversion
// from Java features to core ones is done on a worker thread and once finished,
// the ownership of the converted features is returned to the calling thread.
namespace mln {
namespace android {

// The value was a GeoJsonOptions object on the Java side, so a value of the
// wrong type is a bug in our serialization or Java-side static typing. Options
// that refuse each other (labelLane with synchronousUpdate) also fail here: an
// IllegalArgumentException to the caller.
static Immutable<style::GeoJSONOptions> convertGeoJSONOptions(jni::JNIEnv& env, const jni::Object<>& options) {
    using namespace mln::style::conversion;
    if (!options) {
        return style::GeoJSONOptions::defaultOptions();
    }
    Error error;
    std::optional<style::GeoJSONOptions> result = convert<style::GeoJSONOptions>(mln::android::Value(env, options),
                                                                                 error);
    if (!result) {
        jni::ThrowNew(env, jni::FindClass(env, "java/lang/IllegalArgumentException"), error.message.c_str());
        throw jni::PendingJavaException();
    }
    return makeMutable<style::GeoJSONOptions>(std::move(*result));
}

namespace {

// The converter runs on the source's parse scheduler (the label lane for a
// `labelLane` source) and makes data that cuts on the source's own slicer.
std::unique_ptr<OptionalActor<FeatureConverter>> makeConverter(style::GeoJSONSource& source) {
    return std::make_unique<OptionalActor<FeatureConverter>>(source.isUpdateSynchronous(),
                                                             source.getParseScheduler(),
                                                             source.impl().getOptions(),
                                                             source.getSliceScheduler(),
                                                             source.getID());
}

std::chrono::duration<double> queuedNow() {
    return util::LabelSteps::recording() ? util::MonotonicTimer::now() : std::chrono::duration<double>{0};
}

} // namespace

GeoJSONSource::GeoJSONSource(jni::JNIEnv& env, const jni::String& sourceId, const jni::Object<>& options)
    : Source(env,
             std::make_unique<mln::style::GeoJSONSource>(jni::Make<std::string>(env, sourceId),
                                                         convertGeoJSONOptions(env, options))),
      converter(makeConverter(*source.as<style::GeoJSONSource>())) {}

GeoJSONSource::GeoJSONSource(jni::JNIEnv& env, mln::style::Source& coreSource, AndroidRendererFrontend* frontend)
    : Source(env, coreSource, createJavaPeer(env), frontend),
      converter(makeConverter(*source.as<style::GeoJSONSource>())) {}

GeoJSONSource::~GeoJSONSource() = default;

jni::jlong GeoJSONSource::setGeoJSONString(jni::JNIEnv& env, const jni::String& jString) {
    std::shared_ptr<std::string> json = std::make_shared<std::string>(jni::Make<std::string>(env, jString));

    OptionalActorRef<FeatureConverter> converterRef = converter->self();
    Update::Converter converterFn = [converterRef, json](ActorRef<GeoJSONDataCallback> _callback) {
        converterRef.invoke(&FeatureConverter::convertJson, json, queuedNow(), _callback);
    };

    return setAsync(converterFn);
}

jni::jlong GeoJSONSource::setFeatureCollection(jni::JNIEnv& env,
                                               const jni::Object<geojson::FeatureCollection>& jFeatures) {
    return setCollectionAsync(env, jFeatures);
}

jni::jlong GeoJSONSource::setFeature(jni::JNIEnv& env, const jni::Object<geojson::Feature>& jFeature) {
    return setCollectionAsync(env, jFeature);
}

jni::jlong GeoJSONSource::setGeometry(jni::JNIEnv& env, const jni::Object<geojson::Geometry>& jGeometry) {
    return setCollectionAsync(env, jGeometry);
}

jni::jlong GeoJSONSource::setURL(jni::JNIEnv& env, const jni::String& url) {
    // Update the core source
    return static_cast<jni::jlong>(source.as<style::GeoJSONSource>()->setURL(jni::Make<std::string>(env, url)));
}

jni::Local<jni::String> GeoJSONSource::getURL(jni::JNIEnv& env) {
    std::optional<std::string> url = source.as<style::GeoJSONSource>()->getURL();
    return url ? jni::Make<jni::String>(env, *url) : jni::Local<jni::String>();
}

jni::Local<jni::Array<jni::Object<geojson::Feature>>> GeoJSONSource::querySourceFeatures(
    jni::JNIEnv& env, const jni::Array<jni::Object<>>& jfilter) {
    using namespace mln::android::conversion;
    using namespace mln::android::geojson;

    std::vector<mln::Feature> features;
    if (rendererFrontend) {
        features = rendererFrontend->querySourceFeatures(source.getID(), {{}, toFilter(env, jfilter)});
    }
    return Feature::convert(env, features);
}

jni::Local<jni::Array<jni::Object<geojson::Feature>>> GeoJSONSource::getClusterChildren(
    jni::JNIEnv& env, const jni::Object<geojson::Feature>& feature) {
    using namespace mln::android::conversion;
    using namespace mln::android::geojson;

    if (rendererFrontend) {
        mln::Feature _feature = Feature::convert(env, feature);
        _feature.properties["cluster_id"] = static_cast<uint64_t>(_feature.properties["cluster_id"].get<double>());
        const auto featureExtension = rendererFrontend->queryFeatureExtensions(
            source.getID(), _feature, "supercluster", "children", {});
        if (featureExtension.is<mln::FeatureCollection>()) {
            return Feature::convert(env, featureExtension.get<mln::FeatureCollection>());
        }
    }
    return jni::Array<jni::Object<Feature>>::New(env, 0);
}

jni::Local<jni::Array<jni::Object<geojson::Feature>>> GeoJSONSource::getClusterLeaves(
    jni::JNIEnv& env, const jni::Object<geojson::Feature>& feature, jni::jlong limit, jni::jlong offset) {
    using namespace mln::android::conversion;
    using namespace mln::android::geojson;

    if (rendererFrontend) {
        mln::Feature _feature = Feature::convert(env, feature);
        _feature.properties["cluster_id"] = static_cast<uint64_t>(_feature.properties["cluster_id"].get<double>());
        const std::map<std::string, mln::Value> options = {{"limit", static_cast<uint64_t>(limit)},
                                                           {"offset", static_cast<uint64_t>(offset)}};
        auto featureExtension = rendererFrontend->queryFeatureExtensions(
            source.getID(), _feature, "supercluster", "leaves", options);
        if (featureExtension.is<mln::FeatureCollection>()) {
            return Feature::convert(env, featureExtension.get<mln::FeatureCollection>());
        }
    }
    return jni::Array<jni::Object<Feature>>::New(env, 0);
    ;
}

jint GeoJSONSource::getClusterExpansionZoom(jni::JNIEnv& env, const jni::Object<geojson::Feature>& feature) {
    using namespace mln::android::conversion;
    using namespace mln::android::geojson;

    if (rendererFrontend) {
        mln::Feature _feature = Feature::convert(env, feature);
        _feature.properties["cluster_id"] = static_cast<uint64_t>(_feature.properties["cluster_id"].get<double>());
        auto featureExtension = rendererFrontend->queryFeatureExtensions(
            source.getID(), _feature, "supercluster", "expansion-zoom", {});
        if (featureExtension.is<mln::Value>()) {
            auto value = featureExtension.get<mln::Value>();
            if (value.is<uint64_t>()) {
                return value.get<uint64_t>();
            }
        }
    }
    return 0;
}

jni::Local<jni::Object<Source>> GeoJSONSource::createJavaPeer(jni::JNIEnv& env) {
    static auto& javaClass = jni::Class<GeoJSONSource>::Singleton(env);
    static auto constructor = javaClass.GetConstructor<jni::jlong>(env);
    return javaClass.New(env, constructor, reinterpret_cast<jni::jlong>(this));
}

template <class JNIType>
jni::jlong GeoJSONSource::setCollectionAsync(jni::JNIEnv& env, const jni::Object<JNIType>& jObject) {
    auto global = jni::NewGlobal<jni::EnvAttachingDeleter>(env, jObject);
    auto object = std::make_shared<decltype(global)>(std::move(global));

    OptionalActorRef<FeatureConverter> converterRef = converter->self();
    Update::Converter converterFn = [converterRef, object](ActorRef<GeoJSONDataCallback> _callback) {
        converterRef.invoke(&FeatureConverter::convertObject<JNIType>, object, queuedNow(), _callback);
    };

    return setAsync(converterFn);
}

jni::jlong GeoJSONSource::setAsync(Update::Converter converterFn) {
    // The generation is taken now, in call order; a body coalesced away before
    // it converts never lands, so its generation is never reported.
    const uint64_t generation = source.as<mln::style::GeoJSONSource>()->nextDataGeneration();
    awaitingUpdate = std::make_unique<Update>(
        std::move(converterFn),
        std::make_unique<Actor<GeoJSONDataCallback>>(
            *Scheduler::GetCurrent(), [this, generation](style::GeoJSONData::ParseResult result) {
                // conversion from Java features to core ones finished
                android::UniqueEnv _env = android::AttachEnv();

                // Update the core source, or report why the body could not be made.
                auto& core = *source.as<mln::style::GeoJSONSource>();
                if (auto* error = std::get_if<std::string>(&result)) {
                    core.setGeoJSONError(generation, *error);
                } else {
                    core.setGeoJSONData(std::get<std::shared_ptr<style::GeoJSONData>>(std::move(result)), generation);
                }

                // if there is an awaiting update, execute it, otherwise, release resources
                if (awaitingUpdate) {
                    update = std::move(awaitingUpdate);
                    update->converterFn(update->callback->self());
                } else {
                    update.reset();
                }
            }));

    // If another update is running, wait
    if (update) {
        return static_cast<jni::jlong>(generation);
    }

    // no updates are being processed, execute this one
    update = std::move(awaitingUpdate);
    update->converterFn(update->callback->self());
    return static_cast<jni::jlong>(generation);
}

jni::jlong GeoJSONSource::getDataGeneration(jni::JNIEnv&) {
    return static_cast<jni::jlong>(source.as<style::GeoJSONSource>()->getLatestGeneration());
}

jboolean GeoJSONSource::isUpdateSynchronous(jni::JNIEnv&) {
    return source.as<style::GeoJSONSource>()->isUpdateSynchronous();
}

void GeoJSONSource::setOverrideSynchronousUpdate(jni::JNIEnv& env, jni::jboolean newOverride) {
    try {
        source.as<style::GeoJSONSource>()->setOverrideSynchronousUpdate(newOverride);
    } catch (const std::logic_error& refused) {
        // A label lane source refuses a synchronous override.
        jni::ThrowNew(env, jni::FindClass(env, "java/lang/IllegalStateException"), refused.what());
    }
}

void GeoJSONSource::registerNative(jni::JNIEnv& env) {
    // Lookup the class
    static auto& javaClass = jni::Class<GeoJSONSource>::Singleton(env);

#define METHOD(MethodPtr, name) jni::MakeNativePeerMethod<decltype(MethodPtr), (MethodPtr)>(name)

    // Register the peer
    jni::RegisterNativePeer<GeoJSONSource>(
        env,
        javaClass,
        "nativePtr",
        jni::MakePeer<GeoJSONSource, const jni::String&, const jni::Object<>&>,
        "initialize",
        "finalize",
        METHOD(&GeoJSONSource::setGeoJSONString, "nativeSetGeoJsonString"),
        METHOD(&GeoJSONSource::setFeatureCollection, "nativeSetFeatureCollection"),
        METHOD(&GeoJSONSource::setFeature, "nativeSetFeature"),
        METHOD(&GeoJSONSource::setGeometry, "nativeSetGeometry"),
        METHOD(&GeoJSONSource::setURL, "nativeSetUrl"),
        METHOD(&GeoJSONSource::getURL, "nativeGetUrl"),
        METHOD(&GeoJSONSource::querySourceFeatures, "querySourceFeatures"),
        METHOD(&GeoJSONSource::getClusterChildren, "nativeGetClusterChildren"),
        METHOD(&GeoJSONSource::getClusterLeaves, "nativeGetClusterLeaves"),
        METHOD(&GeoJSONSource::getClusterExpansionZoom, "nativeGetClusterExpansionZoom"),
        METHOD(&GeoJSONSource::isUpdateSynchronous, "nativeIsUpdateSynchronous"),
        METHOD(&GeoJSONSource::getDataGeneration, "nativeGetDataGeneration"),
        METHOD(&GeoJSONSource::setOverrideSynchronousUpdate, "nativeSetOverrideSynchronousUpdate"));
}

void FeatureConverter::convertJson(std::shared_ptr<std::string> json,
                                   std::chrono::duration<double> queued,
                                   ActorRef<GeoJSONDataCallback> callback) {
    const util::LabelSteps::Scope step(sourceID, "convert", false, queued);
    // A body that does not parse answers its error: the update chain goes on,
    // and the source reports it. It used to log and never answer, which held
    // every later body of the source for ever.
    callback.invoke(&GeoJSONDataCallback::operator(),
                    style::GeoJSONData::parse(*json, sequencedScheduler, options, sourceID));
}

template <class JNIType>
void FeatureConverter::convertObject(
    std::shared_ptr<jni::Global<jni::Object<JNIType>, jni::EnvAttachingDeleter>> jObject,
    std::chrono::duration<double> queued,
    ActorRef<GeoJSONDataCallback> callback) {
    using namespace mln::android::geojson;

    const util::LabelSteps::Scope step(sourceID, "convert", false, queued);
    android::UniqueEnv _env = android::AttachEnv();
    // Convert the jni object
    auto geometry = JNIType::convert(*_env, *jObject);
    callback.invoke(
        &GeoJSONDataCallback::operator(),
        style::GeoJSONData::ParseResult(style::GeoJSONData::create(geometry, sequencedScheduler, options, sourceID)));
}

Update::Update(Converter _converterFn, std::unique_ptr<Actor<GeoJSONDataCallback>> _callback)
    : converterFn(std::move(_converterFn)),
      callback(std::move(_callback)) {}

} // namespace android
} // namespace mln
