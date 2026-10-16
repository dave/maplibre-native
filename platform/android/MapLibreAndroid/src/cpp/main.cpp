#include "jni.hpp"
#include "jni_native.hpp"
#include <jni/jni.hpp>

#include <mln/platform/settings.hpp>

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *) {
    assert(vm != nullptr);
    // SCRATCH (A/B arm "prio"): every worker thread, the label lane's
    // included, runs at nice 10 (Android's THREAD_PRIORITY_BACKGROUND), so a
    // busy pool yields the big cores to the GL thread (nice 0). Set before any
    // map makes a thread pool.
    mln::platform::Settings::getInstance().set(mln::platform::EXPERIMENTAL_THREAD_PRIORITY_WORKER, 10.0);
    mln::android::registerNatives(vm);
    return JNI_VERSION_1_6;
}
