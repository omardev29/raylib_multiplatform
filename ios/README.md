# iOS target

iOS uses the community fork **`ghera/raylib-iOS`**, pinned as the submodule
`thirdparty/raylib-ios` at tag `6.0.3-iOS` (see `thirdparty/FROZEN_VERSIONS.md`).
Upstream raylib has no iOS backend, so this fork provides `rcore_ios.c`
(UIKit + EGL via ANGLE). Its author ships raylib games on the App Store with it.

## How it differs from desktop

- **No blocking main loop.** iOS is callback-driven. The game already follows a
  Godot-style lifecycle (`_ready`/`_process`/`_exit` in `src/main.cpp`), which the
  runner maps to `ios_ready`/`ios_update`/`ios_destroy` on iOS. No game-code
  changes are needed per platform.
- **Graphics** are OpenGL ES 3 through **ANGLE** (GLES→Metal). The fork bundles
  prebuilt `libEGL.xcframework` / `libGLESv2.xcframework` under
  `thirdparty/raylib-ios/deps/ANGLE/`.
- **Build is Xcode-based**, not CMake. raylib is compiled to a
  `raylib.xcframework` with the fork's script, then an Xcode app links it.

## CI builds this automatically

The `build-ios` job in `.github/workflows/build.yaml` runs on GitHub's **hosted
macOS runners** (`macos-latest`) — you do **not** need a local Mac. It builds the
`raylib.xcframework`, generates the Xcode project with XcodeGen, and compiles the
app for the simulator (no signing). It is `continue-on-error` while the scaffold
is being tuned. A local Mac is only needed for running on a physical device or
interactive debugging/signing.

## Manual build steps (macOS + Xcode)

```bash
# 1. Build raylib.xcframework (device + simulator slices)
cd thirdparty/raylib-ios/projects/scripts
chmod +x build-ios-xcframework.sh
./build-ios-xcframework.sh

# 2. Generate the app's Xcode project (needs xcodegen: brew install xcodegen)
cd ../../../../ios
xcodegen generate

# 3. Build for the simulator (no signing needed)
xcodebuild -project ray_test.xcodeproj -scheme ray_test \
    -destination 'generic/platform=iOS Simulator' \
    CODE_SIGNING_ALLOWED=NO build
```

For a **device** build, set a development team / signing identity and use
`-destination 'generic/platform=iOS'`.

## Known caveats / TODO

- **Resources:** `RESOURCES_PATH` must resolve inside the iOS app bundle. The
  asset layer (`src/assets.cpp`) currently uses `RESOURCES_PATH`; on iOS this
  should be pointed at the bundle (see the fork's `GetIOSDocumentsPath()` in
  `IOSBridge`). Wire this before shipping.
- **Audio:** verify miniaudio output on device (the experimental upstream PR had
  audio issues; this fork uses a different path, but confirm).
- The `ios/project.yml` scaffold is best-effort and should be validated/tuned on
  a Mac. The CI `build-ios` job builds the `raylib.xcframework` to keep the fork
  compiling; building the full app is a macOS+Xcode step.
