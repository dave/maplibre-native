#!/bin/bash
# build-xcframework.sh <out dir> -- build the iOS MapLibre.xcframework that the
# Viatrail app vendors (vendor/maplibre/MapLibre.xcframework).
#
# This folder (viatrail/) is fork-only. Nothing upstream uses it.
#
# The script builds the commit the tree is at, and refuses:
#   - a tree with uncommitted or untracked changes;
#   - a commit that is not on the fork, or whose linux-ci or ios-ci run is not
#     green (viatrail/ci-runs.sh);
#   - an <out dir> that already exists.
#
# The build is upstream's release build (.github/workflows/ios-release.yml):
# --compilation_mode=opt (so -O and NDEBUG), dead_strip, thin_lto, binary
# stripping, a dSYM, the Metal renderer. Bazel's default mode is fastbuild
# (-O0, asserts on): never vendor that.
#
# Then, for the app:
#   - the simulator slice is thinned to arm64 (upstream ships arm64 and
#     x86_64; every Mac that builds the app is Apple silicon, and the app's Go
#     library has no x86_64 simulator slice);
#   - the dSYMs (about 200 MB) move out of the framework to <out dir>/dSYMs,
#     because the app repo keeps the framework in git;
#   - the device binary is checked for release mode: no __assert_rtn import.
#
# It writes <out dir>/MapLibre.xcframework and <out dir>/BUILD_INFO (the fork
# commit, its CI runs, the command, Xcode's version).
#
# To build another commit: git worktree add <dir> <sha>, and run the script
# from that tree.
set -euo pipefail
OUT=${1:?usage: viatrail/build-xcframework.sh <out dir>}
ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"
die() { echo "build-xcframework: $*" >&2; exit 1; }

[ -z "$(git status --porcelain)" ] || die "the tree has uncommitted or untracked changes; a vendored framework must map to a commit"
[ ! -e "$OUT" ] || die "$OUT exists; give a new directory"
SHA=$(git rev-parse HEAD)
CI=$(viatrail/ci-runs.sh "$SHA" linux-ci ios-ci) || die "CI is not green on $SHA"

CMD=(bazel build --compilation_mode=opt --features=dead_strip,thin_lto --objc_enable_binary_stripping
     --apple_generate_dsym --//:renderer=metal //platform/ios:MapLibre.dynamic)
"${CMD[@]}"
ZIP="$(bazel info --compilation_mode=opt bazel-bin)/platform/ios/MapLibre.dynamic.xcframework.zip"
[ -f "$ZIP" ] || die "no $ZIP"
mkdir -p "$OUT"
(cd "$OUT" && unzip -q "$ZIP")
XCF="$OUT/MapLibre.xcframework"
PLIST="$XCF/Info.plist"
PB=/usr/libexec/PlistBuddy

# The simulator slice: arm64 only.
n=0
found_sim=0
while $PB -c "Print :AvailableLibraries:$n" "$PLIST" >/dev/null 2>&1; do
  variant=$($PB -c "Print :AvailableLibraries:$n:SupportedPlatformVariant" "$PLIST" 2>/dev/null || echo device)
  if [ "$variant" = simulator ]; then
    found_sim=1
    id=$($PB -c "Print :AvailableLibraries:$n:LibraryIdentifier" "$PLIST")
    bin="$XCF/$id/MapLibre.framework/MapLibre"
    [ -f "$bin" ] || die "no simulator binary at $bin"
    lipo "$bin" -verify_arch arm64 || die "the simulator binary has no arm64 slice"
    if [ "$(lipo -archs "$bin")" != arm64 ]; then
      lipo "$bin" -thin arm64 -output "$bin.arm64"
      mv "$bin.arm64" "$bin"
    fi
    [ "$(lipo -archs "$bin")" = arm64 ] || die "the simulator binary is not arm64 only"
    if [ -d "$XCF/$id/dSYMs" ]; then
      for dwarf in "$XCF/$id"/dSYMs/*.dSYM/Contents/Resources/DWARF/*; do
        if [ "$(lipo -archs "$dwarf")" != arm64 ]; then
          lipo "$dwarf" -thin arm64 -output "$dwarf.arm64"
          mv "$dwarf.arm64" "$dwarf"
        fi
      done
    fi
    $PB -c "Delete :AvailableLibraries:$n:SupportedArchitectures" "$PLIST"
    $PB -c "Add :AvailableLibraries:$n:SupportedArchitectures array" "$PLIST"
    $PB -c "Add :AvailableLibraries:$n:SupportedArchitectures:0 string arm64" "$PLIST"
    $PB -c "Set :AvailableLibraries:$n:LibraryIdentifier ios-arm64-simulator" "$PLIST"
    mv "$XCF/$id" "$XCF/ios-arm64-simulator"
  fi
  n=$((n+1))
done
[ "$found_sim" = 1 ] || die "the framework has no simulator slice"

# The dSYMs go beside the framework.
mkdir -p "$OUT/dSYMs"
n=0
while $PB -c "Print :AvailableLibraries:$n" "$PLIST" >/dev/null 2>&1; do
  id=$($PB -c "Print :AvailableLibraries:$n:LibraryIdentifier" "$PLIST")
  [ -d "$XCF/$id/dSYMs" ] || die "the $id slice has no dSYMs"
  mv "$XCF/$id/dSYMs" "$OUT/dSYMs/$id"
  $PB -c "Delete :AvailableLibraries:$n:DebugSymbolsPath" "$PLIST"
  n=$((n+1))
done

BIN="$XCF/ios-arm64/MapLibre.framework/MapLibre"
[ -f "$BIN" ] || die "no device binary at $BIN"
if nm -u "$BIN" | grep -q "__assert_rtn"; then
  die "the device binary imports __assert_rtn: not a release build"
fi
SIM="$XCF/ios-arm64-simulator/MapLibre.framework/MapLibre"
if nm -u "$SIM" | grep -q "__assert_rtn"; then
  die "the simulator binary imports __assert_rtn: not a release build"
fi

{
  echo "fork $SHA"
  echo "$CI" | sed 's/^/ci /'
  echo "command ${CMD[*]}"
  echo "script viatrail/build-xcframework.sh (simulator slice thinned to arm64, dSYMs moved out)"
  echo "xcode $(xcodebuild -version | tr '\n' ' ')"
} > "$OUT/BUILD_INFO"
echo "built $XCF from $SHA (release mode)"
