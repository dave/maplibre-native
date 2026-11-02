#include "resource_provider.hpp"

#include "attach_env.hpp"

#include <mln/util/logging.hpp>

#include <string>
#include <utility>

namespace mln {
namespace android {

namespace {

// The loader thread is a native thread. It stays attached to the JVM from its
// first request to its end, so a request does not pay for an attach, and for the
// java.lang.Thread that each attach makes.
jni::JNIEnv& loaderThreadEnv() {
    thread_local UniqueEnv env = AttachEnv();
    return *env;
}

class JavaResourceProvider final : public mln::ResourceProvider {
public:
    using Adapter = ResourceProviders::Adapter;

    JavaResourceProvider(jni::JNIEnv& env, const jni::Object<Adapter>& adapter_)
        : adapter(jni::NewGlobal<jni::EnvAttachingDeleter>(env, adapter_)) {}

    void start(const ProviderRequest& request, std::shared_ptr<ProviderResponder> responder) override {
        auto& env = loaderThreadEnv();
        // ResourceProviders.registerNative resolved the class on a thread that can find it.
        static auto& javaClass = jni::Class<Adapter>::Singleton(env);
        static auto method = javaClass.GetMethod<void(jni::jlong,
                                                      jni::jint,
                                                      jni::String,
                                                      jni::String,
                                                      jni::jboolean,
                                                      jni::String,
                                                      jni::jint,
                                                      jni::jint,
                                                      jni::jint,
                                                      jni::jint,
                                                      jni::jboolean,
                                                      jni::jboolean,
                                                      jni::jlong)>(env, "start");
        const auto& tile = request.tile;
        // The Java peer takes this over as it is made.
        auto* held = new std::shared_ptr<ProviderResponder>(responder);
        try {
            adapter.Call(env,
                         method,
                         static_cast<jni::jlong>(request.id),
                         static_cast<jni::jint>(request.kind),
                         jni::Make<jni::String>(env, request.url),
                         jni::Make<jni::String>(env, request.sourceID),
                         static_cast<jni::jboolean>(tile.has_value()),
                         jni::Make<jni::String>(env, tile ? tile->urlTemplate : std::string()),
                         static_cast<jni::jint>(tile ? tile->pixelRatio : 0),
                         static_cast<jni::jint>(tile ? tile->x : 0),
                         static_cast<jni::jint>(tile ? tile->y : 0),
                         static_cast<jni::jint>(tile ? tile->z : 0),
                         static_cast<jni::jboolean>(request.localOnly),
                         static_cast<jni::jboolean>(request.priority == Resource::Priority::Low),
                         reinterpret_cast<jni::jlong>(held));
        } catch (const jni::PendingJavaException&) {
            // Adapter.start catches what a provider throws, so the request itself could
            // not be made. Nothing else can answer it.
            jni::ExceptionDescribe(env);
            jni::ExceptionClear(env);
            responder->error("The resource request could not be made in Java");
        }
    }

    void cancel(uint64_t id) override {
        auto& env = loaderThreadEnv();
        static auto& javaClass = jni::Class<Adapter>::Singleton(env);
        static auto method = javaClass.GetMethod<void(jni::jlong)>(env, "cancel");
        try {
            adapter.Call(env, method, static_cast<jni::jlong>(id));
        } catch (const jni::PendingJavaException&) {
            jni::ExceptionDescribe(env);
            jni::ExceptionClear(env);
            Log::Error(Event::General, "Resource provider cancel failed in Java");
        }
    }

private:
    const jni::Global<jni::Object<Adapter>, jni::EnvAttachingDeleter> adapter;
};

} // namespace

ResourceRequest::ResourceRequest(jni::JNIEnv&, jni::jlong responder_) {
    std::unique_ptr<std::shared_ptr<ProviderResponder>> held(
        reinterpret_cast<std::shared_ptr<ProviderResponder>*>(responder_));
    responder = std::move(*held);
}

void ResourceRequest::answerData(jni::JNIEnv& env, const jni::Array<jni::jbyte>& bytes) {
    auto data = std::make_shared<std::string>(bytes.Length(env), char());
    jni::GetArrayRegion(env, *bytes, 0, data->size(), reinterpret_cast<jni::jbyte*>(data->data()));
    responder->data(std::move(data));
}

void ResourceRequest::answerEmpty(jni::JNIEnv&) {
    responder->empty();
}

void ResourceRequest::answerUnavailable(jni::JNIEnv& env, const jni::String& why) {
    responder->unavailable(jni::Make<std::string>(env, why));
}

void ResourceRequest::answerNotFound(jni::JNIEnv& env, const jni::String& why) {
    responder->notFound(jni::Make<std::string>(env, why));
}

void ResourceRequest::answerError(jni::JNIEnv& env, const jni::String& why) {
    responder->error(jni::Make<std::string>(env, why));
}

void ResourceRequest::registerNative(jni::JNIEnv& env) {
    static auto& javaClass = jni::Class<ResourceRequest>::Singleton(env);

#define METHOD(MethodPtr, name) jni::MakeNativePeerMethod<decltype(MethodPtr), (MethodPtr)>(name)

    jni::RegisterNativePeer<ResourceRequest>(env,
                                             javaClass,
                                             "nativePtr",
                                             jni::MakePeer<ResourceRequest, jni::jlong>,
                                             "initialize",
                                             "finalize",
                                             METHOD(&ResourceRequest::answerData, "nativeAnswerData"),
                                             METHOD(&ResourceRequest::answerEmpty, "nativeAnswerEmpty"),
                                             METHOD(&ResourceRequest::answerUnavailable, "nativeAnswerUnavailable"),
                                             METHOD(&ResourceRequest::answerNotFound, "nativeAnswerNotFound"),
                                             METHOD(&ResourceRequest::answerError, "nativeAnswerError"));

#undef METHOD
}

void ResourceProviders::registerProvider(jni::JNIEnv& env,
                                         const jni::Class<ResourceProviders>&,
                                         const jni::String& scheme,
                                         const jni::Object<Adapter>& adapter) {
    registerResourceProvider(jni::Make<std::string>(env, scheme), std::make_shared<JavaResourceProvider>(env, adapter));
}

void ResourceProviders::unregisterProvider(jni::JNIEnv& env,
                                           const jni::Class<ResourceProviders>&,
                                           const jni::String& scheme) {
    unregisterResourceProvider(jni::Make<std::string>(env, scheme));
}

jni::Local<jni::Array<jni::jlong>> ResourceProviders::counts(jni::JNIEnv& env,
                                                             const jni::Class<ResourceProviders>&,
                                                             const jni::String& scheme) {
    const auto counts = getResourceProviderCounts(jni::Make<std::string>(env, scheme));
    if (!counts) {
        return jni::Local<jni::Array<jni::jlong>>();
    }
    auto result = jni::Array<jni::jlong>::New(env, 2);
    result.Set(env, 0, static_cast<jni::jlong>(counts->inFlight));
    result.Set(env, 1, static_cast<jni::jlong>(counts->peakInFlight));
    return result;
}

void ResourceProviders::registerNative(jni::JNIEnv& env) {
    static auto& javaClass = jni::Class<ResourceProviders>::Singleton(env);
    // Resolved here: a native thread's class loader cannot find it.
    jni::Class<Adapter>::Singleton(env);
    jni::RegisterNatives(
        env,
        *javaClass,
        jni::MakeNativeMethod<decltype(&ResourceProviders::registerProvider), &ResourceProviders::registerProvider>(
            "nativeRegister"),
        jni::MakeNativeMethod<decltype(&ResourceProviders::unregisterProvider), &ResourceProviders::unregisterProvider>(
            "nativeUnregister"),
        jni::MakeNativeMethod<decltype(&ResourceProviders::counts), &ResourceProviders::counts>("nativeCounts"));
}

} // namespace android
} // namespace mln
