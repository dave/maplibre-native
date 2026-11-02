#pragma once

#include <mln/storage/resource_provider.hpp>

#include <jni/jni.hpp>

#include <memory>

namespace mln {
namespace android {

/// The native peer of org.maplibre.android.storage.ResourceRequest. It keeps one
/// request's responder for as long as the Java object lives.
class ResourceRequest {
public:
    static constexpr auto Name() { return "org/maplibre/android/storage/ResourceRequest"; };
    static void registerNative(jni::JNIEnv&);

    /// Takes over the `std::shared_ptr<ProviderResponder>` at `responder`.
    ResourceRequest(jni::JNIEnv&, jni::jlong responder);

    void answerData(jni::JNIEnv&, const jni::Array<jni::jbyte>&);
    void answerEmpty(jni::JNIEnv&);
    void answerUnavailable(jni::JNIEnv&, const jni::String&);
    void answerNotFound(jni::JNIEnv&, const jni::String&);
    void answerError(jni::JNIEnv&, const jni::String&);

private:
    std::shared_ptr<ProviderResponder> responder;
};

/// org.maplibre.android.storage.ResourceProviders: the process-wide registry.
class ResourceProviders {
public:
    static constexpr auto Name() { return "org/maplibre/android/storage/ResourceProviders"; };
    static void registerNative(jni::JNIEnv&);

    /// ResourceProviders.Adapter: what the loader thread calls.
    struct Adapter {
        static constexpr auto Name() { return "org/maplibre/android/storage/ResourceProviders$Adapter"; };
    };

    static void registerProvider(jni::JNIEnv&,
                                 const jni::Class<ResourceProviders>&,
                                 const jni::String& scheme,
                                 const jni::Object<Adapter>&);
    static void unregisterProvider(jni::JNIEnv&, const jni::Class<ResourceProviders>&, const jni::String& scheme);
    /// In flight, then peak; null if the scheme has no provider.
    static jni::Local<jni::Array<jni::jlong>> counts(jni::JNIEnv&,
                                                     const jni::Class<ResourceProviders>&,
                                                     const jni::String& scheme);
};

} // namespace android
} // namespace mln
