// PROBE ONLY (branch probe/android-atrace-render): atrace slices in the
// render path, for a measure build. Never vendor.
#pragma once
#include <string>
#ifdef __ANDROID__
#include <android/trace.h>
#endif

namespace mln::probe {
struct Slice {
    explicit Slice(const std::string& name) {
#ifdef __ANDROID__
        ATrace_beginSection(name.c_str());
#else
        (void)name;
#endif
    }
    ~Slice() {
#ifdef __ANDROID__
        ATrace_endSection();
#endif
    }
    Slice(const Slice&) = delete;
    Slice& operator=(const Slice&) = delete;
};
} // namespace mln::probe

#define MLN_PROBE_CAT2(a, b) a##b
#define MLN_PROBE_CAT(a, b) MLN_PROBE_CAT2(a, b)
#define MLN_PROBE_SLICE(name) const ::mln::probe::Slice MLN_PROBE_CAT(mlnProbeSlice, __LINE__)(name)
