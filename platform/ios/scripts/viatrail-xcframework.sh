#!/bin/bash
# viatrail-xcframework.sh — the one way to build the iOS MapLibre.xcframework
# that the Viatrail app vendors (vendor/maplibre/MapLibre.xcframework).
#
#   platform/ios/scripts/viatrail-xcframework.sh <out dir>
#
# Builds the CLEAN tree it is run in (it refuses uncommitted changes), with
# the release mode of upstream's .github/workflows/ios-release.yml:
# --compilation_mode=opt (so -O, and NDEBUG: no assert), dead_strip, thin_lto,
# binary stripping, a dSYM, the Metal renderer. Bazel's default mode is
# fastbuild (-O0, asserts on); every framework vendored until 2026-10-16 was
# built that way by mistake. Then it checks the device binary: no
# __assert_rtn import and no assert text left. It writes to <out dir>:
#   MapLibre.xcframework/   the framework to vendor
#   BUILD_INFO              the fork commit, the bazel command, Xcode's version
# Run it from another commit's tree to build that commit, e.g.
#   git worktree add /tmp/fw <sha> && (cd /tmp/fw && /path/to/this/script <out>)
set -euo pipefail
OUT=${1:?usage: viatrail-xcframework.sh <out dir>}
ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"
if [ -n "$(git status --porcelain)" ]; then
  echo "viatrail-xcframework: the tree has uncommitted changes; a vendored framework must map to a commit" >&2
  exit 1
fi
SHA=$(git rev-parse HEAD)
CMD=(bazel build --compilation_mode=opt --features=dead_strip,thin_lto --objc_enable_binary_stripping
     --apple_generate_dsym --//:renderer=metal //platform/ios:MapLibre.dynamic)
"${CMD[@]}"
ZIP="$(bazel info --compilation_mode=opt bazel-bin)/platform/ios/MapLibre.dynamic.xcframework.zip"
rm -rf "$OUT" && mkdir -p "$OUT"
(cd "$OUT" && unzip -q "$ZIP")
# The dSYMs (about 200 MB) go beside the framework, not in it: the app repo
# vendors the framework, and large files do not belong in git. Keep them with
# the build for symbolicating crash reports.
XCF="$OUT/MapLibre.xcframework"
mkdir -p "$OUT/dSYMs"
for slice in "$XCF"/*/; do
  name=$(basename "$slice")
  if [ -d "$slice/dSYMs" ]; then mv "$slice/dSYMs" "$OUT/dSYMs/$name"; fi
done
n=0
while /usr/libexec/PlistBuddy -c "Print :AvailableLibraries:$n" "$XCF/Info.plist" >/dev/null 2>&1; do
  /usr/libexec/PlistBuddy -c "Delete :AvailableLibraries:$n:DebugSymbolsPath" "$XCF/Info.plist" 2>/dev/null || true
  n=$((n+1))
done
BIN="$XCF/ios-arm64/MapLibre.framework/MapLibre"
[ -f "$BIN" ] || { echo "viatrail-xcframework: no device binary at $BIN" >&2; exit 1; }
if nm -u "$BIN" | grep -q "__assert_rtn"; then
  echo "viatrail-xcframework: the device binary imports __assert_rtn: not a release build" >&2
  exit 1
fi
if strings "$BIN" | grep -q "x >= -border"; then
  echo "viatrail-xcframework: the device binary still holds assert text: not a release build" >&2
  exit 1
fi
{
  echo "fork $SHA"
  echo "command ${CMD[*]}"
  echo "xcode $(xcodebuild -version | tr '\n' ' ')"
} > "$OUT/BUILD_INFO"
echo "built $OUT/MapLibre.xcframework from $SHA (release mode)"
