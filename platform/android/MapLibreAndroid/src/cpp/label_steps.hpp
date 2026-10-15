#pragma once

#include <jni/jni.hpp>

namespace mln {
namespace android {

/// The core's step recorder (`mln::util::LabelSteps`), for debug builds:
/// org.maplibre.android.maps.LabelSteps.
class LabelSteps {
public:
    static constexpr auto Name() { return "org/maplibre/android/maps/LabelSteps"; };
    static void registerNative(jni::JNIEnv&);

    static void setRecording(jni::JNIEnv&, const jni::Class<LabelSteps>&, jni::jboolean);
    /// The steps since the last call, one a line: source, step, thread, hop
    /// (0 or 1), queued, start and end in seconds, tab-separated.
    static jni::Local<jni::String> take(jni::JNIEnv&, const jni::Class<LabelSteps>&);
};

} // namespace android
} // namespace mln
