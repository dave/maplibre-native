#!/bin/bash
# copy-licenses.sh <out dir> -- copy the fork's licence files into
# <out dir>/licenses/, at their paths in the tree, for the Viatrail app's
# licence notices (Viatrail #270). The app commits them as
# vendor/maplibre/licenses/ beside the vendored framework and AAR.
#
# This folder (viatrail/) is fork-only. Nothing upstream uses it.
#
# 1. The fork's own tree: every tracked file at HEAD whose name starts
#    LICENSE, LICENCE, COPYING, NOTICE or PATENTS (any case), except under
#    test/, metrics/, scripts/ and platform/*/scripts/ (tests and build
#    tooling) and *.cmake files.
# 2. Every submodule the binaries link (all of them except NOT_LINKED below):
#    it must be checked out at the commit HEAD pins (the build needs it
#    anyway), and its tracked licence files are copied, except under test,
#    bench, example, fuzz, python, bin, contrib, docs and deps/gyp folders. Plus
#    EXTRA files the name rule misses (FreeType's FTL text). A linked
#    submodule with no licence file is an error. A new submodule is copied
#    by default: add it to NOT_LINKED only with a reason.
#
# Both build scripts call it, so a vendored binary and its licence files
# always come from the same commit.
set -euo pipefail
OUT=${1:?usage: viatrail/copy-licenses.sh <out dir>}
ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"
die() { echo "copy-licenses: $*" >&2; exit 1; }

NAME_RE='(^|/)(licen[cs]e|copying|notice|patents)[^/]*$'

# Submodules no shipped binary links, with the reason.
NOT_LINKED='
docs/doxygen/doxygen-awesome-css            documentation styling
platform/windows/vendor/vcpkg               Windows package manager
vendor/Vulkan-Headers                       Vulkan renderer (we build Metal and OpenGL)
vendor/VulkanMemoryAllocator                Vulkan renderer
vendor/glslang                              Vulkan shader compiler
vendor/webgpu-cpp                           WebGPU renderer
vendor/wgpu-native                          WebGPU renderer
vendor/args                                 command-line tools only
vendor/benchmark                            benchmarks
vendor/googletest                           tests
vendor/zip-archive                          render-test app only
vendor/cpp-httplib                          render tests only
vendor/maplibre-native-base/deps/pixelmatch-cpp  render tests only
'
EXTRA='
vendor/freetype/docs/FTL.TXT
'

copy() { mkdir -p "$OUT/licenses/$(dirname "$1")"; cp "$1" "$OUT/licenses/$1"; }

FILES=$(git ls-files | grep -iE "$NAME_RE" \
  | grep -vE '^(test|metrics|scripts)/|^platform/[^/]+/scripts/|\.cmake$' || true)
[ -n "$FILES" ] || die "no licence files found"
echo "$FILES" | grep -qx 'LICENSE.md' || die "no LICENSE.md at the root"
n=0
while IFS= read -r f; do copy "$f"; n=$((n + 1)); done <<< "$FILES"

SKIP=$(echo "$NOT_LINKED" | awk 'NF {print $1}')
SUBS=$(git ls-tree -r HEAD | awk '$2 == "commit" {print $3 " " $4}')
while read -r pin path; do
  if echo "$SKIP" | grep -qxF "$path"; then continue; fi
  [ -e "$path/.git" ] || die "submodule $path is not checked out (git submodule update --init -- $path)"
  head=$(git -C "$path" rev-parse HEAD)
  [ "$head" = "$pin" ] || die "submodule $path is at $head, HEAD pins $pin"
  sub=$(git -C "$path" ls-files | grep -iE "$NAME_RE" \
    | grep -viE '(^|/)(tests?|testdata|testing|examples?|bench|benchmarks?|fuzz|python|bin|contrib|docs?)/|(^|/)deps/gyp/' || true)
  [ -n "$sub" ] || die "submodule $path (linked) has no licence file"
  while IFS= read -r f; do copy "$path/$f"; n=$((n + 1)); done <<< "$sub"
done <<< "$SUBS"
# Every NOT_LINKED entry must still be a submodule, so the list cannot rot.
for s in $SKIP; do
  echo "$SUBS" | awk '{print $2}' | grep -qxF "$s" || die "NOT_LINKED names $s, which is not a submodule"
done

for f in $EXTRA; do
  [ -f "$f" ] || die "required licence file $f is missing"
  copy "$f"; n=$((n + 1))
done
echo "copied $n licence files to $OUT/licenses"
