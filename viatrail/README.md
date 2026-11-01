# viatrail/

Fork-only build scripts. Upstream MapLibre Native does not use this folder.

- `build-xcframework.sh <out dir>`: the iOS `MapLibre.xcframework` the Viatrail
  app vendors. Upstream's release build, then the simulator slice is thinned to
  arm64 and the dSYMs move out of the framework.
- `build-aar.sh <out dir>`: the Android OpenGL release AAR for arm64-v8a.
- `ci-runs.sh <sha> <workflow>...`: the fork's CI runs for a commit; both build
  scripts use it.

Both build scripts refuse a dirty tree and a commit whose CI is not green, and
write a `BUILD_INFO` file with the fork commit and its CI runs.
