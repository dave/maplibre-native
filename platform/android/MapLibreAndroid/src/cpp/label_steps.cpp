#include "label_steps.hpp"

#include <mln/util/label_steps.hpp>

#include <sstream>

namespace mln {
namespace android {

void LabelSteps::setRecording(jni::JNIEnv&, const jni::Class<LabelSteps>&, jni::jboolean value) {
    util::LabelSteps::setRecording(value);
}

jni::Local<jni::String> LabelSteps::take(jni::JNIEnv& env, const jni::Class<LabelSteps>&) {
    std::ostringstream out;
    out.precision(9);
    for (const auto& step : util::LabelSteps::take()) {
        out << step.sourceID << '\t' << step.name << '\t' << step.thread << '\t' << (step.hop ? 1 : 0) << '\t'
            << std::fixed << step.queued.count() << '\t' << step.start.count() << '\t' << step.end.count() << '\n';
    }
    return jni::Make<jni::String>(env, out.str());
}

void LabelSteps::registerNative(jni::JNIEnv& env) {
    static auto& javaClass = jni::Class<LabelSteps>::Singleton(env);
    jni::RegisterNatives(
        env,
        *javaClass,
        jni::MakeNativeMethod<decltype(&LabelSteps::setRecording), &LabelSteps::setRecording>("nativeSetRecording"),
        jni::MakeNativeMethod<decltype(&LabelSteps::take), &LabelSteps::take>("nativeTake"));
}

} // namespace android
} // namespace mln
