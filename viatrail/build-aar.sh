#!/bin/bash
# build-aar.sh <out dir> -- build the Android MapLibre AAR that the Viatrail app
# vendors (vendor/maplibre/maplibre-android-fork.aar).
#
# This folder (viatrail/) is fork-only. Nothing upstream uses it.
#
# The script builds the commit the tree is at, and refuses:
#   - a tree with uncommitted or untracked changes;
#   - a commit that is not on the fork, or whose linux-ci or android-ci run is
#     not green (viatrail/ci-runs.sh);
#   - an <out dir> that already exists;
#   - an NDK other than the one upstream pins (Versions.kt "ndkVersion").
#
# The build is the OpenGL release AAR for arm64-v8a, the one ABI the app ships:
#   ./gradlew :MapLibreAndroid:assembleOpenglRelease -Pmaplibre.abis=arm64-v8a
# Its native code is compiled with upstream's release flags
# (platform/android/android.cmake: -Oz to compile, -O2 and -flto to link).
#
# It writes <out dir>/maplibre-android-fork.aar and <out dir>/BUILD_INFO (the
# fork commit, its CI runs, the command, the NDK, the AAR's md5).
#
# Set ANDROID_SDK_ROOT if the SDK is not at ~/Library/Android/sdk.
#
# To build another commit: git worktree add <dir> <sha>, and run the script
# from that tree.
set -euo pipefail
OUT=${1:?usage: viatrail/build-aar.sh <out dir>}
ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"
die() { echo "build-aar: $*" >&2; exit 1; }

[ -z "$(git status --porcelain)" ] || die "the tree has uncommitted or untracked changes; a vendored AAR must map to a commit"
[ ! -e "$OUT" ] || die "$OUT exists; give a new directory"
SHA=$(git rev-parse HEAD)
CI=$(viatrail/ci-runs.sh "$SHA" linux-ci android-ci) || die "CI is not green on $SHA"

SDK=${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}
NDK_VERSION=$(sed -n 's/.*ndkVersion *= *"\([^"]*\)".*/\1/p' platform/android/buildSrc/src/main/kotlin/Versions.kt)
[ -n "$NDK_VERSION" ] || die "no ndkVersion in platform/android/buildSrc/src/main/kotlin/Versions.kt"
NDK="$SDK/ndk/$NDK_VERSION"
[ -d "$NDK" ] || die "NDK $NDK_VERSION is not installed at $NDK"

CMD=(./gradlew :MapLibreAndroid:assembleOpenglRelease -Pmaplibre.abis=arm64-v8a)
(cd platform/android && ANDROID_NDK_HOME="$NDK" "${CMD[@]}")
AAR=platform/android/MapLibreAndroid/build/outputs/aar/MapLibreAndroid-opengl-release.aar
[ -f "$AAR" ] || die "no $AAR"
# The build must not have touched the tree.
[ -z "$(git status --porcelain)" ] || die "the build changed tracked files"

mkdir -p "$OUT"
cp "$AAR" "$OUT/maplibre-android-fork.aar"
ABIS=$(unzip -Z1 "$OUT/maplibre-android-fork.aar" 'jni/*/libmaplibre.so' | cut -d/ -f2 | tr '\n' ' ')
[ "$ABIS" = "arm64-v8a " ] || die "the AAR's native ABIs are '$ABIS', not arm64-v8a only"
MD5=$(md5 -q "$OUT/maplibre-android-fork.aar")

{
  echo "fork $SHA"
  echo "$CI" | sed 's/^/ci /'
  echo "command (platform/android) ANDROID_NDK_HOME=<sdk>/ndk/$NDK_VERSION ${CMD[*]}"
  echo "script viatrail/build-aar.sh"
  echo "ndk $NDK_VERSION"
  echo "md5 $MD5"
} > "$OUT/BUILD_INFO"
echo "built $OUT/maplibre-android-fork.aar from $SHA (OpenGL release, arm64-v8a), md5 $MD5"
