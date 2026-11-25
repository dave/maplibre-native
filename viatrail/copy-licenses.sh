#!/bin/bash
# copy-licenses.sh <out dir> -- copy the fork's licence files into
# <out dir>/licenses/, at their paths in the tree, for the Viatrail app's
# licence notices (Viatrail #270). The app commits them as
# vendor/maplibre/licenses/ beside the vendored framework and AAR.
#
# This folder (viatrail/) is fork-only. Nothing upstream uses it.
#
# Copies every tracked file at HEAD whose name starts LICENSE, LICENCE,
# COPYING, NOTICE or PATENTS (any case), except under test/, metrics/,
# scripts/ and platform/*/scripts/ (tests and build tooling, not shipped)
# and *.cmake files. LICENSE.md, LICENSES.core.md and each platform's
# LICENSE.md list the vendored components the binaries contain.
#
# Both build scripts call it, so a vendored binary and its licence files
# always come from the same commit.
set -euo pipefail
OUT=${1:?usage: viatrail/copy-licenses.sh <out dir>}
ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"
die() { echo "copy-licenses: $*" >&2; exit 1; }

FILES=$(git ls-files | grep -iE '(^|/)(licen[cs]e|copying|notice|patents)[^/]*$' \
  | grep -vE '^(test|metrics|scripts)/|^platform/[^/]+/scripts/|\.cmake$' || true)
[ -n "$FILES" ] || die "no licence files found"
echo "$FILES" | grep -qx 'LICENSE.md' || die "no LICENSE.md at the root"

echo "$FILES" | while IFS= read -r f; do
  mkdir -p "$OUT/licenses/$(dirname "$f")"
  cp "$f" "$OUT/licenses/$f"
done
echo "copied $(echo "$FILES" | wc -l | tr -d ' ') licence files to $OUT/licenses"
