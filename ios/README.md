# iOS target

iOS uses the community fork of raylib for iOS, **`ghera/raylib-iOS`**, through a
copy under our own organisation, **`omardev29/raylib-iOS`** -- so a deleted or
rewritten upstream cannot break the build. It is the submodule
`thirdparty/raylib-ios`, pinned at tag `6.0.3-iOS` (commit `29ce933d`; see
`.gitmodules` and `thirdparty/FROZEN_VERSIONS.md`). Upstream raylib has no iOS
backend, so this fork provides `rcore_ios.c` (UIKit + EGL via ANGLE).

## How it differs from desktop

- **No blocking main loop.** UIKit owns the run loop and calls the app. The
  game's code does not change for it: `RMP_GAME(...)` in `src/main.cpp` expands
  to `RMP_IOS_FUNCS` (in `include/rmp/app.h`) on iOS, which defines the three
  callbacks the fork's `rcore_ios.c` calls -- `ios_ready`, `ios_update` and
  `ios_destroy` -- around the same scene stack every other platform runs.
- **Graphics** are OpenGL ES 3 through **ANGLE** (GLES -> Metal). The fork bundles
  prebuilt `libEGL.xcframework` / `libGLESv2.xcframework` under
  `thirdparty/raylib-ios/deps/ANGLE/`.
- **The build is Xcode's**, not CMake's. raylib is compiled into a
  `raylib.xcframework` by the fork's script -- one device slice (`ios-arm64`) and
  two simulator slices -- and an Xcode project generated from `ios/project.yml`
  links it into the app.
- **No quitting.** `rmp::app::quit()` does nothing here: Apple rejects an app
  that terminates itself. Hide a Quit button with `#if !defined(PLATFORM_IOS)`.

## What CI builds

The `ios` job in `.github/workflows/_apple.yml` runs on GitHub's hosted macOS
runner (`macos-26`, Xcode pinned in `thirdparty/FROZEN_VERSIONS.md`), so you do
not need a Mac for it. It checks out the submodule, runs
`tools/configure.py --require-notices`, builds `raylib.xcframework`, generates
the Xcode project with XcodeGen, and **builds the app itself** for the
simulator, unsigned. Then it checks the bundle -- an executable Mach-O, an
`Info.plist` with a bundle identifier, and `resources/` inside the `.app` -- and
uploads two artifacts: `ios-app-simulator` (the `.app`) and
`ios-raylib-xcframework`.

The step that would boot the app in the simulator is **switched off**: it runs
only when the repository variable `IOS_SIMULATOR_TEST` is `true`, because the
hosted simulator does not boot reliably under `simctl`. It is gated off rather
than made `continue-on-error`, on purpose -- a step that is allowed to fail
reports green whatever happens, and a disabled one says so. Nothing in CI makes
a build for a device: that needs signing, which needs credentials that do not
belong in CI.

## Building it yourself (macOS + Xcode)

```bash
# 0. The fork is a submodule, and a plain clone leaves it empty.
git submodule update --init thirdparty/raylib-ios

# 1. The .toml becomes ios/project.yml, the app icon and the iOS LICENSES.txt.
python3 tools/configure.py

# 2. raylib.xcframework, device and simulator slices in one go. Once, and again
#    only when the submodule moves.
(cd thirdparty/raylib-ios/projects/scripts && bash build-ios-xcframework.sh)

# 3. The app's Xcode project (needs XcodeGen: brew install xcodegen).
cd ios
xcodegen generate

# 4a. For the simulator, no signing needed:
xcodebuild -project ray_test.xcodeproj -scheme ray_test \
    -destination 'generic/platform=iOS Simulator' \
    CODE_SIGNING_ALLOWED=NO build

# 4b. For a device: open ray_test.xcodeproj, pick the phone and press Run --
#     or build from here with -destination 'generic/platform=iOS'.
```

`ray_test` is `[project] name` in `raylib_multiplatform.toml`; use yours. A device
build needs a signing team. Set it in the `.toml` rather than in Xcode, so it
survives the next `xcodegen generate`:

```toml
[ios.settings]
DEVELOPMENT_TEAM = "ABCDE12345"
CODE_SIGN_STYLE  = "Automatic"
```

## Caveats

- **Resources:** `RMP_RESOURCES_PATH` is `./resources/`, and an iOS process
  does not start inside its bundle. `rmp::app::detail::begin_run()` (in
  `src/rmp/app.cpp`, called by `RMP_IOS_FUNCS`) does a
  `ChangeDirectory(GetApplicationDirectory())` before anything loads, so the
  relative path resolves against the `.app`. The bundle is read-only: write
  with `rmp::save`, which puts saves in the app's `Library/Application Support/`.
- **Audio:** check miniaudio's output on a device; the simulator is not the
  phone.
